#include "runtime/inject.h"

#include "core/clock.h"
#include "runtime/call.h"
#include "runtime/script_patch.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace mirage::inject
{
	using namespace UnrealLib;

	namespace
	{
		using Native = void(__thiscall*)(Classes::UObject* self, void* stack, void* result);
		constexpr int kNativeCount = 0x1000;			// GNatives[EX_Max]
		constexpr std::uint32_t kFuncNative = 0x400;
		constexpr int kFunctionFlags = 0x90;
		constexpr std::uint64_t kReturnParm = 0x400;	// CPF_ReturnParm
		constexpr const char* kOwner = "mirage-inject";

		struct Injection
		{
			int id = 0;
			Classes::UStruct* target = nullptr;
			std::string function, pointText, owner;
			Point point;
			int priority = 0, order = 0;
			hooks::Callback callback = nullptr;
			void* user = nullptr;
			int places = 0;
		};

		// One place in one function's bytecode where the stub is called: the callbacks it runs, in order
		struct Entry
		{
			hooks::Callback callback;
			void* user;
		};
		struct Site
		{
			Classes::UStruct* function = nullptr;
			bool canReturn = true;
			bool warned = false;
			int returnOffset = -1;		// "ReturnValue" in the frame's locals (-1: the function returns nothing)
			std::shared_ptr<const std::vector<Entry>> entries;		// Null: retired (old bytecode may still call it)
		};

		struct Target
		{
			Classes::UStruct* function = nullptr;
			Script base;						// Its bytecode before the first injection
			bool wasPatched = false;			// By a script patch (step 8): put back with that patch's owner
			std::string patchOwner;
			std::vector<int> sites;
		};

		std::vector<Site> g_sites;				// By site id (the stub's argument); never shrinks
		std::map<Classes::UStruct*, Target> g_targets;
		std::map<int, Injection> g_injections;
		int g_nextId = 1, g_order = 0;

		// The native table and the stub's slot
		Native* g_natives = nullptr;
		int g_slot = -1;
		Native g_slotBefore = nullptr;
		bool g_tried = false;
		std::string g_why;
		std::uint8_t g_opIntConst = 0xFF, g_opEndParms = 0xFF;
		int g_nodeOffset = -1;					// FFrame::Node (then Object, Code, Locals)

		std::uint64_t g_calls = 0;
		double g_micros = 0.0;

		std::uint32_t Dword(const void* p, int at)
		{
			std::uint32_t v;
			std::memcpy(&v, static_cast<const std::uint8_t*>(p) + at, sizeof v);
			return v;
		}

		struct Image
		{
			std::uintptr_t base = 0, end = 0;
		};
		const Image& Exe()
		{
			static const Image img = [] {
				MODULEINFO m{};
				GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &m, sizeof m);
				const auto b = reinterpret_cast<std::uintptr_t>(m.lpBaseOfDll);
				return Image{b, b + m.SizeOfImage};
			}();
			return img;
		}

		std::uint8_t OpcodeOf(TokenKind kind)
		{
			const auto& kinds = scripts::Map().kinds;
			for (std::size_t i = 0; i < kinds.size(); ++i)
				if (kinds[i] == kind) return static_cast<std::uint8_t>(i);
			return 0xFF;
		}

		// ---- The stub ------------------------------------------------------------------------------------------------
		int NodeOffset(Classes::UObject* self, std::uint8_t* frame)
		{
			if (g_nodeOffset >= 0) return g_nodeOffset;
			static Classes::UClass* const functionClass = Classes::UFunction::StaticClass();
			for (int at = 0; at <= 0x30; at += 4) {
				if (Dword(frame, at + 4) != reinterpret_cast<std::uintptr_t>(self)) continue;
				auto* node = reinterpret_cast<Classes::UStruct*>(static_cast<std::uintptr_t>(Dword(frame, at)));
				if (!node || IsBadReadPtr(node, 0x60) || node->Class != functionClass) continue;
				// Code (Node + 8) must point into the function's bytecode, right where this call's argument is
				const auto code = static_cast<std::uintptr_t>(Dword(frame, at + 8));
				const auto* data = *reinterpret_cast<std::uint8_t* const*>(&node->Script);
				const std::int32_t count = Dword(&node->Script, 4);
				const auto from = reinterpret_cast<std::uintptr_t>(data);
				if (code < from || code >= from + static_cast<std::uintptr_t>(count)) continue;
				g_nodeOffset = at;
				spdlog::info("Script injection: FFrame::Node at 0x{:X}", at);
				return at;
			}
			return -1;
		}

		void __fastcall Stub(Classes::UObject* self, void*, std::uint8_t* frame, void* result)
		{
			const clk::Ticks start = clk::Now();
			std::uint32_t returnNow = 0;
			const int node = NodeOffset(self, frame);
			if (node < 0) {
				// The argument can't be skipped without the frame: the VM would read it as code. This never happens with
				// the layout every UE3 game has; stop loudly rather than corrupt the call.
				spdlog::critical("Script injection: the stub's frame wasn't recognised");
				if (result) std::memcpy(result, &returnNow, sizeof returnNow);
				return;
			}
			auto*& code = *reinterpret_cast<std::uint8_t**>(frame + node + 8);
			int site = -1;
			if (code[0] == g_opIntConst) {
				std::memcpy(&site, code + 1, sizeof site);
				code += 5;
			}
			if (code[0] == g_opEndParms) ++code;
			if (site >= 0 && site < static_cast<int>(g_sites.size())) {
				Site& s = g_sites[static_cast<std::size_t>(site)];
				const auto entries = s.entries;		// Kept alive while it runs (a callback may add or remove injections)
				if (entries) {
					hooks::Call call;
					call.object = self;
					call.function = reinterpret_cast<Classes::UFunction*>(static_cast<std::uintptr_t>(Dword(frame, node)));
					call.params = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(Dword(frame, node + 12)));
					// Inside the function its return value is a local: what `return ReturnValue;` hands back
					if (s.returnOffset >= 0 && call.params) call.result = call.params + s.returnOffset;
					for (const Entry& e : *entries) {
						e.callback(e.user, call);
						if (call.skip) break;
					}
					if (call.skip && !s.canReturn && !s.warned) {
						s.warned = true;
						spdlog::warn("Script injection: a callback in {} asked to return inside a ForEach: ignored",
									 s.function ? s.function->GetPathName() : std::string("?"));
					}
					returnNow = call.skip && s.canReturn ? 1u : 0u;
				}
			}
			if (result) std::memcpy(result, &returnNow, sizeof returnNow);	// A UBOOL (the JumpIfNot's condition)
			++g_calls;
			g_micros += clk::Micros(clk::Now() - start);
		}

		// ---- The native table ----------------------------------------------------------------------------------------
		// GNatives: the array whose entry at each native function's iNative is that function's code (UFunction::Func).
		// Found by matching a few dozen natives against the exe's data; the free slot is one still holding the most
		// common entry (execUndefined) that no function uses.
		bool FindNatives()
		{
			if (g_tried) return g_natives != nullptr;
			g_tried = true;
			g_opIntConst = OpcodeOf(TokenKind::IntConst);
			g_opEndParms = OpcodeOf(TokenKind::EndFunctionParms);
			if (g_opIntConst == 0xFF || g_opEndParms == 0xFF) {
				g_why = "the token map has no IntConst or EndFunctionParms opcode";
				return false;
			}
			Classes::UClass* functionClass = Classes::UFunction::StaticClass();
			std::vector<std::pair<int, std::uint32_t>> natives;		// iNative, Func
			std::set<int> used;
			const auto& objects = Classes::UObject::GetGlobalObjects();
			for (std::size_t i = 0; i < objects.Num(); ++i) {
				auto* o = objects.GetByIndex(i);
				if (!o || o->Class != functionClass) continue;
				auto* fn = static_cast<Classes::UFunction*>(o);
				const int index = call::NativeIndex(fn);
				if (index <= 0) continue;
				used.insert(index);
				if (natives.size() < 64 && (Dword(fn, kFunctionFlags) & kFuncNative)) {
					const auto entry = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(hooks::FunctionEntry(fn)));
					if (entry >= Exe().base && entry < Exe().end) natives.emplace_back(index, entry);
				}
			}
			if (natives.size() < 16) {
				g_why = "too few native functions with an index to find the native table (" + std::to_string(natives.size()) + ")";
				return false;
			}
			// Scan the exe's writable data for the first native's code, then check the rest at their indices
			const auto [firstIndex, firstEntry] = natives.front();
			std::uintptr_t found = 0;
			MEMORY_BASIC_INFORMATION mbi{};
			for (std::uintptr_t at = Exe().base; at < Exe().end && !found; at += mbi.RegionSize) {
				if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof mbi) || mbi.RegionSize == 0) break;
				const bool data = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY));
				if (!data) continue;
				const auto from = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
				const auto to = std::min<std::uintptr_t>(from + mbi.RegionSize, Exe().end);
				for (std::uintptr_t p = from; p + 4 <= to; p += 4) {
					if (*reinterpret_cast<const std::uint32_t*>(p) != firstEntry) continue;
					const std::uintptr_t base = p - static_cast<std::uintptr_t>(firstIndex) * 4;
					if (base < Exe().base || base + kNativeCount * 4 > Exe().end) continue;
					std::size_t matches = 0;
					for (const auto& [index, entry] : natives)
						if (!IsBadReadPtr(reinterpret_cast<void*>(base + index * 4), 4) &&
							*reinterpret_cast<const std::uint32_t*>(base + index * 4) == entry)
							++matches;
					if (matches * 4 >= natives.size() * 3) {		// A hooked native's Func is a thunk: most, not all
						found = base;
						break;
					}
				}
			}
			if (!found) {
				g_why = "the native function table (GNatives) wasn't found";
				return false;
			}
			auto* table = reinterpret_cast<Native*>(found);
			std::map<Native, int> counts;
			for (int i = 0; i < kNativeCount; ++i) ++counts[table[i]];
			const Native undefined = std::max_element(counts.begin(), counts.end(),
													  [](const auto& a, const auto& b) { return a.second < b.second; })->first;
			for (int i = kNativeCount - 1; i >= 0x100 && g_slot < 0; --i)
				if (table[i] == undefined && !used.count(i)) g_slot = i;
			if (g_slot < 0) {
				g_why = "the native function table has no free slot";
				return false;
			}
			g_natives = table;
			g_slotBefore = table[g_slot];
			DWORD old = 0;
			VirtualProtect(&table[g_slot], sizeof(Native), PAGE_READWRITE, &old);
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&table[g_slot]),
								static_cast<LONG>(reinterpret_cast<std::uintptr_t>(reinterpret_cast<void*>(&Stub))));
			spdlog::info("Script injection: native table at {:#x} ({} of {} natives matched), stub in slot {} ({} free entries)",
						 found, natives.size(), natives.size(), g_slot, counts[undefined]);
			return true;
		}

		// ---- Points ---------------------------------------------------------------------------------------------------
		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		std::string CalledName(const Token& t)
		{
			if (t.kind == TokenKind::FinalFunction && !t.object.is_null()) {
				auto* o = reinterpret_cast<Classes::UObject*>(static_cast<std::uintptr_t>(static_cast<std::uint32_t>(t.object.value)));
				return o ? o->GetName() : std::string();
			}
			if (t.kind == TokenKind::VirtualFunction || t.kind == TokenKind::GlobalFunction) {
				Classes::FName n;
				n.Index = t.name.index;
				n.Number = t.name.number;
				return n.GetName();
			}
			return {};
		}

		bool Calls(const Script& s, std::uint32_t index, const std::string& lowerName, int depth = 0)
		{
			if (index >= s.tokens.size() || depth > 64) return false;
			const Token& t = s.tokens[index];
			if (Lower(CalledName(t)) == lowerName) return true;
			for (const auto c : t.children)
				if (Calls(s, c, lowerName, depth + 1)) return true;
			return false;
		}

		bool Holds(const Script& s, std::uint32_t index, std::uint32_t wanted, int depth = 0)
		{
			if (index == wanted) return true;
			if (index >= s.tokens.size() || depth > 64) return false;
			for (const auto c : s.tokens[index].children)
				if (Holds(s, c, wanted, depth + 1)) return true;
			return false;
		}

		// The gaps `point` names: gap g is before statement g. `before`: jumps to that statement go to the injection.
		struct Gap
		{
			std::size_t gap;
			bool before;
		};
		std::vector<Gap> Resolve(const Script& s, const Point& p)
		{
			std::vector<Gap> out;
			const auto& st = s.statements;
			switch (p.kind) {
				case Point::Kind::Head:
					if (!st.empty()) out.push_back({0, true});
					break;
				case Point::Kind::Return:
					for (std::size_t i = 0; i < st.size(); ++i) {
						const TokenKind k = s.tokens[st[i]].kind;
						if (k == TokenKind::Return || k == TokenKind::ReturnNothing) out.push_back({i, true});
					}
					break;
				case Point::Kind::BeforeCall:
				case Point::Kind::AfterCall: {
					const std::string name = Lower(p.function);
					int seen = 0;
					for (std::size_t i = 0; i < st.size(); ++i) {
						if (!Calls(s, st[i], name)) continue;
						++seen;
						if (p.occurrence && seen != p.occurrence) continue;
						if (p.kind == Point::Kind::BeforeCall) out.push_back({i, true});
						else out.push_back({i + 1, false});
					}
					break;
				}
				case Point::Kind::Token:
					for (std::size_t i = 0; i < st.size(); ++i)
						if (p.token >= 0 && Holds(s, st[i], static_cast<std::uint32_t>(p.token))) {
							out.push_back({i, true});
							break;
						}
					break;
			}
			return out;
		}

		// A statement inside a ForEach (an array one too): a return there would leave the iterator on the VM's stack
		bool InsideIterator(const Script& s, std::uint32_t memoryOffset)
		{
			for (const auto i : s.statements) {
				const Token& t = s.tokens[i];
				if ((t.kind == TokenKind::Iterator || t.kind == TokenKind::DynamicArrayIterator) && t.memory_offset < memoryOffset &&
					memoryOffset < t.word)
					return true;
			}
			return false;
		}

		std::uint32_t AddToken(Script& s, Token t)
		{
			if (t.kind != TokenKind::NativeFunction) t.opcode = OpcodeOf(t.kind);
			s.tokens.push_back(std::move(t));
			return static_cast<std::uint32_t>(s.tokens.size() - 1);
		}

		// `return ReturnValue;` (or `return;`): what the callbacks left in the return value goes back to the caller
		Classes::UProperty* ReturnProperty(Classes::UStruct* function)
		{
			for (Classes::UField* f = function->Children; f; f = f->Next)
				if (f->IsA(Classes::UProperty::StaticClass()) && (static_cast<Classes::UProperty*>(f)->PropertyFlags & kReturnParm))
					return static_cast<Classes::UProperty*>(f);
			return nullptr;
		}

		std::uint32_t ReturnToken(Script& s, Classes::UStruct* function)
		{
			Token value;
			value.kind = TokenKind::Nothing;
			for (Classes::UField* f = function->Children; f; f = f->Next) {
				if (!f->IsA(Classes::UProperty::StaticClass())) continue;
				auto* prop = static_cast<Classes::UProperty*>(f);
				if (!(prop->PropertyFlags & kReturnParm)) continue;
				value.kind = TokenKind::LocalVariable;
				value.object.value = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(prop));
				if (prop->IsA(Classes::UBoolProperty::StaticClass())) {		// A bool is read through BoolVariable
					Token b;
					b.kind = TokenKind::BoolVariable;
					b.children = {AddToken(s, value)};
					value = b;
				}
				break;
			}
			Token r;
			r.kind = TokenKind::Return;
			r.children = {AddToken(s, value)};
			return AddToken(s, r);
		}

		void Retire(Target& t)
		{
			for (const int id : t.sites) g_sites[static_cast<std::size_t>(id)].entries.reset();
			t.sites.clear();
		}

		// The function's bytecode again from its base with every injection it has now. False and `error`: nothing
		// changed (the previous bytecode stays).
		bool Rebuild(Target& t, std::string* error)
		{
			std::vector<Injection*> mine;
			for (auto& [id, inj] : g_injections)
				if (inj.target == t.function) mine.push_back(&inj);
			std::sort(mine.begin(), mine.end(), [](const Injection* a, const Injection* b) {
				return a->priority != b->priority ? a->priority > b->priority : a->order < b->order;
			});

			const std::string path = t.function->GetPathName();
			if (mine.empty()) {		// Back to the base: the original when nothing else had patched it
				Retire(t);
				if (!t.wasPatched) scripts::RestoreRaw(t.function);
				else if (!scripts::ApplyRaw(t.function, t.base, t.patchOwner, error)) return false;
				spdlog::info("Script injection: {} has no injections left: its bytecode is back", path);
				return true;
			}

			Script s = t.base;
			const std::size_t baseTokens = s.tokens.size();
			struct Place
			{
				bool before = false;
				std::vector<Entry> entries;
			};
			std::map<std::size_t, Place> places;
			for (Injection* inj : mine) {
				inj->places = 0;
				for (const Gap& g : Resolve(t.base, inj->point)) {
					if (g.gap >= s.statements.size()) continue;		// After the last statement: nothing runs there
					const TokenKind k = s.tokens[s.statements[g.gap]].kind;
					if (k == TokenKind::Case || k == TokenKind::LabelTable) continue;
					Place& place = places[g.gap];
					place.before = place.before || g.before;
					place.entries.push_back({inj->callback, inj->user});
					++inj->places;
				}
			}
			for (Injection* inj : mine)
				if (inj->places == 0) {
					if (error) *error = "the point '" + inj->pointText + "' matches nothing in " + path;
					return false;
				}

			std::vector<int> sites;
			for (auto it = places.rbegin(); it != places.rend(); ++it) {	// Last first: the gaps before stay valid
				const std::size_t gap = it->first;
				const std::uint32_t at = s.statements[gap];
				const std::uint32_t offset = s.tokens[at].memory_offset;
				const bool canReturn = !InsideIterator(t.base, offset);
				const int site = static_cast<int>(g_sites.size());
				Classes::UProperty* rv = ReturnProperty(t.function);
				g_sites.push_back({t.function, canReturn, false, rv ? rv->Offset : -1,
								   std::make_shared<const std::vector<Entry>>(it->second.entries)});
				sites.push_back(site);

				Token arg;
				arg.kind = TokenKind::IntConst;
				arg.int_value = site;
				Token stub;
				stub.kind = TokenKind::NativeFunction;
				stub.native_index = static_cast<std::uint16_t>(g_slot);
				stub.children = {AddToken(s, arg)};
				const std::uint32_t call = AddToken(s, stub);

				std::vector<std::uint32_t> inserted;
				if (canReturn) {
					Token jump;
					jump.kind = TokenKind::JumpIfNot;
					jump.target = static_cast<std::int32_t>(at);		// Not asked to return: on to the statement
					jump.children = {call};
					inserted = {AddToken(s, jump), ReturnToken(s, t.function)};
				} else {
					inserted = {call};
				}
				if (it->second.before)		// Jumps that went to the statement run the injection first
					for (std::size_t i = 0; i < baseTokens; ++i) {
						Token& k = s.tokens[i];
						if ((k.kind == TokenKind::Jump || k.kind == TokenKind::JumpIfNot || k.kind == TokenKind::Iterator ||
							 k.kind == TokenKind::DynamicArrayIterator) &&
							k.target < 0 && k.word == offset)
							k.target = static_cast<std::int32_t>(inserted.front());
					}
				s.statements.insert(s.statements.begin() + static_cast<std::ptrdiff_t>(gap), inserted.begin(), inserted.end());
			}

			if (!scripts::ApplyRaw(t.function, s, kOwner, error)) {
				for (const int id : sites) g_sites[static_cast<std::size_t>(id)].entries.reset();
				return false;
			}
			Retire(t);
			t.sites = std::move(sites);
			spdlog::info("Script injection: {}: {} injection(s) at {} place(s)", path, mine.size(), places.size());
			return true;
		}
	}

	namespace
	{
		// The layer over script patches (script_patch.h): a patch of a function with injections goes under them
		bool LayerHolds(Classes::UStruct* f) { return g_targets.count(f) != 0; }
		void LayerBase(Classes::UStruct* f, Script& out) { out = g_targets.at(f).base; }
		bool LayerRebase(Classes::UStruct* f, const Script* base, const std::string& owner, std::string* error)
		{
			Target& t = g_targets.at(f);
			Script next;
			if (base) next = *base;
			else if (!scripts::ReadOriginal(f, next, error)) return false;
			Script previous = std::move(t.base);
			const bool wasPatched = t.wasPatched;
			const std::string patchOwner = t.patchOwner;
			t.base = std::move(next);
			t.wasPatched = base != nullptr;
			t.patchOwner = owner;
			if (Rebuild(t, error)) return true;
			t.base = std::move(previous);		// The injections don't fit the new code: it stays as it was
			t.wasPatched = wasPatched;
			t.patchOwner = patchOwner;
			return false;
		}
		const scripts::Layer kLayer{kOwner, &LayerHolds, &LayerBase, &LayerRebase};
		void InstallLayer()
		{
			static const bool once = (scripts::SetLayer(&kLayer), true);
			(void)once;
		}
	}

	bool ParsePoint(const std::string& text, Point& out, std::string* error)
	{
		out = {};
		const auto colon = text.find(':');
		const std::string head = Lower(text.substr(0, colon));
		const std::string rest = colon == std::string::npos ? std::string() : text.substr(colon + 1);
		if (head == "head" && rest.empty()) out.kind = Point::Kind::Head;
		else if (head == "return" && rest.empty()) out.kind = Point::Kind::Return;
		else if ((head == "before" || head == "after") && !rest.empty()) {
			out.kind = head == "before" ? Point::Kind::BeforeCall : Point::Kind::AfterCall;
			const auto hash = rest.find('#');
			out.function = rest.substr(0, hash);
			if (hash != std::string::npos) {
				try {
					out.occurrence = std::stoi(rest.substr(hash + 1));
				} catch (...) {
					out.occurrence = -1;
				}
				if (out.occurrence < 1) {
					if (error) *error = "'" + text + "': #n must be 1 or more";
					return false;
				}
			}
			if (out.function.empty()) {
				if (error) *error = "'" + text + "' names no function";
				return false;
			}
		} else if (head == "token" && !rest.empty()) {
			out.kind = Point::Kind::Token;
			try {
				out.token = std::stoi(rest);
			} catch (...) {
				out.token = -1;
			}
			if (out.token < 0) {
				if (error) *error = "'" + text + "': token:<index>";
				return false;
			}
		} else {
			if (error) *error = "'" + text + "' isn't a point (head, return, before:<function>[#n], after:<function>[#n], token:<i>)";
			return false;
		}
		return true;
	}

	int Add(const std::string& function, const std::string& pointText, int priority, hooks::Callback callback, void* user,
			const std::string& owner, std::string* error)
	{
		Point point;
		if (!callback) {
			if (error) *error = "no callback";
			return 0;
		}
		if (!ParsePoint(pointText, point, error)) return 0;
		if (!FindNatives()) {
			if (error) *error = g_why;
			return 0;
		}
		InstallLayer();
		Classes::UStruct* fn = scripts::Find(function);
		if (!fn || fn->Class != Classes::UFunction::StaticClass()) {
			if (error) *error = function + " isn't a loaded function with script";
			return 0;
		}
		// The base is read before the function is registered here: once it is, Read answers with the base itself
		Script base;
		const bool first = !g_targets.count(fn);
		if (first && !scripts::Read(fn, base, error)) return 0;
		Target& t = g_targets[fn];
		if (first) {
			t.function = fn;
			t.wasPatched = scripts::Patched(fn);
			t.patchOwner = scripts::Owner(fn);
			t.base = std::move(base);
		}
		const int id = g_nextId++;
		Injection& inj = g_injections[id];
		inj.id = id;
		inj.target = fn;
		inj.function = function;
		inj.pointText = pointText;
		inj.point = point;
		inj.owner = owner;
		inj.priority = priority;
		inj.order = g_order++;
		inj.callback = callback;
		inj.user = user;
		if (!Rebuild(t, error)) {
			g_injections.erase(id);
			if (t.sites.empty() && first) g_targets.erase(fn);
			return 0;
		}
		spdlog::info("Script injection: {} into {} at {} (priority {}, {} place(s))", owner, function, pointText, priority,
					 g_injections[id].places);
		return id;
	}

	void Remove(int id)
	{
		const auto it = g_injections.find(id);
		if (it == g_injections.end()) return;
		Classes::UStruct* fn = it->second.target;
		g_injections.erase(it);
		const auto t = g_targets.find(fn);
		if (t == g_targets.end()) return;
		std::string error;
		if (!Rebuild(t->second, &error)) spdlog::error("Script injection: removing #{}: {}", id, error);
		if (t->second.sites.empty()) g_targets.erase(t);
	}

	void RemoveAll()
	{
		g_injections.clear();
		for (auto& [fn, t] : g_targets) {
			std::string error;
			if (!Rebuild(t, &error)) spdlog::error("Script injection: {}: {}", fn->GetPathName(), error);
		}
		g_targets.clear();
		scripts::SetLayer(nullptr);
		if (g_natives && g_slot >= 0) {
			InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_natives[g_slot]),
								static_cast<LONG>(reinterpret_cast<std::uintptr_t>(g_slotBefore)));
			g_slot = -1;
			g_natives = nullptr;
		}
	}

	bool Ready(std::string* why)
	{
		const bool ok = FindNatives();
		if (!ok && why) *why = g_why;
		return ok;
	}

	std::vector<Info> List()
	{
		std::vector<Info> out;
		for (const auto& [id, inj] : g_injections) out.push_back({id, inj.function, inj.pointText, inj.owner, inj.priority, inj.places});
		return out;
	}

	Stats GetStats() { return {g_calls, g_micros, g_slot}; }
}
