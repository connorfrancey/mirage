// The Object Inspector: the engine's live objects (GObjects) found by class, path or name, and one object at a time
// with its outer chain, its class chain and every property's value, read through the engine's own UProperty objects
// (runtime/reflect). Object references, outers and classes are links: a click inspects that object.
//
// Its Classes tab is the class explorer: every loaded UClass as a tree by what it extends (or a flat list while
// searching), and one class at a time with its base classes, its direct subclasses, the properties it declares (with
// their values in its class default object), its functions and states, and how many instances are alive.
//
// The search and the selected object's rows are read on the game thread (OnTick, while the window is open) and drawn
// on the render thread from a copy under the lock. An object is held as its pointer and its GObjects slot, and trusted
// only while the slot still holds that pointer (as game::ObjectRef does), so a destroyed object is never read.

#include "core/module.h"
#include "game/game.h"
#include "game/object_query.h"
#include "game/object_ref.h"
#include "gui.h"
#include "runtime/layout.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"
#include "theme.h"

#include <fmt/format.h>
#include <IconsFontAwesome6.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	namespace reflect = mirage::reflect;
	using mirage::layout::Kind;

	constexpr std::size_t kMaxResults = 5000;	// A search stops there (GObjects holds ~100k objects)
	constexpr int kMaxElements = 256;			// Elements of one array shown when it is opened
	constexpr int kMaxDepth = 8;				// Structs and arrays opened inside each other

	// An object as the inspector keeps it: alive while GObjects[index] is still this pointer
	struct Handle
	{
		Classes::UObject* object = nullptr;
		std::int32_t index = -1;
		bool operator==(const Handle&) const = default;
	};

	// The object's GObjects slot, read under SEH: a reference in a property may point at freed memory
	std::int32_t SlotOf(const Classes::UObject* object)
	{
		__try {
			return object->ObjectInternalInteger;
		} __except (1) {
			return -1;
		}
	}

	bool Alive(const Handle& h)
	{
		auto& objects = Classes::UObject::GetGlobalObjects();
		return h.object && h.index >= 0 && static_cast<std::size_t>(h.index) < objects.Num() && objects.GetByIndex(h.index) == h.object;
	}

	// A handle for an object pointer found in memory (a property, an outer): empty unless it is a live object
	Handle HandleOf(Classes::UObject* object)
	{
		if (!object) return {};
		const Handle h{object, SlotOf(object)};
		return Alive(h) ? h : Handle{};
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	struct Result
	{
		Handle handle;
		std::string name;	// "Class Package.Outer.Name"
	};

	// One line of the property tree
	struct Row
	{
		int depth = 0;
		std::string key;		// The path from the object ("Location.X", "Controllers[2]"): what opening a row remembers
		std::string name, type, value;
		std::uint32_t offset = 0;
		Handle link;			// An object the value refers to
		bool children = false;	// A struct or an array: can be opened
	};

	struct Link
	{
		std::string label;
		Handle handle;
	};

	// What the window shows, built on the game thread
	struct View
	{
		Handle selected;
		bool alive = false;
		std::string fullName, address;
		std::vector<Link> outers, classes;	// The outer chain (innermost first), the class and what it extends
		Link archetype;
		std::vector<Row> rows;
	};

	std::mutex g_lock;
	std::string g_query;				// The search the render thread asked for
	bool g_search = false, g_skipDefaults = true;
	std::vector<Result> g_results;
	std::string g_searchStatus;
	Handle g_selected;
	std::vector<Handle> g_back;			// Objects inspected before the selected one
	std::set<std::string> g_open;		// Rows opened (their keys)
	View g_view;

	// The class explorer: every loaded class, linked to what it extends
	struct ClassEntry
	{
		Handle handle;
		std::string name, path;			// "TdPawn", "TdGame.TdPawn"
		int super = -1;					// Index of what it extends in g_classes (-1: a root, or its base isn't loaded)
		std::vector<int> children;		// What extends it directly, by name
	};

	struct Function
	{
		std::string name, flags;		// "Tick", "native, event"
	};

	// The selected class, built on the game thread when the selection changes
	struct ClassView
	{
		Handle handle;
		bool alive = false;
		std::string path, package;
		int size = 0, instances = 0, inherited = 0;
		std::vector<Link> supers, subclasses;
		Link defaults;					// The class default object
		std::vector<Row> declared;		// Properties it declares, with the default object's values
		std::vector<Function> functions;
		std::vector<std::string> states;
	};

	std::vector<ClassEntry> g_classes;
	std::string g_classStatus;
	bool g_loadClasses = false;			// The render thread asks for the list (again)
	Handle g_class;						// The selected class
	ClassView g_classView;

	// ---- Game thread ------------------------------------------------------------------------------------------------

	std::string Label(const Handle& h) { return Alive(h) ? h.object->GetFullName() : std::string("(none)"); }

	// A class first ("TdPawn", "Engine.Actor"), then a path ("TdGame.Default__TdPawn"), then part of a name
	void Search(const std::string& query, bool skipDefaults, std::vector<Result>& out, std::string& status)
	{
		out.clear();
		auto add = [&](Classes::UObject* o) {
			if (out.size() >= kMaxResults || (skipDefaults && game::IsDefaultObject(o))) return;
			if (const Handle h = HandleOf(o); h.object) out.push_back({h, o->GetFullName()});
		};
		if (query.empty()) {
			for (Classes::AActor* a : game::ActorsInPlay()) add(a);
			status = fmt::format("{} actors in play", out.size());
		} else if (Classes::UClass* cls = game::FindClass(query, game::Freshness::Exact)) {
			for (Classes::UObject* o : game::ObjectsOf(cls, game::Freshness::Exact)) add(o);
			status = fmt::format("{} objects of class {}", out.size(), cls->GetName());
		} else if (Classes::UObject* found = game::FindByPath(query, game::Freshness::Exact)) {
			out.push_back({HandleOf(found), found->GetFullName()});
			status = "found by path";
		} else {
			const std::string needle = Lower(query);
			auto& objects = Classes::UObject::GetGlobalObjects();
			for (std::size_t i = 0; i < objects.Num() && out.size() < kMaxResults; ++i)
				if (Classes::UObject* o = objects.GetByIndex(i); o && Lower(o->GetName()).find(needle) != std::string::npos) add(o);
			status = fmt::format("{} objects named like \"{}\"", out.size(), query);
		}
		if (out.size() >= kMaxResults) status += fmt::format(" (the first {})", kMaxResults);
		std::sort(out.begin(), out.end(), [](const Result& a, const Result& b) { return a.name < b.name; });
	}

	std::string TypeOf(const reflect::Member& m)
	{
		std::string type = mirage::layout::KindName(m.kind);
		if (m.structType) type += " " + reflect::NameOf(m.structType);
		if (m.objectClass) type += " " + reflect::NameOf(m.objectClass);
		if (m.kind == Kind::Array && m.inner) type += "<" + TypeOf(*m.inner) + ">";
		if (m.arrayDim > 1) type += fmt::format("[{}]", m.arrayDim);
		return type;
	}

	std::vector<const reflect::Member*> Members(Classes::UStruct* type)
	{
		std::vector<const reflect::Member*> members = reflect::List(type);
		std::sort(members.begin(), members.end(), [](const auto* a, const auto* b) {
			return a->offset != b->offset ? a->offset < b->offset : a->boolMask < b->boolMask;
		});
		return members;
	}

	void AddRows(std::vector<Row>& rows, const reflect::Member& m, std::uint8_t* base, const std::string& name,
				 const std::string& key, int depth, bool element);

	// One value (an element of a static array is its own value): the row, and its children when it is open
	void AddValue(std::vector<Row>& rows, const reflect::Member& m, std::uint8_t* address, const std::string& name,
				  const std::string& key, int depth)
	{
		Row row{depth, key, name, TypeOf(m)};
		const reflect::Value v{&m, address};
		const bool open = depth < kMaxDepth && g_open.contains(key);
		switch (m.kind) {
		case Kind::Byte:
		case Kind::Int: row.value = std::to_string(reflect::GetInt(v)); break;
		case Kind::Bool: row.value = reflect::GetInt(v) ? "true" : "false"; break;
		case Kind::Float: row.value = fmt::format("{:g}", reflect::GetFloat(v)); break;
		case Kind::Name:
		case Kind::Str: row.value = reflect::GetText(v); break;
		case Kind::Object:
		case Kind::Class:
		case Kind::Component:
		case Kind::Interface: {
			Classes::UObject* o = *reinterpret_cast<Classes::UObject**>(address);
			row.link = HandleOf(o);
			row.value = !o ? "None" : row.link.object ? o->GetFullName() : fmt::format("(not a live object: {})", static_cast<void*>(o));
			break;
		}
		case Kind::Struct: {
			row.children = m.structType != nullptr;
			row.value = "{...}";
			rows.push_back(row);
			if (open && m.structType)
				for (const reflect::Member* c : Members(m.structType))
					AddRows(rows, *c, address, c->name, key + "." + c->name, depth + 1, false);
			return;
		}
		case Kind::Array: {
			const auto* header = reinterpret_cast<const reflect::ArrayHeader*>(address);
			row.children = m.inner && header->count > 0;
			row.value = fmt::format("{} element(s)", header->count);
			rows.push_back(row);
			if (open && row.children)
				for (int i = 0; i < std::min(header->count, kMaxElements); ++i)
					AddValue(rows, *m.inner, header->data + static_cast<std::size_t>(i) * m.inner->elementSize,
							 fmt::format("[{}]", i), fmt::format("{}[{}]", key, i), depth + 1);
			if (open && header->count > kMaxElements)
				rows.push_back({depth + 1, key + "[...]", fmt::format("... {} more", header->count - kMaxElements)});
			return;
		}
		default: row.value = fmt::format("({} bytes)", m.elementSize); break;
		}
		rows.push_back(std::move(row));
	}

	// A member of the object or struct at `base`: a static array is a row with its elements under it
	void AddRows(std::vector<Row>& rows, const reflect::Member& m, std::uint8_t* base, const std::string& name,
				 const std::string& key, int depth, bool element)
	{
		std::uint8_t* address = base + m.offset;
		if (m.arrayDim <= 1 || element) {
			const std::size_t at = rows.size();
			AddValue(rows, m, address, name, key, depth);
			rows[at].offset = m.offset;
			return;
		}
		rows.push_back({depth, key, name, TypeOf(m), fmt::format("{} element(s)", m.arrayDim), m.offset, {}, true});
		if (depth < kMaxDepth && g_open.contains(key))
			for (std::uint32_t i = 0; i < m.arrayDim; ++i)
				AddValue(rows, m, address + i * m.elementSize, fmt::format("[{}]", i), fmt::format("{}[{}]", key, i), depth + 1);
	}

	View Read(const Handle& h)
	{
		View view;
		view.selected = h;
		if (!(view.alive = Alive(h))) return view;
		Classes::UObject* o = h.object;
		view.fullName = o->GetFullName();
		view.address = fmt::format("{} (GObjects[{}])", static_cast<void*>(o), h.index);
		for (Classes::UObject* outer = o->Outer; outer; outer = outer->Outer) view.outers.push_back({outer->GetFullName(), HandleOf(outer)});
		for (Classes::UStruct* c = o->Class; c; c = c->SuperField) view.classes.push_back({c->GetName(), HandleOf(c)});
		if (o->ObjectArchetype) view.archetype = {o->ObjectArchetype->GetFullName(), HandleOf(o->ObjectArchetype)};
		for (const reflect::Member* m : Members(o->Class))
			AddRows(view.rows, *m, reinterpret_cast<std::uint8_t*>(o), m->name, m->name, 0, false);
		return view;
	}

	void Select(const Handle& h)
	{
		if (h == g_selected) return;
		if (g_selected.object) g_back.push_back(g_selected);
		g_selected = h;
		g_open.clear();
	}

	void LoadClasses(std::vector<ClassEntry>& out, std::string& status)
	{
		out.clear();
		std::unordered_map<const Classes::UObject*, int> at;
		for (Classes::UObject* o : game::ObjectsOf(Classes::UClass::StaticClass(), game::Freshness::Exact))
			if (const Handle h = HandleOf(o); h.object) {
				at[o] = static_cast<int>(out.size());
				out.push_back({h, o->GetName(), o->GetPathName()});
			}
		for (ClassEntry& e : out)
			if (const auto it = at.find(static_cast<Classes::UClass*>(e.handle.object)->SuperField); it != at.end()) e.super = it->second;
		for (int i = 0; i < static_cast<int>(out.size()); ++i)
			if (out[i].super >= 0) out[out[i].super].children.push_back(i);
		for (ClassEntry& e : out)
			std::sort(e.children.begin(), e.children.end(), [&](int a, int b) { return out[a].name < out[b].name; });
		status = fmt::format("{} classes loaded", out.size());
	}

	std::string FunctionFlags(std::uint32_t flags)
	{
		// FUNC_* (UE3): Final 0x1, Exec 0x200, Native 0x400, Event 0x800, Static 0x2000
		std::string out;
		for (const auto& [bit, name] : {std::pair{0x400u, "native"}, {0x800u, "event"}, {0x2000u, "static"}, {0x1u, "final"}, {0x200u, "exec"}})
			if (flags & bit) out += (out.empty() ? "" : ", ") + std::string(name);
		return out;
	}

	ClassView ReadClass(const Handle& h)
	{
		ClassView v;
		v.handle = h;
		if (!(v.alive = Alive(h))) return v;
		auto* cls = static_cast<Classes::UClass*>(h.object);
		v.path = cls->GetPathName();
		const Classes::UObject* package = cls;
		while (package->Outer) package = package->Outer;
		v.package = package->GetName();
		v.size = cls->PropertySize;
		for (Classes::UStruct* s = cls->SuperField; s; s = s->SuperField) v.supers.push_back({s->GetName(), HandleOf(s)});
		for (const ClassEntry& e : g_classes)
			if (e.super >= 0 && g_classes[e.super].handle == h) v.subclasses.push_back({e.name, e.handle});
		std::sort(v.subclasses.begin(), v.subclasses.end(), [](const Link& a, const Link& b) { return a.label < b.label; });
		for (Classes::UObject* o : game::ObjectsOf(cls, game::Freshness::Exact)) v.instances += game::IsDefaultObject(o) ? 0 : 1;
		if (Classes::UObject* cdo = game::FindByPath(v.package + ".Default__" + cls->GetName(), game::Freshness::Exact))
			v.defaults = {cdo->GetFullName(), HandleOf(cdo)};

		// Declared here: what its base doesn't have, valued in the default object when there is one
		std::set<std::string> inherited;
		if (cls->SuperField)
			for (const reflect::Member* m : reflect::List(cls->SuperField)) inherited.insert(m->name);
		v.inherited = static_cast<int>(inherited.size());
		for (const reflect::Member* m : Members(cls)) {
			if (inherited.contains(m->name)) continue;
			if (Alive(v.defaults.handle))
				AddRows(v.declared, *m, reinterpret_cast<std::uint8_t*>(v.defaults.handle.object), m->name, "class:" + m->name, 0, false);
			else
				v.declared.push_back({0, m->name, m->name, TypeOf(*m), {}, m->offset});
		}

		for (Classes::UField* f = cls->Children; f; f = f->Next) {
			if (f->IsA(Classes::UFunction::StaticClass()))
				v.functions.push_back({f->GetName(), FunctionFlags(static_cast<Classes::UFunction*>(f)->FunctionFlags)});
			else if (f->IsA(Classes::UState::StaticClass()))
				v.states.push_back(f->GetName());
		}
		std::sort(v.functions.begin(), v.functions.end(), [](const Function& a, const Function& b) { return a.name < b.name; });
		std::sort(v.states.begin(), v.states.end());
		return v;
	}

	// ---- Render thread ----------------------------------------------------------------------------------------------

	bool LinkButton(const Link& link, const char* id)
	{
		ImGui::PushID(id);
		bool clicked = false;
		if (link.handle.object) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			clicked = ImGui::Selectable(link.label.c_str(), false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(link.label.c_str()));
			ImGui::PopStyleColor();
		} else {
			ImGui::TextDisabled("%s", link.label.c_str());
		}
		ImGui::PopID();
		return clicked;
	}

	class ObjectInspector final : public Module
	{
	public:
		ObjectInspector() : Module("Object Inspector", "Misc", "The engine's live objects and classes: find one, see every property's value", 5) {}

		void OnEnable() override
		{
			std::lock_guard lock(g_lock);
			g_search = true;		// The actors in play, to start with
		}

		void OnTick() override
		{
			std::lock_guard lock(g_lock);
			if (g_search) {
				Search(g_query, g_skipDefaults, g_results, g_searchStatus);
				g_search = false;
			}
			g_view = Read(g_selected);
			if (g_loadClasses) {
				LoadClasses(g_classes, g_classStatus);
				g_loadClasses = false;
				g_classView = {};
			}
			if (g_class.object && !(g_classView.handle == g_class)) g_classView = ReadClass(g_class);
		}

		void OnRender() override
		{
			if (!gui::visible) return;
			theme::Begin();
			ImGui::SetNextWindowPos({300.f, 100.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({1000.f, 620.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({560.f, 280.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Object Inspector", &open, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		char query_[256] = {};
		char filter_[128] = {};
		char classFilter_[128] = {};
		char memberFilter_[128] = {};
		int switchTo_ = -1;			// A tab to bring forward on the next frame: 0 Objects, 1 Classes

		void Draw()
		{
			std::lock_guard lock(g_lock);
			if (!ImGui::BeginTabBar("##tabs")) return;
			const int switchTo = std::exchange(switchTo_, -1);
			if (ImGui::BeginTabItem("Objects", nullptr, switchTo == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (ImGui::BeginTable("##objects", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
					ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthStretch, 1.f);
					ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch, 2.f);
					ImGui::TableNextColumn();
					DrawSearch();
					ImGui::TableNextColumn();
					DrawObject();
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Classes", nullptr, switchTo == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
				if (g_classes.empty() && g_classStatus.empty()) g_loadClasses = true;		// The first visit
				if (ImGui::BeginTable("##classes", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
					ImGui::TableSetupColumn("Classes", ImGuiTableColumnFlags_WidthStretch, 1.f);
					ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 2.f);
					ImGui::TableNextColumn();
					DrawClassList();
					ImGui::TableNextColumn();
					DrawClass();
					ImGui::EndTable();
				}
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		// ---- The class explorer ----------------------------------------------------------------------------------

		void DrawClassList()
		{
			ImGui::SetNextItemWidth(-40.f);
			ImGui::InputTextWithHint("##classfilter", ICON_FA_MAGNIFYING_GLASS " Search classes", classFilter_, sizeof classFilter_);
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_ROTATE)) g_loadClasses = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the loaded classes again (a level load brings its own)");
			ImGui::TextDisabled("%s", g_loadClasses ? "reading..." : g_classStatus.c_str());
			if (!ImGui::BeginChild("##classlist")) {
				ImGui::EndChild();
				return;
			}
			const std::string needle = Lower(classFilter_);
			if (needle.empty()) {
				// The tree, from the classes that extend nothing loaded (Object, the intrinsic ones)
				for (int i = 0; i < static_cast<int>(g_classes.size()); ++i)
					if (g_classes[i].super < 0) DrawClassNode(i, true);
			} else {
				std::vector<int> shown;
				for (int i = 0; i < static_cast<int>(g_classes.size()); ++i)
					if (Lower(g_classes[i].path).find(needle) != std::string::npos) shown.push_back(i);
				std::sort(shown.begin(), shown.end(), [](int a, int b) { return g_classes[a].name < g_classes[b].name; });
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(shown.size()));
				while (clipper.Step())
					for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
						const ClassEntry& e = g_classes[shown[k]];
						ImGui::PushID(shown[k]);
						if (ImGui::Selectable(e.name.c_str(), e.handle == g_class)) g_class = e.handle;
						ImGui::SameLine();
						ImGui::TextDisabled("%s", e.path.c_str());
						ImGui::PopID();
					}
			}
			ImGui::EndChild();
		}

		void DrawClassNode(int i, bool root)
		{
			const ClassEntry& e = g_classes[i];
			ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
			if (e.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (e.handle == g_class) flags |= ImGuiTreeNodeFlags_Selected;
			if (root && e.name == "Object") ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
			ImGui::PushID(i);
			const std::string label = e.children.empty() ? e.name : fmt::format("{}  ({})", e.name, e.children.size());
			const bool open = ImGui::TreeNodeEx("##class", flags, "%s", label.c_str());
			if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_class = e.handle;
			if (open && !e.children.empty()) {
				for (int c : e.children) DrawClassNode(c, false);
				ImGui::TreePop();
			}
			ImGui::PopID();
		}

		void DrawClass()
		{
			const ClassView& v = g_classView;
			if (!g_class.object) {
				ImGui::TextDisabled("Pick a class on the left");
				return;
			}
			if (!(v.handle == g_class)) {
				ImGui::TextDisabled("reading...");
				return;
			}
			if (!v.alive) {
				ImGui::TextDisabled("The class is gone (its package was unloaded)");
				return;
			}
			ImGui::TextUnformatted(v.path.c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::SetClipboardText(v.path.c_str());
			ImGui::TextDisabled("Package %s, %d bytes, %d live instance(s)", v.package.c_str(), v.size, v.instances);

			Handle pick;	// A class clicked: explore it
			for (std::size_t i = 0; i < v.supers.size(); ++i) {
				if (i) ImGui::SameLine(0.f, 4.f);
				ImGui::TextDisabled(ICON_FA_ANGLE_RIGHT);
				ImGui::SameLine(0.f, 4.f);
				if (LinkButton(v.supers[i], fmt::format("s{}", i).c_str())) pick = v.supers[i].handle;
			}

			if (ImGui::Button(fmt::format(ICON_FA_MAGNIFYING_GLASS " Instances ({})", v.instances).c_str())) {
				std::snprintf(query_, sizeof query_, "%s", v.path.c_str());
				g_query = v.path;
				g_search = true;
				switchTo_ = 0;
			}
			ImGui::SameLine();
			ImGui::BeginDisabled(!v.defaults.handle.object);
			if (ImGui::Button(ICON_FA_CUBE " Default object")) {
				Select(v.defaults.handle);
				switchTo_ = 0;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_CIRCLE_INFO " Class object")) {
				Select(v.handle);
				switchTo_ = 0;
			}

			if (!v.subclasses.empty() && ImGui::TreeNode("Subclasses", "Subclasses (%zu)", v.subclasses.size())) {
				for (std::size_t i = 0; i < v.subclasses.size(); ++i)
					if (LinkButton(v.subclasses[i], fmt::format("d{}", i).c_str())) pick = v.subclasses[i].handle;
				ImGui::TreePop();
			}
			if (!v.states.empty() && ImGui::TreeNode("States", "States (%zu)", v.states.size())) {
				for (const std::string& s : v.states) ImGui::BulletText("%s", s.c_str());
				ImGui::TreePop();
			}

			ImGui::SetNextItemWidth(220.f);
			ImGui::InputTextWithHint("##members", ICON_FA_FILTER " Filter members", memberFilter_, sizeof memberFilter_);
			const std::string needle = Lower(memberFilter_);
			auto shows = [&](const std::string& name) { return needle.empty() || Lower(name).find(needle) != std::string::npos; };

			ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
			if (ImGui::CollapsingHeader(fmt::format("Properties: {} declared here, {} inherited###props", v.declared.size(), v.inherited).c_str()) &&
				ImGui::BeginTable("##declared", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 56.f);
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.f);
				ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 1.2f);
				ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 1.6f);
				ImGui::TableHeadersRow();
				for (const Row& r : v.declared) {
					if (!shows(r.name)) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextDisabled("0x%04X", r.offset);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", r.type.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(r.name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(r.value.c_str());
				}
				ImGui::EndTable();
			}
			if (ImGui::CollapsingHeader(fmt::format("Functions: {} declared here###funcs", v.functions.size()).c_str()) &&
				ImGui::BeginTable("##functions", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 1.f);
				ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthStretch, 1.f);
				for (const Function& f : v.functions) {
					if (!shows(f.name)) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(f.name.c_str());
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", f.flags.c_str());
				}
				ImGui::EndTable();
			}
			if (pick.object) g_class = pick;
		}

		// ---- The objects -----------------------------------------------------------------------------------------

		void DrawSearch()
		{
			ImGui::SetNextItemWidth(-90.f);
			const bool enter = ImGui::InputTextWithHint("##query", "Class, path or name (empty: actors in play)", query_, sizeof query_,
														ImGuiInputTextFlags_EnterReturnsTrue);
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS " Find") || enter) {
				g_query = query_;
				g_search = true;
			}
			if (ImGui::Checkbox("Skip class defaults", &g_skipDefaults)) g_search = true;
			ImGui::TextDisabled("%s", g_search ? "searching..." : g_searchStatus.c_str());
			if (!ImGui::BeginChild("##results")) {
				ImGui::EndChild();
				return;
			}
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(g_results.size()));
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					ImGui::PushID(i);
					if (ImGui::Selectable(g_results[i].name.c_str(), g_results[i].handle == g_selected)) Select(g_results[i].handle);
					ImGui::PopID();
				}
			ImGui::EndChild();
		}

		void DrawObject()
		{
			ImGui::BeginDisabled(g_back.empty());
			if (ImGui::Button(ICON_FA_ARROW_LEFT)) {
				g_selected = g_back.back();
				g_back.pop_back();
				g_open.clear();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			const View& v = g_view;
			if (!v.selected.object || !(v.selected == g_selected)) {
				ImGui::TextDisabled("Pick an object on the left");
				return;
			}
			if (!v.alive) {
				ImGui::TextDisabled("The object is gone (destroyed, or its level unloaded)");
				return;
			}
			ImGui::TextUnformatted(v.fullName.c_str());
			if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::SetClipboardText(v.fullName.c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Right click: copy");
			ImGui::TextDisabled("%s", v.address.c_str());

			Handle go;
			if (ImGui::TreeNode("Outers", "Outers (%zu)", v.outers.size())) {
				for (std::size_t i = 0; i < v.outers.size(); ++i)
					if (LinkButton(v.outers[i], fmt::format("o{}", i).c_str())) go = v.outers[i].handle;
				ImGui::TreePop();
			}
			if (ImGui::TreeNode("Classes", "Class chain (%zu)", v.classes.size())) {
				for (std::size_t i = 0; i < v.classes.size(); ++i) {
					if (i) {
						ImGui::SameLine(0.f, 4.f);
						ImGui::TextDisabled(ICON_FA_ANGLE_RIGHT);
						ImGui::SameLine(0.f, 4.f);
					}
					if (LinkButton(v.classes[i], fmt::format("c{}", i).c_str())) go = v.classes[i].handle;
				}
				ImGui::TreePop();
			}
			if (!v.archetype.label.empty()) {
				ImGui::TextDisabled("Archetype:");
				ImGui::SameLine();
				if (LinkButton(v.archetype, "arch")) go = v.archetype.handle;
			}

			ImGui::SetNextItemWidth(220.f);
			ImGui::InputTextWithHint("##filter", ICON_FA_FILTER " Filter properties", filter_, sizeof filter_);
			ImGui::SameLine();
			ImGui::TextDisabled("%zu rows, live", v.rows.size());
			const std::string needle = Lower(filter_);

			if (ImGui::BeginTable("##props", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 1.4f);
				ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.f);
				ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 2.f);
				ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 56.f);
				ImGui::TableHeadersRow();
				for (std::size_t i = 0; i < v.rows.size(); ++i) {
					const Row& r = v.rows[i];
					// The filter matches top-level properties; what is open under a match stays with it
					if (!needle.empty() && Lower(r.key.substr(0, r.key.find_first_of(".["))).find(needle) == std::string::npos) continue;
					ImGui::PushID(static_cast<int>(i));
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Indent(r.depth * 14.f);
					if (r.children) {
						bool open = g_open.contains(r.key);
						if (ImGui::ArrowButton("##open", open ? ImGuiDir_Down : ImGuiDir_Right)) {
							if (open) g_open.erase(r.key);
							else g_open.insert(r.key);
						}
						ImGui::SameLine();
					}
					ImGui::TextUnformatted(r.name.c_str());
					ImGui::Unindent(r.depth * 14.f);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", r.type.c_str());
					ImGui::TableNextColumn();
					if (r.link.object) {
						if (LinkButton({r.value, r.link}, "v")) go = r.link;
					} else {
						ImGui::TextUnformatted(r.value.c_str());
					}
					if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::SetClipboardText(r.value.c_str());
					ImGui::TableNextColumn();
					if (r.depth == 0) ImGui::TextDisabled("0x%04X", r.offset);
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (go.object) Select(go);
		}
	};
	MEM_MODULE(ObjectInspector)
}
