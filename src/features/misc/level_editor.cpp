// The Level Editor (Mirage, step 13): places actors into a mod's level layer (levels/<map>.toml, runtime/layers.h)
// while playing. Pick a mod and a class (any placeable class the game has loaded, or the mod's own .uc classes), place
// one where the camera looks, then move, turn and scale it with the gizmo in the world (with snapping) or type its
// values, and edit its properties from the list of what its class lets an editor change. Every change is saved to the
// mod's folder and the layer is spawned again from it; Ctrl+Z / Ctrl+Y undo and redo, Ctrl+D duplicates, Delete
// removes. The placed actors are marked in the world: click one to select it. Fly (No Clip) is the free camera.
//
// Threads: the window and the world overlay draw on the render thread from the editor's copy of the layer (under
// g_mutex) and only note what the user did; the actions run after the lock is let go, and anything touching game
// objects is posted to the game thread.

#include "features/misc/level_editor.h"

#include "core/module.h"
#include "game/game.h"
#include "game/object_query.h"
#include "game/uobject.h"
#include "gui.h"
#include "render/math.h"
#include "runtime/call.h"
#include "runtime/classes.h"
#include "runtime/layers.h"
#include "runtime/mods.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef GetObject
#undef GetObject
#endif

namespace
{
	namespace layers = mirage::layers;
	namespace reflect = mirage::reflect;
	namespace fs = std::filesystem;
	using EntryList = std::vector<layers::EditActor>;

	constexpr std::uint32_t kClassAbstract = 0x1, kClassPlaceable = 0x200;	// UClass::ClassFlags
	constexpr std::uint64_t kPropertyEdit = 0x1;							// CPF_Edit: var() in UnrealScript
	constexpr float kRotationPerDegree = 65536.f / 360.f;

	std::atomic<std::uint64_t> g_frames{0}, g_gizmoFrames{0};

	// What the window edits: shared between the render thread (the window) and the game thread (reads, saves, moves)
	std::mutex g_mutex;
	std::string g_mod;							// The mod whose layer is edited
	std::string g_map;							// The level it was read for
	EntryList g_actors;							// Its entries as last read or edited
	int g_selected = -1;
	std::string g_status;
	bool g_reload = true;						// Read the layer again (mod or level changed)
	std::string g_pendingSelect;				// Select this entry once the layer is read
	// Undo and redo: the layer as it was saved before each change (the editor's own history; a mod's other changes
	// aren't in it)
	EntryList g_saved;
	std::vector<EntryList> g_undo, g_redo;
	constexpr std::size_t kHistory = 100;
	// Snapping (0: off)
	float g_grid = 0.f, g_angle = 0.f, g_scaleStep = 0.f;
	// The placeable classes the game has loaded (built on the game thread when asked; a level load changes it)
	std::vector<std::string> g_classes;
	bool g_classesWanted = true;
	// The selected entry's spawned actor: what its class lets an editor change, with the values it has now
	std::vector<level_editor::Property> g_props;
	std::string g_propsFor;						// The entry they are for ("" none)
	bool g_propsWanted = false;

	void Status(std::string text)
	{
		std::lock_guard lock(g_mutex);
		g_status = std::move(text);
	}

