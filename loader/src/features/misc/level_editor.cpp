// The in-game Level Editor (see level_editor.h): a window over a mod's layer actors for the current level. Edits are
// queued to the game thread, where the working copy lives with the layers; the window draws a copy of it.

#include "features/misc/level_editor.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "render/math.h"
#include "runtime/mod_world.h"
#include "runtime/mods.h"
#include "sdk/SDK.hpp"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

namespace level_editor
{
	namespace
	{
		using mirage::layers::EditActor;

		std::mutex g_lock;
		std::string g_mod;
		std::vector<EditActor> g_actors;
		bool g_dirty = false;
		int g_selected = -1;

		// The camera's axes (rows of its rotation matrix: forward, right, up) and the projection scale for a w-wide view
		struct View
		{
			rmath::Vec3 at, forward, right, up;
			float scale;
		};
		View ViewOf(const rmath::Camera& c, float w)
		{
			const rmath::Mat4 m = rmath::RotationMatrix(c.pitch, c.yaw, c.roll);
			const float fov = (c.fov > 1.f ? c.fov : 90.f) * rmath::kPi / 180.f;
			return {c.pos, {m.m[0][0], m.m[0][1], m.m[0][2]}, {m.m[1][0], m.m[1][1], m.m[1][2]}, {m.m[2][0], m.m[2][1], m.m[2][2]},
					(w * 0.5f) / std::tan(fov * 0.5f)};
		}
	}

	bool Project(const rmath::Camera& camera, const float world[3], float w, float h, float& x, float& y)
	{
		if (!camera.valid) return false;
		const View v = ViewOf(camera, w);
		const rmath::Vec3 d = rmath::Vec3{world[0], world[1], world[2]} - v.at;
		const float z = rmath::Dot(d, v.forward);
		if (z < 1.f) return false;
		x = w * 0.5f + rmath::Dot(d, v.right) / z * v.scale;
		y = h * 0.5f - rmath::Dot(d, v.up) / z * v.scale;
		return true;
	}

	int Pick(float x, float y, float w, float h)
	{
		const rmath::Camera camera = game::GetCamera();
		std::lock_guard lock(g_lock);
		g_selected = -1;
		if (!camera.valid) return -1;
		const View v = ViewOf(camera, w);
		const rmath::Vec3 ray = rmath::Normalize(v.forward + v.right * ((x - w * 0.5f) / v.scale) + v.up * ((h * 0.5f - y) / v.scale));
		float best = 150.f;
		for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
			auto* a = mirage::layers::Find(g_mod, g_actors[i].name);
			if (!a) continue;
			const rmath::Vec3 d = rmath::Vec3{a->Location.X, a->Location.Y, a->Location.Z} - v.at;
			const float along = rmath::Dot(d, ray);
			if (along < 0.f) continue;
			const float off = rmath::Length(d - ray * along);
			if (off < best) best = off, g_selected = i;
		}
		return g_selected;
	}

	void Drag(int axis, float value, float grid)
	{
		std::lock_guard lock(g_lock);
		if (g_selected < 0 || g_selected >= static_cast<int>(g_actors.size()) || axis < 0 || axis > 2) return;
		EditActor& a = g_actors[g_selected];
		a.location[axis] = grid > 0.f ? std::round(value / grid) * grid : value;
		if (auto* actor = mirage::layers::Find(g_mod, a.name)) mirage::layers::Place(actor, a.location, a.rotation, a.scale);
		g_dirty = true;
	}

	int Selected()
	{
		std::lock_guard lock(g_lock);
		return g_selected;
	}
	void Select(int i)
	{
		std::lock_guard lock(g_lock);
		g_selected = i;
	}

	bool Open(const std::string& modId, std::string* error)
	{
		std::vector<EditActor> actors;
		if (!mirage::layers::Read(modId, actors, error)) return false;
		std::lock_guard lock(g_lock);
		g_mod = modId;
		g_actors = std::move(actors);
		g_dirty = false;
		g_selected = -1;
		return true;
	}

	int Add(const std::string& cls)
	{
		EditActor a;
		a.cls = cls;
		if (const rmath::Camera cam = game::GetCamera(); cam.valid) {
			const float k = 2.f * rmath::kPi / 65536.f, p = static_cast<float>(cam.pitch & 0xFFFF) * k,
						y = static_cast<float>(cam.yaw & 0xFFFF) * k;
			a.location[0] = cam.pos.x + std::cos(p) * std::cos(y) * 400.f;
			a.location[1] = cam.pos.y + std::cos(p) * std::sin(y) * 400.f;
			a.location[2] = cam.pos.z + std::sin(p) * 400.f;
			a.rotation[1] = cam.yaw;
		}
		std::lock_guard lock(g_lock);
		for (int n = 1;; ++n) {
			a.name = "Actor" + std::to_string(n);
			bool used = false;
			for (const EditActor& e : g_actors) used |= e.name == a.name;
			if (!used) break;
		}
		g_actors.push_back(std::move(a));
		g_dirty = true;
		return static_cast<int>(g_actors.size()) - 1;
	}

	void Set(int i, const EditActor& a)
	{
		std::lock_guard lock(g_lock);
		if (i < 0 || i >= static_cast<int>(g_actors.size())) return;
		if (g_actors[i].name == a.name)
			if (auto* actor = mirage::layers::Find(g_mod, a.name)) mirage::layers::Place(actor, a.location, a.rotation, a.scale);
		g_actors[i] = a;
		g_dirty = true;
	}

	void Remove(int i)
	{
		std::lock_guard lock(g_lock);
		if (i < 0 || i >= static_cast<int>(g_actors.size())) return;
		g_actors.erase(g_actors.begin() + i);
		g_dirty = true;
		g_selected = -1;
	}

	bool Save(std::string* error)
	{
		std::lock_guard lock(g_lock);
		if (g_mod.empty()) {
			if (error) *error = "no mod open";
			return false;
		}
		if (!mirage::layers::Write(g_mod, g_actors, error)) return false;
		g_dirty = false;
		return true;
	}

	Classes::AActor* Live(int i)
	{
		std::lock_guard lock(g_lock);
		if (i < 0 || i >= static_cast<int>(g_actors.size())) return nullptr;
		return mirage::layers::Find(g_mod, g_actors[i].name);
	}

	std::string Mod()
	{
		std::lock_guard lock(g_lock);
		return g_mod;
	}
	std::vector<EditActor> Actors()
	{
		std::lock_guard lock(g_lock);
		return g_actors;
	}
	bool Dirty()
	{
		std::lock_guard lock(g_lock);
		return g_dirty;
	}
}

