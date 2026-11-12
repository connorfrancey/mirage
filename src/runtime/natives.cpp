#include "runtime/natives.h"

#include "runtime/inject.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cctype>
#include <cstring>
#include <mutex>
#include <utility>

namespace mirage::natives
{
	namespace
	{
		enum class Type : std::uint8_t { Void, Int, Float, Bool, Byte, Object, Name };
		constexpr int kMax = 64;			// Mod natives in a session (a slot stays with its owner and name)

		struct Entry
		{
			std::string owner, name, signature;
			std::vector<Type> args;
			Type ret = Type::Void;
			int index = 0;
			Callback callback = nullptr;	// Null: its mod is gone (arguments are still read, zero returned)
			void* user = nullptr;
			std::uint64_t calls = 0;
		};
		std::array<Entry, kMax> g_entries;
		int g_count = 0;
		std::mutex g_lock;					// Registering (any thread); the thunks read their entry on the game thread

		bool ParseType(std::string t, Type& out)
		{
			for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			if (t == "int") out = Type::Int;
			else if (t == "float") out = Type::Float;
			else if (t == "bool") out = Type::Bool;
			else if (t == "byte") out = Type::Byte;
			else if (t == "object") out = Type::Object;
			else if (t == "name") out = Type::Name;
			else if (t == "void") out = Type::Void;
			else return false;
			return true;
		}
		std::string Trim(std::string s)
		{
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
			while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
			return s;
		}
		// "float(float, int)" -> return type and argument types
		bool ParseSignature(const std::string& text, Type& ret, std::vector<Type>& args, std::string* error)
		{
			const auto open = text.find('('), close = text.rfind(')');
			const auto bad = [&](const std::string& why) {
				if (error) *error = "signature '" + text + "': " + why + " (like \"float(float, int)\"; types int, float, bool, "
									"byte, object, name (arguments), void (return))";
				return false;
			};
			if (open == std::string::npos || close == std::string::npos || close < open) return bad("no (...)");
			if (!ParseType(Trim(text.substr(0, open)), ret) || ret == Type::Name) return bad("unknown return type");
			args.clear();
			const std::string inside = Trim(text.substr(open + 1, close - open - 1));
			for (std::size_t at = 0; !inside.empty() && at <= inside.size();) {
				const auto comma = inside.find(',', at);
				Type t;
				if (!ParseType(Trim(inside.substr(at, comma == std::string::npos ? std::string::npos : comma - at)), t) || t == Type::Void)
					return bad("unknown argument type");
				args.push_back(t);
				if (comma == std::string::npos) break;
				at = comma + 1;
			}
			return true;
		}

		MirageValue ToValue(Type t, const std::uint8_t* raw)
		{
			MirageValue v{};
			std::int32_t i;
			std::memcpy(&i, raw, sizeof i);
			switch (t) {
				case Type::Int: v.type = MIRAGE_VALUE_INT; v.i = i; break;
				case Type::Float: v.type = MIRAGE_VALUE_FLOAT; std::memcpy(&v.f, raw, sizeof v.f); break;
				case Type::Bool: v.type = MIRAGE_VALUE_BOOL; v.i = i != 0; break;		// A UBOOL
				case Type::Byte: v.type = MIRAGE_VALUE_INT; v.i = raw[0]; break;
				case Type::Object: v.type = MIRAGE_VALUE_OBJECT; v.o = reinterpret_cast<MirageObject*>(static_cast<std::uintptr_t>(i)); break;
				case Type::Name: {
					Classes::FName n;
					std::memcpy(&n, raw, sizeof n);
					const std::string text = n.GetName();
					v.type = MIRAGE_VALUE_TEXT;
					std::memcpy(v.text, text.c_str(), std::min(text.size(), sizeof v.text - 1));
					break;
				}
				case Type::Void: break;
			}
			return v;
		}

