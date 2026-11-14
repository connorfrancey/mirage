#include "features/editor/editor_core.h"

#include "game/game.h"
#include "game/object_query.h"
#include "render/math.h"
#include "runtime/call.h"
#include "runtime/changes.h"
#include "runtime/mod_game.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"
#include "util/float_util.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

#ifdef GetObject
#undef GetObject
#endif

namespace editor
{
	namespace
	{
		namespace reflect = mirage::reflect;
		namespace ga = mirage::mods::game_api;
		using mirage::layout::Kind;

		constexpr const char* kChangeOwner = "editor";		// The change log's "mod" for the editors' writes
		constexpr std::uint64_t kPropertyEdit = 0x1;			// CPF_Edit: var()
		constexpr int kMaxElements = 64;

		std::mutex g_lock;
		MirageHandle g_selected{};
		std::atomic<std::uint64_t> g_serial{0};
		std::string g_label;
		std::vector<int> g_undo;							// The editors' transactions, newest last

		MirageObject* Handle(Classes::UObject* o) { return reinterpret_cast<MirageObject*>(o); }

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		std::string FloatText(float f)
		{
			char buf[48];
			std::snprintf(buf, sizeof buf, "%.6g", f);
			return buf;
		}

		const char* KindText(Kind k)
		{
			switch (k) {
				case Kind::Float: return "float";
				case Kind::Int: return "int";
				case Kind::Byte: return "byte";
				case Kind::Bool: return "bool";
				case Kind::Name: return "name";
				case Kind::Str: return "string";
				case Kind::Object: return "object";
				case Kind::Class: return "class";
				case Kind::Component: return "component";
				case Kind::Interface: return "interface";
				case Kind::Struct: return "struct";
				case Kind::Array: return "array";
				case Kind::Delegate: return "delegate";
				case Kind::Map: return "map";
				case Kind::Pointer: return "pointer";
				default: return "?";
			}
		}

		bool Editable(Kind k) { return k == Kind::Float || k == Kind::Int || k == Kind::Byte || k == Kind::Bool || k == Kind::Name; }

		// The row of a scalar at `path` (its kind `kind`), read from the object
		void Fill(Row& r, Classes::UObject* o, Kind kind)
		{
			r.kind = KindText(kind);
			r.editable = Editable(kind);
			const reflect::Value v = reflect::Resolve(o->Class, o, r.path);
			if (!v) {
				r.value = "?";
				return;
			}
			switch (kind) {
				case Kind::Float: r.value = FloatText(reflect::GetFloat(v)); break;
				case Kind::Int: case Kind::Byte: r.value = std::to_string(reflect::GetInt(v)); break;
				case Kind::Bool: r.value = reflect::GetInt(v) ? "true" : "false"; break;
				case Kind::Name: case Kind::Str: r.value = reflect::GetText(v); break;
				case Kind::Object: case Kind::Class: case Kind::Component: case Kind::Interface: {
					Classes::UObject* x = reflect::GetObj(v);
					r.ref = x;
					r.value = x ? reflect::NameOf(x) + " (" + reflect::ClassNameOf(x) + ")" : "None";
					break;
				}
				default: r.value = "(" + r.kind + ")"; break;
			}
		}

		void Members(std::vector<Row>& out, Classes::UObject* o, Classes::UStruct* type, const std::string& prefix, int depth,
					 const std::string& filter, const std::set<std::string>& expanded);