namespace
{
	using mirage::layers::EditActor;

	class LevelEditor final : public Module
	{
	public:
		LevelEditor() : Module("Level Editor", "Misc", "Places, moves and saves a mod's actors in the current level (its layer)", 2) {}

		// The gizmo and picking project with the game's camera: asking for it once starts it being followed
		void OnEnable() override
		{
			MirageCamera c{};
			c.size = sizeof c;
			mirage::mods::world::CameraGet(&c);
		}

		// Game thread: which entries have a spawned actor, and the map, for the window
		void OnTick() override
		{
			const int n = static_cast<int>(level_editor::Actors().size());
			std::vector<char> live(static_cast<std::size_t>(n));
			for (int i = 0; i < n; ++i) live[static_cast<std::size_t>(i)] = level_editor::Live(i) != nullptr;
			std::string map = mirage::layers::CurrentMap();
			std::lock_guard lock(view_);
			live_ = std::move(live);
			map_ = std::move(map);
		}

		void OnRender() override
		{
			if (!gui::visible) return;
			theme::Begin();
			ImGui::SetNextWindowPos({360.f, 140.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({620.f, 420.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({420.f, 260.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Level Editor", &open, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			if (!level_editor::Mod().empty()) Gizmo();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		void Draw()
		{
			std::vector<char> live;
			std::string map;
			{
				std::lock_guard lock(view_);
				live = live_;
				map = map_;
			}
			const std::string mod = level_editor::Mod();
			const auto actors = level_editor::Actors();
			const bool dirty = level_editor::Dirty();

			ImGui::SetNextItemWidth(220.f);
			if (ImGui::BeginCombo("Mod", mod.empty() ? "(choose a mod)" : mod.c_str())) {
				for (const auto& m : mirage::mods::List())
					if (m.loaded && ImGui::Selectable(m.id.c_str(), m.id == mod)) Run([id = m.id] { level_editor::Open(id); });
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("levels/%s.toml", map.empty() ? "(no level)" : map.c_str());
			if (mod.empty()) return;

			ImGui::BeginDisabled(!dirty);
			if (ImGui::Button("Save")) Run([] { level_editor::Save(); });
			ImGui::SameLine();
			if (ImGui::Button("Revert")) Run([mod] { level_editor::Open(mod); });
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(240.f);
			ImGui::InputText("##class", class_, sizeof class_);
			ImGui::SameLine();
			if (ImGui::Button("Add at view") && class_[0]) Run([cls = std::string(class_)] { level_editor::Add(cls); });
			ImGui::SameLine();
			ImGui::SetNextItemWidth(70.f);
			ImGui::InputFloat("Grid", &grid_, 0.f, 0.f, "%.0f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Units the gizmo snaps to (0: off)");
			const int selected = level_editor::Selected();

			ImGui::BeginChild("##list", {200.f, 0.f}, ImGuiChildFlags_Borders);
			for (int i = 0; i < static_cast<int>(actors.size()); ++i) {
				const bool spawned = i < static_cast<int>(live.size()) && live[static_cast<std::size_t>(i)];
				ImGui::PushID(i);
				if (ImGui::Selectable(actors[i].name.empty() ? "(unnamed)" : actors[i].name.c_str(), i == selected)) level_editor::Select(i);
				ImGui::SameLine(160.f);
				ImGui::TextDisabled("%s", spawned ? "" : "new");
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##entry");
			if (selected >= 0 && selected < static_cast<int>(actors.size())) DrawEntry(selected, actors[selected]);
			else ImGui::TextDisabled("Select an actor (or click it in the world), or add one at the view.");
			ImGui::EndChild();
		}

		void DrawEntry(int i, EditActor a)
		{
			bool changed = false;
			char name[128], cls[256];
			snprintf(name, sizeof name, "%s", a.name.c_str());
			snprintf(cls, sizeof cls, "%s", a.cls.c_str());
			if (ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) a.name = name, changed = true;
			if (ImGui::InputText("Class", cls, sizeof cls, ImGuiInputTextFlags_EnterReturnsTrue)) a.cls = cls, changed = true;
			changed |= ImGui::DragFloat3("Location", a.location, 4.f, 0.f, 0.f, "%.0f");
			changed |= ImGui::DragInt3("Rotation", a.rotation, 64.f);
			changed |= ImGui::DragFloat("Scale", &a.scale, 0.01f, 0.01f, 100.f, "%.2f");
			if (changed) Run([i, a] { level_editor::Set(i, a); });
			if (ImGui::Button("Delete")) {
				Run([i] { level_editor::Remove(i); });
			}
			if (!a.properties.empty()) ImGui::TextDisabled("%zu propert%s, kept as saved", a.properties.size(), a.properties.size() == 1 ? "y" : "ies");
		}

		// The selected entry's axes in the world (X red, Y green, Z blue): drag one to move it along that axis (snapped
		// to the grid when set). A click in the world (not on the menu's windows) picks the actor nearest the cursor.
		void Gizmo()
		{
			constexpr float kLength = 120.f;
			const ImGuiIO& io = ImGui::GetIO();
			const float w = io.DisplaySize.x, h = io.DisplaySize.y;
			const rmath::Camera cam = game::GetCamera();
			const int selected = level_editor::Selected();
			const auto actors = level_editor::Actors();
			ImVec2 origin, tips[3];
			bool shown = false, tip[3]{};
			if (selected >= 0 && selected < static_cast<int>(actors.size())) {
				const float* at = actors[selected].location;
				shown = level_editor::Project(cam, at, w, h, origin.x, origin.y);
				for (int a = 0; a < 3 && shown; ++a) {
					float end[3] = {at[0], at[1], at[2]};
					end[a] += kLength;
					tip[a] = level_editor::Project(cam, end, w, h, tips[a].x, tips[a].y);
				}
				static const ImU32 kColors[3] = {IM_COL32(230, 70, 60, 255), IM_COL32(80, 210, 90, 255), IM_COL32(70, 130, 240, 255)};
				ImDrawList* dl = ImGui::GetBackgroundDrawList();
				for (int a = 0; a < 3; ++a)
					if (tip[a]) {
						dl->AddLine(origin, tips[a], kColors[a], a == dragAxis_ ? 5.f : 3.f);
						dl->AddCircleFilled(tips[a], 5.f, kColors[a]);
					}
				if (shown) dl->AddCircle(origin, 6.f, IM_COL32(255, 255, 255, 220), 0, 2.f);
			}
			if (!ImGui::IsMouseDown(0)) dragAxis_ = -1;
			if (ImGui::IsMouseClicked(0) && !io.WantCaptureMouse) {
				const ImVec2 m = io.MousePos;
				for (int a = 0; a < 3; ++a)
					if (tip[a] && DistanceToSegment(m, origin, tips[a]) < 8.f) {
						dragAxis_ = a;
						dragStart_ = actors[selected].location[a];
						dragDone_ = 0.f;
					}
				if (dragAxis_ < 0) Run([m, w, h] { level_editor::Pick(m.x, m.y, w, h); });
			} else if (dragAxis_ >= 0 && tip[dragAxis_]) {
				// The mouse's movement along the axis as it appears on screen, in world units
				const ImVec2 axis{tips[dragAxis_].x - origin.x, tips[dragAxis_].y - origin.y};
				const float len2 = axis.x * axis.x + axis.y * axis.y;
				if (len2 > 1.f && (io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f)) {
					dragDone_ += (io.MouseDelta.x * axis.x + io.MouseDelta.y * axis.y) / len2 * kLength;
					Run([a = dragAxis_, v = dragStart_ + dragDone_, g = grid_] { level_editor::Drag(a, v, g); });
				}
			}
		}

		static float DistanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b)
		{
			const float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
			const float t = len2 > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.f, 1.f) : 0.f;
			const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
			return std::sqrt(ex * ex + ey * ey);
		}

		template <class F>
		static void Run(F&& f) { game::RunOnGameThread(std::forward<F>(f)); }

		std::mutex view_;
		std::vector<char> live_;
		std::string map_;
		float grid_ = 0.f;
		int dragAxis_ = -1;		// The gizmo axis being dragged (-1: none)
		float dragStart_ = 0.f, dragDone_ = 0.f;	// The coordinate when the drag began, and how far the mouse took it
		char class_[256] = "Engine.DynamicSMActor_Spawnable";
	};
	MEM_MODULE(LevelEditor)
}