	bool Same(const layers::EditActor& a, const layers::EditActor& b)
	{
		return a.name == b.name && a.cls == b.cls && std::equal(a.location, a.location + 3, b.location) &&
			   std::equal(a.rotation, a.rotation + 3, b.rotation) && a.scale == b.scale && a.properties == b.properties;
	}
	bool Same(const EntryList& a, const EntryList& b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) { return Same(x, y); });
	}

	std::string UniqueName(const EntryList& entries, const std::string& base)
	{
		for (int n = 1;; ++n) {
			std::string name = base + "_" + std::to_string(n);
			if (std::none_of(entries.begin(), entries.end(), [&](const layers::EditActor& a) { return a.name == name; })) return name;
		}
	}

	std::string ShortClass(const std::string& cls)
	{
		std::string s = cls;
		if (s.size() > 3 && s.substr(s.size() - 3) == ".uc") s.resize(s.size() - 3);
		s = s.substr(s.find_last_of("./\\") + 1);
		return s.empty() ? "Actor" : s;
	}

	float SnapTo(float v, float step) { return step > 0.f ? std::round(v / step) * step : v; }

	// Game thread: the layer read again
	void ReadNow()
	{
		std::string mod;
		{
			std::lock_guard lock(g_mutex);
			mod = g_mod;
		}
		EntryList actors;
		std::string error;
		const bool ok = mod.empty() || layers::Read(mod, actors, &error);
		std::lock_guard lock(g_mutex);
		const std::string map = layers::CurrentMap();
		if (map != g_map) {						// Another level: another layer, another history
			g_undo.clear();
			g_redo.clear();
			g_classesWanted = true;
		}
		g_map = map;
		if (ok) {
			g_actors = std::move(actors);
			g_saved = g_actors;
		} else {
			g_status = "the layer doesn't parse: " + error;
		}
		if (!g_pendingSelect.empty())
			for (std::size_t i = 0; i < g_actors.size(); ++i)
				if (g_actors[i].name == g_pendingSelect) {
					g_selected = static_cast<int>(i);
					g_pendingSelect.clear();
				}
		if (g_selected >= static_cast<int>(g_actors.size())) g_selected = -1;
		g_propsWanted = true;
	}

	// The layer written as the editor has it (game thread), spawned again by the layers
	void WriteOnGameThread(const std::string& mod, const EntryList& actors, const std::string& done)
	{
		game::RunOnGameThread([mod, actors, done] {
			std::string error;
			const bool ok = layers::Write(mod, actors, &error);
			Status(ok ? done : "not saved: " + error);
			std::lock_guard lock(g_mutex);
			g_propsWanted = true;				// The selected actor was spawned again
		});
	}

	// Saves the entries (any thread), first remembering the layer as it was for undo
	void Save(const std::string& done = {})
	{
		std::string mod;
		EntryList actors;
		{
			std::lock_guard lock(g_mutex);
			if (g_mod.empty()) return;
			if (!Same(g_actors, g_saved)) {
				g_undo.push_back(g_saved);
				if (g_undo.size() > kHistory) g_undo.erase(g_undo.begin());
				g_redo.clear();
				g_saved = g_actors;
			}
			mod = g_mod;
			actors = g_actors;
		}
		WriteOnGameThread(mod, actors, done.empty() ? "saved levels/" + layers::CurrentMap() + ".toml" : done);
	}

	// Undo / redo: the layer as it was, saved as it is
	void Step(bool undo)
	{
		std::string mod;
		EntryList actors;
		{
			std::lock_guard lock(g_mutex);
			auto& from = undo ? g_undo : g_redo;
			auto& to = undo ? g_redo : g_undo;
			if (from.empty() || g_mod.empty()) return;
			const std::string selected = g_selected >= 0 && g_selected < static_cast<int>(g_actors.size()) ? g_actors[static_cast<std::size_t>(g_selected)].name : "";
			to.push_back(g_actors);
			g_actors = std::move(from.back());
			from.pop_back();
			g_saved = g_actors;
			g_selected = -1;
			for (std::size_t i = 0; i < g_actors.size(); ++i)
				if (g_actors[i].name == selected) g_selected = static_cast<int>(i);
			mod = g_mod;
			actors = g_actors;
		}
		WriteOnGameThread(mod, actors, undo ? "undone" : "redone");
	}

	// Game thread: the spawned actor of entry `a` moved to where the entry says (the gizmo, the fields)
	void PlaceLive(const std::string& mod, const layers::EditActor& a)
	{
		layers::Place(layers::Find(mod, a.name), a.location, a.rotation, a.scale);
	}

	// Game thread: where a trace from `start` to `end` hits the world (actors too if `actors`)
	bool Trace(const Classes::FVector& start, const Classes::FVector& end, bool actors, Classes::FVector& hit, Classes::FVector& normal)
	{
		auto* pawn = game::Pawn();
		if (!pawn) return false;
		mirage::call::Params trace(pawn, "Trace");
		if (!trace.Valid() || !trace.SetRaw("TraceEnd", &end, sizeof end) || !trace.SetRaw("TraceStart", &start, sizeof start) ||
			!trace.Set("bTraceActors", actors) || !mirage::call::Call(pawn, trace) || !trace.GetObject("ReturnValue"))
			return false;
		trace.GetRaw("HitLocation", &hit, sizeof hit);
		trace.GetRaw("HitNormal", &normal, sizeof normal);
		return true;
	}

	// Game thread: where the camera looks, on the world (a trace from the camera), or 500 units ahead
	bool LookAt(float out[3])
	{
		const rmath::Camera c = game::GetCamera();
		if (!c.valid || !game::Pawn()) return false;
		constexpr float kTurn = 6.2831853f / 65536.f;
		const float pitch = static_cast<float>(c.pitch) * kTurn, yaw = static_cast<float>(c.yaw) * kTurn;
		const Classes::FVector dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
		const Classes::FVector start{c.pos.x, c.pos.y, c.pos.z};
		const Classes::FVector end{start.X + dir.X * 5000.f, start.Y + dir.Y * 5000.f, start.Z + dir.Z * 5000.f};
		Classes::FVector hit{}, normal{};
		if (Trace(start, end, true, hit, normal)) {
			out[0] = hit.X + normal.X * 40.f;		// A little off the surface
			out[1] = hit.Y + normal.Y * 40.f;
			out[2] = hit.Z + normal.Z * 40.f;
			return true;
		}
		out[0] = start.X + dir.X * 500.f;
		out[1] = start.Y + dir.Y * 500.f;
		out[2] = start.Z + dir.Z * 500.f;
		return true;
	}

	// Game thread: how far an actor's origin is above the bottom of its collision (0 if it has none)
	float BottomOffset(Classes::AActor* a)
	{
		if (!a) return 0.f;
		const reflect::Value origin = reflect::Resolve(a->Class, a, "CollisionComponent.Bounds.Origin.Z");
		const reflect::Value extent = reflect::Resolve(a->Class, a, "CollisionComponent.Bounds.BoxExtent.Z");
		if (!origin || !extent) return 0.f;
		const float bottom = reflect::GetFloat(origin) - reflect::GetFloat(extent);
		return std::max(0.f, a->Location.Z - bottom);
	}

	// ---- What a class lets an editor change ----------------------------------------------------------------------
	std::string FloatText(float f)
	{
		char buf[48];
		std::snprintf(buf, sizeof buf, "%.6g", f);
		std::string s = buf;
		if (s.find_first_of(".eEn") == std::string::npos) s += ".0";		// A TOML float, not an integer
		return s;
	}

	// The value of member `m` of object `o` as layer text ("1500.0", "true", "\"Name\"", "[1.0, 2.0, 3.0]"); false for a
	// kind the layers can't set
	bool ValueText(Classes::UObject* o, const reflect::Member& m, std::string& out, std::string& kind)
	{
		using mirage::layout::Kind;
		const reflect::Value v = reflect::Resolve(o->Class, o, m.name);
		if (!v) return false;
		switch (m.kind) {
			case Kind::Float: kind = "float"; out = FloatText(reflect::GetFloat(v)); return true;
			case Kind::Int: kind = "int"; out = std::to_string(reflect::GetInt(v)); return true;
			case Kind::Byte: kind = "byte"; out = std::to_string(reflect::GetInt(v)); return true;
			case Kind::Bool: kind = "bool"; out = reflect::GetInt(v) ? "true" : "false"; return true;
			case Kind::Name: kind = "name"; out = "\"" + reflect::GetText(v) + "\""; return true;
			case Kind::Object: case Kind::Class: case Kind::Component: {
				kind = m.kind == Kind::Class ? "class" : "object";
				Classes::UObject* x = reflect::GetObj(v);
				out = "\"" + (x ? x->GetPathName() : std::string("None")) + "\"";
				return true;
			}
			case Kind::Struct: {
				const std::string type = m.structType ? m.structType->GetName() : "";
				if (type == "Vector") {
					kind = "vector";
					out = "[" + FloatText(reflect::GetFloat(reflect::Resolve(o->Class, o, m.name + ".X"))) + ", " +
						  FloatText(reflect::GetFloat(reflect::Resolve(o->Class, o, m.name + ".Y"))) + ", " +
						  FloatText(reflect::GetFloat(reflect::Resolve(o->Class, o, m.name + ".Z"))) + "]";
					return true;
				}
				if (type == "Rotator") {
					kind = "rotator";
					out = "[" + std::to_string(reflect::GetInt(reflect::Resolve(o->Class, o, m.name + ".Pitch"))) + ", " +
						  std::to_string(reflect::GetInt(reflect::Resolve(o->Class, o, m.name + ".Yaw"))) + ", " +
						  std::to_string(reflect::GetInt(reflect::Resolve(o->Class, o, m.name + ".Roll"))) + "]";
					return true;
				}
				return false;
			}
			default: return false;
		}
	}

	// The world on screen (the camera the game reports)
	bool Project(const float p[3], ImVec2& out)
	{
		const rmath::Camera c = game::GetCamera();
		const ImVec2 size = ImGui::GetIO().DisplaySize;
		if (!c.valid || size.x <= 0.f || size.y <= 0.f) return false;
		const rmath::Mat4 vp = rmath::Mul(rmath::ViewMatrix(c), rmath::ProjectionMatrix(c.fov, size.x / size.y, 10.f));
		return rmath::Project(vp, {p[0], p[1], p[2]}, size.x, size.y, out.x, out.y);
	}

	// What the user asked for this frame (run once the lock is let go)
	enum class Action { None, Save, Undo, Redo, Duplicate, Delete, DropToFloor, BringHere, GoTo, Snap, Place, AddProperty };

	class LevelEditor final : public Module
	{
	public:
		LevelEditor() : Module("Level Editor", "Levels", "Place actors into a mod's level layer while playing: gizmo, properties, saved to the mod", 2) {}

		void OnEnable() override
		{
			game::SetCameraTracking(true);
			std::lock_guard lock(g_mutex);
			g_reload = true;
		}

		void OnDisable() override
		{
			if (Module* fly = modules::Find("No Clip"); fly && flying_) fly->SetEnabled(false);
			flying_ = false;
		}

		void OnTick() override
		{
			bool reload, classes, props;
			std::string mod, propsFor;
			{
				std::lock_guard lock(g_mutex);
				reload = g_reload || layers::CurrentMap() != g_map;
				g_reload = false;
				classes = g_classesWanted;
				g_classesWanted = false;
				const std::string selected = g_selected >= 0 && g_selected < static_cast<int>(g_actors.size()) ? g_actors[static_cast<std::size_t>(g_selected)].name : "";
				props = g_propsWanted || selected != g_propsFor;
				g_propsWanted = false;
				mod = g_mod;
				propsFor = selected;
			}
			if (reload) ReadNow();
			if (classes) {
				std::vector<std::string> found = level_editor::PlaceableClasses();
				std::lock_guard lock(g_mutex);
				g_classes = std::move(found);
			}
			if (props) {
				std::vector<level_editor::Property> found = propsFor.empty() ? std::vector<level_editor::Property>{}
																			  : level_editor::PropertiesOf(propsFor);
				std::lock_guard lock(g_mutex);
				g_props = std::move(found);
				g_propsFor = propsFor;
			}
		}

		void OnRender() override
		{
			if (!gui::visible) return;		// A tool to work with the mouse: with the menu open
			++g_frames;
			action_ = Action::None;
			DrawWorld();
			theme::Begin();
			ImGui::SetNextWindowPos({340.f, 160.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({480.f, 640.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({380.f, 300.f}, {FLT_MAX, FLT_MAX});
			if (ImGui::Begin("Level Editor", nullptr, ImGuiWindowFlags_NoCollapse)) DrawWindow();
			ImGui::End();
			theme::End();
			Keys();
			Run();
		}

	private:
		void Keys()
		{
			const ImGuiIO& io = ImGui::GetIO();
			if (io.WantTextInput || action_ != Action::None) return;
			if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) action_ = io.KeyShift ? Action::Redo : Action::Undo;
			else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) action_ = Action::Redo;
			else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) action_ = Action::Duplicate;
			else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) action_ = Action::Delete;
		}

		void Run()
		{
			switch (action_) {
				case Action::Save: Save(); break;
				case Action::Undo: level_editor::Undo(); break;
				case Action::Redo: level_editor::Redo(); break;
				case Action::Duplicate: level_editor::Duplicate(); break;
				case Action::Delete: level_editor::DeleteSelected(); break;
				case Action::DropToFloor: level_editor::DropToFloor(); break;
				case Action::BringHere: level_editor::BringHere(); break;
				case Action::GoTo: level_editor::GoTo(); break;
				case Action::Snap: level_editor::SnapSelected(); break;
				case Action::Place: level_editor::PlaceWhereLooking(cls_); break;
				case Action::AddProperty: Save("added " + addedProperty_); break;
				case Action::None: break;
			}
			action_ = Action::None;
		}

		void DrawWindow()
		{
			std::lock_guard lock(g_mutex);
			// The mod to edit
			const auto list = mirage::mods::List();
			if (ImGui::BeginCombo("Mod", g_mod.empty() ? "(pick one)" : g_mod.c_str())) {
				for (const auto& m : list)
					if (m.loaded && ImGui::Selectable(m.id.c_str(), m.id == g_mod)) {
						g_mod = m.id;
						g_selected = -1;
						g_reload = true;
						g_undo.clear();
						g_redo.clear();
					}
				ImGui::EndCombo();
			}
			ImGui::TextDisabled("Layer: levels/%s.toml", g_map.empty() ? "(no level)" : g_map.c_str());
			if (Module* fly = modules::Find("No Clip")) {
				bool on = fly->Enabled();
				if (ImGui::Checkbox("Fly (No Clip): the free camera", &on)) {
					fly->SetEnabled(on);
					flying_ = on;
				}
			}
			if (g_mod.empty()) return;
			ImGui::Separator();

			// A class and Place
			DrawClassPicker(list);
			ImGui::BeginDisabled(!cls_[0]);
			if (ImGui::Button("Place where I look")) action_ = Action::Place;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(g_undo.empty());
			if (ImGui::Button("Undo")) action_ = Action::Undo;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(g_redo.empty());
			if (ImGui::Button("Redo")) action_ = Action::Redo;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("Ctrl+Z / Ctrl+Y, Ctrl+D, Del");
			if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());

			// Snapping
			if (ImGui::TreeNode("Snapping")) {
				ImGui::SetNextItemWidth(120.f);
				ImGui::DragFloat("Grid", &g_grid, 1.f, 0.f, 1024.f, g_grid > 0.f ? "%.0f" : "off");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(100.f);
				ImGui::DragFloat("Angle", &g_angle, 1.f, 0.f, 90.f, g_angle > 0.f ? "%.0f deg" : "off");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(100.f);
				ImGui::DragFloat("Scale step", &g_scaleStep, 0.01f, 0.f, 1.f, g_scaleStep > 0.f ? "%.2f" : "off");
				ImGui::TreePop();
			}
			ImGui::Separator();

			// The layer's actors
			ImGui::BeginChild("##actors", {0.f, 120.f}, ImGuiChildFlags_Borders);
			for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
				const auto& a = g_actors[static_cast<std::size_t>(i)];
				const std::string label = (a.name.empty() ? "(unnamed)" : a.name) + "  " + a.cls + "##" + std::to_string(i);
				if (ImGui::Selectable(label.c_str(), i == g_selected)) g_selected = i;
			}
			if (g_actors.empty()) ImGui::TextDisabled("Nothing placed in this level yet");
			ImGui::EndChild();

			if (g_selected < 0 || g_selected >= static_cast<int>(g_actors.size())) return;
			auto& a = g_actors[static_cast<std::size_t>(g_selected)];
			char name[96];
			std::snprintf(name, sizeof name, "%s", a.name.c_str());
			if (ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) && name[0]) {
				a.name = name;
				action_ = Action::Save;
			}
			bool moved = ImGui::DragFloat3("Location", a.location, 2.f);
			moved |= ImGui::DragInt3("Rotation", a.rotation, 64.f);
			moved |= ImGui::DragFloat("Scale", &a.scale, 0.01f, 0.05f, 50.f);
			if (moved) PostPlace(a);
			if (moved && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) action_ = Action::Save;		// Typed: saved now
			if (ImGui::IsItemDeactivatedAfterEdit()) action_ = Action::Save;					// Dragged: on release

			// Tools
			if (ImGui::Button("Drop to floor")) action_ = Action::DropToFloor;
			ImGui::SameLine();
			if (ImGui::Button("Bring here")) action_ = Action::BringHere;
			ImGui::SameLine();
			if (ImGui::Button("Go to it")) action_ = Action::GoTo;
			ImGui::SameLine();
			if (ImGui::Button("Snap")) action_ = Action::Snap;
			ImGui::SameLine();
			if (ImGui::Button("Duplicate")) action_ = Action::Duplicate;
			ImGui::SameLine();
			if (ImGui::Button("Delete")) action_ = Action::Delete;

			// Its properties: the overrides in the layer, then what its class lets an editor change
			ImGui::SeparatorText("Properties (saved in the layer)");
			for (std::size_t i = 0; i < a.properties.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));
				char value[160];
				std::snprintf(value, sizeof value, "%s", a.properties[i].second.c_str());
				ImGui::SetNextItemWidth(180.f);
				if (ImGui::InputText(a.properties[i].first.c_str(), value, sizeof value, ImGuiInputTextFlags_EnterReturnsTrue)) {
					a.properties[i].second = value;
					action_ = Action::Save;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x")) {
					a.properties.erase(a.properties.begin() + static_cast<std::ptrdiff_t>(i));
					action_ = Action::Save;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			if (a.properties.empty()) ImGui::TextDisabled("None: add one from the class's list below, or by name");
			ImGui::SetNextItemWidth(140.f);
			ImGui::InputText("##newkey", newKey_, sizeof newKey_);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.f);
			ImGui::InputText("##newvalue", newValue_, sizeof newValue_);
			ImGui::SameLine();
			if (ImGui::Button("Add") && newKey_[0]) {
				a.properties.emplace_back(newKey_, newValue_);
				newKey_[0] = newValue_[0] = 0;
				action_ = Action::Save;
			}
			DrawClassProperties(a);
		}

		void DrawClassPicker(const std::vector<mirage::mods::Info>& list)
		{
			ImGui::SetNextItemWidth(-90.f);
			ImGui::InputTextWithHint("##class", "a class: Engine.Trigger, classes/MyPad.uc", cls_, sizeof cls_);
			ImGui::SameLine();
			if (ImGui::Button("Classes...")) ImGui::OpenPopup("##classpicker");
			ImGui::SetNextWindowSize({420.f, 360.f}, ImGuiCond_Appearing);
			if (!ImGui::BeginPopup("##classpicker")) return;
			ImGui::SetNextItemWidth(-1.f);
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextWithHint("##filter", "search", filter_, sizeof filter_);
			std::string f = filter_;
			std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const auto pick = [&](const std::string& c) {
				std::snprintf(cls_, sizeof cls_, "%s", c.c_str());
				ImGui::CloseCurrentPopup();
			};
			ImGui::BeginChild("##list", {0.f, 0.f});
			// The mod's own classes first
			const auto mod = std::find_if(list.begin(), list.end(), [&](const auto& m) { return m.id == g_mod; });
			std::error_code ec;
			if (mod != list.end())
				for (const auto& e : fs::directory_iterator(mod->dir / "classes", ec))
					if (e.path().extension() == ".uc") {
						const std::string rel = "classes/" + e.path().filename().string();
						std::string low = rel;
						std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
						if ((f.empty() || low.find(f) != std::string::npos) && ImGui::Selectable(rel.c_str())) pick(rel);
					}
			ImGui::Separator();
			int shown = 0;
			for (const std::string& c : g_classes) {
				std::string low = c;
				std::transform(low.begin(), low.end(), low.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				if (!f.empty() && low.find(f) == std::string::npos) continue;
				if (++shown > 400) {
					ImGui::TextDisabled("... more: narrow the search");
					break;
				}
				if (ImGui::Selectable(c.c_str())) pick(c);
			}
			if (g_classes.empty()) ImGui::TextDisabled("(looking for the game's classes)");
			ImGui::EndChild();
			ImGui::EndPopup();
		}

		void DrawClassProperties(layers::EditActor& a)
		{
			const std::string title = "Its class's editable properties (" + std::to_string(g_props.size()) + ")###classprops";
			if (!ImGui::TreeNode(title.c_str())) return;
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputTextWithHint("##propfilter", "search", propFilter_, sizeof propFilter_);
			std::string f = propFilter_;
			std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (g_propsFor != a.name) ImGui::TextDisabled("(reading the spawned actor)");
			if (ImGui::BeginTable("##props", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, {0.f, 200.f})) {
				ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Now", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.f);
				for (const auto& p : g_props) {
					std::string low = p.name;
					std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (!f.empty() && low.find(f) == std::string::npos) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(p.name.c_str());
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.kind.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(p.value.c_str());
					ImGui::TableNextColumn();
					const bool has = std::any_of(a.properties.begin(), a.properties.end(), [&](const auto& kv) { return kv.first == p.name; });
					ImGui::BeginDisabled(has);
					ImGui::PushID(p.name.c_str());
					if (ImGui::SmallButton("+")) {
						a.properties.emplace_back(p.name, p.value);
						addedProperty_ = p.name;
						action_ = Action::AddProperty;
					}
					ImGui::PopID();
					ImGui::EndDisabled();
				}
				ImGui::EndTable();
			}
			ImGui::TreePop();
		}

		void PostPlace(const layers::EditActor& a)
		{
			game::RunOnGameThread([mod = g_mod, a] { PlaceLive(mod, a); });
		}

		// Every placed actor marked (click one to select it); on the selected one, arrows (move along X, Y, Z), a ring
		// (turn) and a handle (scale): dragging one moves the actor live, snapped, and letting go saves
		void DrawWorld()
		{
			std::lock_guard lock(g_mutex);
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const ImGuiIO& io = ImGui::GetIO();
			const bool click = drag_ < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse;
			const auto near = [&](ImVec2 p, float r) { return std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < r; };
			int clicked = -1;
			for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
				if (i == g_selected) continue;
				ImVec2 p;
				if (!Project(g_actors[static_cast<std::size_t>(i)].location, p)) continue;
				const bool hot = near(p, 9.f);
				dl->AddQuadFilled({p.x, p.y - 6.f}, {p.x + 6.f, p.y}, {p.x, p.y + 6.f}, {p.x - 6.f, p.y}, hot ? IM_COL32(255, 220, 80, 255) : IM_COL32(120, 200, 255, 200));
				dl->AddText({p.x + 8.f, p.y - 7.f}, IM_COL32(220, 230, 255, hot ? 255 : 160), g_actors[static_cast<std::size_t>(i)].name.c_str());
				if (click && hot) clicked = i;
			}

			if (g_selected < 0 || g_selected >= static_cast<int>(g_actors.size())) {
				drag_ = -1;
				if (clicked >= 0) g_selected = clicked;
				return;
			}
			auto& a = g_actors[static_cast<std::size_t>(g_selected)];
			ImVec2 c;
			if (!Project(a.location, c)) {
				if (clicked >= 0) g_selected = clicked;
				return;
			}
			++g_gizmoFrames;
			constexpr float kLength = 150.f;
			const ImU32 colors[3] = {IM_COL32(235, 70, 70, 255), IM_COL32(80, 210, 90, 255), IM_COL32(80, 130, 255, 255)};
			ImVec2 ends[3];
			bool have[3]{};
			for (int k = 0; k < 3; ++k) {
				float p[3] = {a.location[0], a.location[1], a.location[2]};
				p[k] += kLength;
				have[k] = Project(p, ends[k]);
				if (!have[k]) continue;
				dl->AddLine(c, ends[k], colors[k], drag_ == k ? 5.f : 3.f);
				dl->AddCircleFilled(ends[k], 7.f, colors[k]);
			}
			const ImVec2 ring{c.x, c.y}, scaleHandle{c.x + 46.f, c.y - 46.f};
			dl->AddCircle(ring, 34.f, IM_COL32(255, 220, 80, 220), 0, drag_ == 3 ? 4.f : 2.f);
			dl->AddRectFilled({scaleHandle.x - 6.f, scaleHandle.y - 6.f}, {scaleHandle.x + 6.f, scaleHandle.y + 6.f}, IM_COL32(230, 230, 230, 230));
			dl->AddText({c.x + 10.f, c.y + 10.f}, IM_COL32(255, 255, 255, 220), a.name.c_str());

			if (click) {
				for (int k = 0; k < 3; ++k)
					if (have[k] && near(ends[k], 12.f)) drag_ = k;
				if (drag_ < 0 && near(scaleHandle, 10.f)) drag_ = 4;
				if (drag_ < 0 && std::fabs(std::hypot(io.MousePos.x - ring.x, io.MousePos.y - ring.y) - 34.f) < 6.f) drag_ = 3;
				if (drag_ >= 0) {		// What the drag starts from, unsnapped
					dragValue_ = drag_ < 3 ? a.location[drag_] : drag_ == 3 ? static_cast<float>(a.rotation[1]) : a.scale;
				} else if (clicked >= 0) {
					g_selected = clicked;
					return;
				}
			}
			if (drag_ < 0) return;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {		// Let go: saved
				drag_ = -1;
				action_ = Action::Save;
				return;
			}
			const ImVec2 d = io.MouseDelta;
			if (d.x == 0.f && d.y == 0.f) return;
			if (drag_ < 3) {
				const ImVec2 axis{ends[drag_].x - c.x, ends[drag_].y - c.y};
				const float len2 = axis.x * axis.x + axis.y * axis.y;
				if (len2 > 1.f) dragValue_ += (d.x * axis.x + d.y * axis.y) / len2 * kLength;
				a.location[drag_] = SnapTo(dragValue_, g_grid);
			} else if (drag_ == 3) {
				dragValue_ += d.x * 120.f;
				a.rotation[1] = static_cast<int>(SnapTo(dragValue_, g_angle * kRotationPerDegree)) & 0xFFFF;
			} else {
				dragValue_ = std::clamp(dragValue_ * (1.f - d.y * 0.01f), 0.05f, 50.f);
				a.scale = std::max(0.05f, SnapTo(dragValue_, g_scaleStep));
			}
			PostPlace(a);
		}

		char cls_[160] = "";
		char filter_[64] = "";
		char propFilter_[64] = "";
		char newKey_[64] = "";
		char newValue_[96] = "";
		int drag_ = -1;			// 0..2: an axis, 3: the ring, 4: scale
		float dragValue_ = 0.f;	// The dragged value before snapping
		bool flying_ = false;	// No Clip was switched on from here
		Action action_ = Action::None;
		std::string addedProperty_;
	};
	MEM_MODULE(LevelEditor)

	// Runs `fn` on the selected entry (a copy of the layer, the entry's index) on the game thread, then saves what it
	// changed; `done` is the status
	template <class F>
	void WithSelected(const std::string& done, F fn)
	{
		game::RunOnGameThread([done, fn] {
			std::string mod;
			EntryList actors;
			int selected;
			{
				std::lock_guard lock(g_mutex);
				selected = g_selected;
				if (g_mod.empty() || selected < 0 || selected >= static_cast<int>(g_actors.size())) return;
				mod = g_mod;
				actors = g_actors;
			}
			if (!fn(mod, actors, selected)) return;
			{
				std::lock_guard lock(g_mutex);
				g_actors = std::move(actors);
				g_selected = std::min(selected, static_cast<int>(g_actors.size()) - 1);
			}
			Save(done);
		});
	}
}