		// One member (and what it opens to, when expanded)
		void Member(std::vector<Row>& out, Classes::UObject* o, const reflect::Member& m, const std::string& path,
					const std::string& name, int depth, const std::set<std::string>& expanded)
		{
			Row r;
			r.path = path;
			r.name = name;
			r.depth = depth;
			r.editorOnly = (m.flags & kPropertyEdit) != 0;
			if (m.arrayDim > 1 && name.find('[') == std::string::npos) {		// A static array: its elements
				r.kind = std::string(KindText(m.kind)) + "[" + std::to_string(m.arrayDim) + "]";
				r.expandable = true;
				r.value = std::to_string(m.arrayDim) + " elements";
				out.push_back(r);
				if (expanded.count(path))
					for (std::uint32_t i = 0; i < m.arrayDim && i < static_cast<std::uint32_t>(kMaxElements); ++i)
						Member(out, o, m, path + "[" + std::to_string(i) + "]", "[" + std::to_string(i) + "]", depth + 1, expanded);
				return;
			}
			if (m.kind == Kind::Struct) {
				r.kind = m.structType ? m.structType->GetName() : "struct";
				r.expandable = true;
				// Vectors and rotators show their values on the row itself
				if (r.kind == "Vector" || r.kind == "Rotator") {
					const bool vec = r.kind == "Vector";
					const char* parts[3] = {vec ? ".X" : ".Pitch", vec ? ".Y" : ".Yaw", vec ? ".Z" : ".Roll"};
					for (int i = 0; i < 3; ++i) {
						const reflect::Value v = reflect::Resolve(o->Class, o, path + parts[i]);
						r.value += (i ? ", " : "") + (vec ? FloatText(reflect::GetFloat(v)) : std::to_string(reflect::GetInt(v)));
					}
				} else {
					r.value = "{...}";
				}
				out.push_back(r);
				if (expanded.count(path) && m.structType) Members(out, o, m.structType, path, depth + 1, "", expanded);
				return;
			}
			if (m.kind == Kind::Array) {
				const int count = reflect::ArrayCount(reflect::Resolve(o->Class, o, path));
				r.kind = "array";
				if (m.inner) r.kind = std::string(KindText(m.inner->kind)) + " array";
				r.expandable = count > 0;
				r.value = std::to_string(count) + " element" + (count == 1 ? "" : "s");
				out.push_back(r);
				if (expanded.count(path) && m.inner)
					for (int i = 0; i < count && i < kMaxElements; ++i) {
						const std::string ep = path + "[" + std::to_string(i) + "]";
						Member(out, o, *m.inner, ep, "[" + std::to_string(i) + "]", depth + 1, expanded);
					}
				return;
			}
			Fill(r, o, m.kind);
			out.push_back(r);
		}