		void WriteResult(Type t, const MirageValue& v, void* result)
		{
			if (!result || t == Type::Void) return;
			const auto number = [&]() -> double { return v.type == MIRAGE_VALUE_FLOAT ? v.f : v.i; };
			switch (t) {
				case Type::Int: { const auto i = static_cast<std::int32_t>(number()); std::memcpy(result, &i, 4); break; }
				case Type::Float: { const auto f = static_cast<float>(number()); std::memcpy(result, &f, 4); break; }
				case Type::Bool: { const std::uint32_t b = number() != 0 ? 1u : 0u; std::memcpy(result, &b, 4); break; }
				case Type::Byte: *static_cast<std::uint8_t*>(result) = static_cast<std::uint8_t>(number()); break;
				case Type::Object: {
					const auto o = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(v.type == MIRAGE_VALUE_OBJECT ? v.o : nullptr));
					std::memcpy(result, &o, 4);
					break;
				}
				default: break;
			}
		}

		void Run(int index, Classes::UObject* self, void* frame, void* result)
		{
			Entry& e = g_entries[static_cast<std::size_t>(index)];
			inject::FrameView view;
			if (!inject::Frame(self, frame, view)) {
				spdlog::critical("Mod natives: the frame of a call to {} wasn't recognised", e.name);
				return;
			}
			Call call;
			call.self = view.object;
			call.args.reserve(e.args.size());
			for (const Type t : e.args) {
				alignas(16) std::uint8_t raw[32] = {};		// Wider than any argument type's value
				inject::Step(view, frame, raw);
				call.args.push_back(ToValue(t, raw));
			}
			inject::SkipEndOfParameters(view);
			++e.calls;
			if (e.callback) e.callback(e.user, call);
			WriteResult(e.ret, call.result, result);
		}

		template <int I>
		void __fastcall Thunk(Classes::UObject* self, void*, void* frame, void* result) { Run(I, self, frame, result); }
		template <int... I>
		constexpr std::array<void*, sizeof...(I)> MakeThunks(std::integer_sequence<int, I...>)
		{
			return {reinterpret_cast<void*>(&Thunk<I>)...};
		}
		const std::array<void*, kMax> kThunks = MakeThunks(std::make_integer_sequence<int, kMax>{});
	}

	int Register(const std::string& owner, const std::string& name, const std::string& signature, Callback callback,
				 void* user, std::string* error)
	{
		Type ret;
		std::vector<Type> args;
		if (!ParseSignature(signature, ret, args, error)) return 0;
		if (!callback || name.empty()) {
			if (error) *error = "a native needs a name and a function";
			return 0;
		}
		std::lock_guard lock(g_lock);
		for (int i = 0; i < g_count; ++i) {
			Entry& e = g_entries[static_cast<std::size_t>(i)];
			if (e.owner != owner || e.name != name) continue;
			if (e.args != args || e.ret != ret) {
				if (error) *error = name + " was registered as " + e.signature + " this session: restart the game to change it";
				return 0;
			}
			e.callback = callback;		// Its index stays: bytecode patched with it keeps working
			e.user = user;
			return e.index;
		}
		if (g_count == kMax) {
			if (error) *error = "more than " + std::to_string(kMax) + " mod natives";
			return 0;
		}
		const int slot = inject::ClaimNativeSlot(kThunks[static_cast<std::size_t>(g_count)], error);
		if (slot < 0) return 0;
		Entry& e = g_entries[static_cast<std::size_t>(g_count++)];
		e = Entry{owner, name, signature, args, ret, slot, callback, user, 0};
		spdlog::info("Mod natives: {} registered {} {} as native {}", owner, name, signature, slot);
		return slot;
	}

	void Release(const std::string& owner)
	{
		std::lock_guard lock(g_lock);
		for (int i = 0; i < g_count; ++i)
			if (g_entries[static_cast<std::size_t>(i)].owner == owner) {
				g_entries[static_cast<std::size_t>(i)].callback = nullptr;
				g_entries[static_cast<std::size_t>(i)].user = nullptr;
			}
	}

	std::vector<Info> List()
	{
		std::lock_guard lock(g_lock);
		std::vector<Info> out;
		for (int i = 0; i < g_count; ++i) {
			const Entry& e = g_entries[static_cast<std::size_t>(i)];
			out.push_back({e.owner, e.name, e.signature, e.index, e.callback != nullptr, e.calls});
		}
		return out;
	}
}