std::uint64_t level_editor::Frames() { return g_frames.load(); }
std::uint64_t level_editor::GizmoFrames() { return g_gizmoFrames.load(); }

void level_editor::Select(const std::string& mod, const std::string& name)
{
	std::lock_guard lock(g_mutex);
	if (g_mod != mod) {
		g_mod = mod;
		g_actors.clear();
		g_undo.clear();
		g_redo.clear();
	}
	g_reload = true;
	g_selected = -1;
	for (std::size_t i = 0; i < g_actors.size(); ++i)
		if (g_actors[i].name == name) g_selected = static_cast<int>(i);
	if (g_selected < 0) g_pendingSelect = name;
}

std::string level_editor::PlaceWhereLooking(const std::string& cls)
{
	std::string mod, name;
	{
		std::lock_guard lock(g_mutex);
		mod = g_mod;
		name = UniqueName(g_actors, ShortClass(cls));
	}
	if (mod.empty() || cls.empty()) return {};
	game::RunOnGameThread([mod, name, cls] {
		layers::EditActor a;
		a.name = name;
		a.cls = cls;
		if (!LookAt(a.location)) {
			Status("no camera or player to place from");
			return;
		}
		if (auto* pc = game::Controller()) a.rotation[1] = pc->Rotation.Yaw & 0xFFFF;
		{
			std::lock_guard lock(g_mutex);
			g_actors.push_back(a);
			g_selected = static_cast<int>(g_actors.size()) - 1;
		}
		Save("placed " + name);
	});
	return name;
}