		void Members(std::vector<Row>& out, Classes::UObject* o, Classes::UStruct* type, const std::string& prefix, int depth,
					 const std::string& filter, const std::set<std::string>& expanded)
		{
			std::vector<const reflect::Member*> list = reflect::List(type);
			std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) { return a->name < b->name; });
			for (const reflect::Member* m : list) {
				if (!filter.empty() && Lower(m->name).find(filter) == std::string::npos) continue;
				Member(out, o, *m, prefix.empty() ? m->name : prefix + "." + m->name, m->name, depth, expanded);
			}
		}
	}

	void Select(Classes::UObject* object)
	{
		MirageHandle h{};
		h.size = sizeof h;
		if (object && !ga::ObjectHandle(Handle(object), &h)) h = MirageHandle{};
		std::string label = object && h.object ? reflect::NameOf(object) + " (" + reflect::ClassNameOf(object) + ")" : std::string();
		std::lock_guard lock(g_lock);
		g_selected = h;
		g_label = std::move(label);
		++g_serial;
	}

	Classes::UObject* Selected()
	{
		MirageHandle h;
		{
			std::lock_guard lock(g_lock);
			h = g_selected;
		}
		return h.object ? reinterpret_cast<Classes::UObject*>(ga::HandleObject(&h)) : nullptr;
	}

	std::uint64_t SelectionSerial() { return g_serial.load(); }

	std::string SelectedLabel()
	{
		std::lock_guard lock(g_lock);
		return g_label;
	}

	std::vector<Row> Rows(Classes::UObject* object, const std::string& filter, const std::set<std::string>& expanded)
	{
		std::vector<Row> out;
		if (!object || !object->Class) return out;
		Members(out, object, object->Class, "", 0, Lower(filter), expanded);
		return out;
	}

	bool SetValue(Classes::UObject* object, const std::string& path, const std::string& text, std::string* error)
	{
		const auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return false;
		};
		if (!object) return fail("nothing selected");
		const reflect::Value v = reflect::Resolve(object->Class, object, path);
		if (!v) return fail(path + ": " + v.error);
		MirageValue value{};
		switch (v.member->kind) {
			case Kind::Float: {
				const auto f = util::ParseFloat(text);
				if (!f) return fail("not a number: " + text);
				value.type = MIRAGE_VALUE_FLOAT;
				value.f = *f;
				break;
			}
			case Kind::Int: case Kind::Byte: {
				const auto f = util::ParseFloat(text);
				if (!f) return fail("not a number: " + text);
				value.type = MIRAGE_VALUE_INT;
				value.i = static_cast<int32_t>(std::lround(*f));
				break;
			}
			case Kind::Bool: {
				const std::string t = Lower(text);
				value.type = MIRAGE_VALUE_BOOL;
				value.i = t == "true" || t == "1" || t == "yes";
				break;
			}
			case Kind::Name:
				value.type = MIRAGE_VALUE_TEXT;
				std::snprintf(value.text, sizeof value.text, "%s", text.c_str());
				break;
			default: return fail(path + " is a " + KindText(v.member->kind) + ": not editable here");
		}
		const int id = mirage::changes::Begin(kChangeOwner, reflect::NameOf(object) + "." + path + " = " + text);
		const int32_t ok = ga::ObjectSet(Handle(object), path.c_str(), &value);
		mirage::changes::End();
		if (!ok) {
			if (id) mirage::changes::Revert(kChangeOwner, id);
			return fail("the engine didn't take it (a name it doesn't know?)");
		}
		if (id) {
			std::lock_guard lock(g_lock);
			g_undo.push_back(id);
		}
		return true;
	}

	bool Undo()
	{
		int id = 0;
		{
			std::lock_guard lock(g_lock);
			if (g_undo.empty()) return false;
			id = g_undo.back();
			g_undo.pop_back();
		}
		mirage::changes::Revert(kChangeOwner, id);
		return true;
	}

	std::size_t UndoDepth()
	{
		std::lock_guard lock(g_lock);
		return g_undo.size();
	}

	std::vector<ActorRow> Actors()
	{
		// Names are made once: an object's name (an FName: index and number) and a class's never change, and building
		// thousands of them from the name table every refresh cost milliseconds
		static std::unordered_map<std::uint64_t, std::string> names;
		static std::unordered_map<const Classes::UClass*, std::string> classNames;
		if (names.size() > 50000) names.clear();
		std::vector<ActorRow> out;
		const Classes::APawn* pawn = game::Pawn();
		const auto& inPlay = game::ActorsInPlay();
		out.reserve(inPlay.size());
		for (Classes::AActor* a : inPlay) {
			if (!a || a->bDeleteMe) continue;
			ActorRow r;
			r.actor = a;
			const std::uint64_t key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(a->Name.Index)) << 32 |
									  static_cast<std::uint32_t>(a->Name.Number);
			auto n = names.find(key);
			if (n == names.end()) n = names.emplace(key, reflect::NameOf(a)).first;
			r.name = n->second;
			auto c = classNames.find(a->Class);
			if (c == classNames.end()) c = classNames.emplace(a->Class, reflect::ClassNameOf(a)).first;
			r.cls = c->second;
			if (pawn) {
				const float dx = a->Location.X - pawn->Location.X, dy = a->Location.Y - pawn->Location.Y, dz = a->Location.Z - pawn->Location.Z;
				r.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			out.push_back(std::move(r));
		}
		std::sort(out.begin(), out.end(), [](const ActorRow& x, const ActorRow& y) { return x.distance < y.distance; });
		return out;
	}

	namespace
	{
		std::atomic<std::uint64_t> g_frames[2];
	}
	void NoteFrame(Window w) { ++g_frames[static_cast<int>(w)]; }
	std::uint64_t Frames(Window w) { return g_frames[static_cast<int>(w)].load(); }

	Classes::UObject* LookedAt()
	{
		const rmath::Camera c = game::GetCamera();
		auto* pawn = game::Pawn();
		if (!c.valid || !pawn) return nullptr;
		constexpr float kTurn = 6.2831853f / 65536.f;
		const float pitch = static_cast<float>(c.pitch) * kTurn, yaw = static_cast<float>(c.yaw) * kTurn;
		const Classes::FVector start{c.pos.x, c.pos.y, c.pos.z};
		const Classes::FVector end{start.X + std::cos(pitch) * std::cos(yaw) * 10000.f, start.Y + std::cos(pitch) * std::sin(yaw) * 10000.f,
								   start.Z + std::sin(pitch) * 10000.f};
		mirage::call::Params trace(pawn, "Trace");
		if (!trace.Valid() || !trace.SetRaw("TraceEnd", &end, sizeof end) || !trace.SetRaw("TraceStart", &start, sizeof start) ||
			!trace.Set("bTraceActors", true) || !mirage::call::Call(pawn, trace))
			return nullptr;
		return trace.GetObject("ReturnValue");
	}
}