void level_editor::Undo() { Step(true); }
void level_editor::Redo() { Step(false); }

void level_editor::Duplicate()
{
	float grid;
	{
		std::lock_guard lock(g_mutex);
		grid = g_grid;
	}
	WithSelected("duplicated", [grid](const std::string&, EntryList& actors, int& selected) {
		layers::EditActor copy = actors[static_cast<std::size_t>(selected)];
		std::string base = copy.name;
		if (const auto us = base.find_last_of('_'); us != std::string::npos && us + 1 < base.size() &&
													 std::all_of(base.begin() + static_cast<std::ptrdiff_t>(us) + 1, base.end(), ::isdigit))
			base.resize(us);
		copy.name = UniqueName(actors, base);
		copy.location[0] += std::max(100.f, grid);		// Beside it, not inside it (a grid step when snapping)
		actors.push_back(copy);
		selected = static_cast<int>(actors.size()) - 1;
		return true;
	});
}

void level_editor::DeleteSelected()
{
	WithSelected("deleted", [](const std::string&, EntryList& actors, int& selected) {
		actors.erase(actors.begin() + selected);
		selected = -1;
		return true;
	});
}

void level_editor::DropToFloor()
{
	WithSelected("dropped to the floor", [](const std::string& mod, EntryList& actors, int& selected) {
		auto& a = actors[static_cast<std::size_t>(selected)];
		auto* live = layers::Find(mod, a.name);
		const float offset = BottomOffset(live);
		// From just above its bottom straight down, the actor itself not in the way (world geometry only)
		const Classes::FVector start{a.location[0], a.location[1], a.location[2] - offset + 2.f};
		const Classes::FVector end{start.X, start.Y, start.Z - 10000.f};
		Classes::FVector hit{}, normal{};
		if (!Trace(start, end, false, hit, normal)) {
			Status("no floor under it");
			return false;
		}
		a.location[2] = hit.Z + offset;
		return true;
	});
}

void level_editor::BringHere()
{
	WithSelected("brought to where you look", [](const std::string&, EntryList& actors, int& selected) {
		auto& a = actors[static_cast<std::size_t>(selected)];
		return LookAt(a.location);
	});
}

void level_editor::GoTo()
{
	game::RunOnGameThread([] {
		float at[3];
		{
			std::lock_guard lock(g_mutex);
			if (g_selected < 0 || g_selected >= static_cast<int>(g_actors.size())) return;
			std::copy(g_actors[static_cast<std::size_t>(g_selected)].location, g_actors[static_cast<std::size_t>(g_selected)].location + 3, at);
		}
		if (auto* pawn = game::Pawn()) {
			pawn->Location = Classes::FVector{at[0] - 150.f, at[1], at[2] + 100.f};	// Beside it and a little above: falls into place
			pawn->Velocity = Classes::FVector{0.f, 0.f, 0.f};
			Status("went to it");
		}
	});
}

void level_editor::SetSnap(float grid, float angleDegrees, float scaleStep)
{
	std::lock_guard lock(g_mutex);
	g_grid = std::max(0.f, grid);
	g_angle = std::max(0.f, angleDegrees);
	g_scaleStep = std::max(0.f, scaleStep);
}

void level_editor::SnapSelected()
{
	float grid, angle, scaleStep;
	{
		std::lock_guard lock(g_mutex);
		grid = g_grid;
		angle = g_angle;
		scaleStep = g_scaleStep;
	}
	WithSelected("snapped", [grid, angle, scaleStep](const std::string&, EntryList& actors, int& selected) {
		auto& a = actors[static_cast<std::size_t>(selected)];
		for (float& v : a.location) v = SnapTo(v, grid);
		if (angle > 0.f)
			for (int& r : a.rotation) r = static_cast<int>(SnapTo(static_cast<float>(r), angle * kRotationPerDegree)) & 0xFFFF;
		a.scale = std::max(0.05f, SnapTo(a.scale, scaleStep));
		return true;
	});
}

std::vector<layers::EditActor> level_editor::Entries()
{
	std::lock_guard lock(g_mutex);
	return g_actors;
}

int level_editor::Selected()
{
	std::lock_guard lock(g_mutex);
	return g_selected;
}

std::size_t level_editor::UndoDepth()
{
	std::lock_guard lock(g_mutex);
	return g_undo.size();
}

std::vector<std::string> level_editor::PlaceableClasses()
{
	std::vector<std::string> out;
	Classes::UClass* actor = Classes::AActor::StaticClass();
	for (Classes::UObject* o : game::ObjectsOf(Classes::UClass::StaticClass())) {
		auto* cls = static_cast<Classes::UClass*>(o);
		const std::uint32_t flags = mirage::classes::ClassFlags(cls);
		if (!(flags & kClassPlaceable) || (flags & kClassAbstract)) continue;
		bool isActor = false;
		int depth = 0;
		for (Classes::UStruct* s = cls; s && depth < 64 && !isActor; s = game::SuperOf(s), ++depth) isActor = s == actor;
		if (isActor) out.push_back(cls->GetPathName());
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::vector<level_editor::Property> level_editor::PropertiesOf(const std::string& name)
{
	std::string mod;
	{
		std::lock_guard lock(g_mutex);
		mod = g_mod;
	}
	std::vector<Property> out;
	Classes::AActor* a = layers::Find(mod, name);
	if (!a || !a->Class) return out;
	for (const reflect::Member* m : reflect::List(a->Class)) {
		if (!(m->flags & kPropertyEdit) || m->arrayDim != 1) continue;
		Property p;
		p.name = m->name;
		if (ValueText(a, *m, p.value, p.kind)) out.push_back(std::move(p));
	}
	std::sort(out.begin(), out.end(), [](const Property& x, const Property& y) { return x.name < y.name; });
	return out;
}
