// The in-game Level Editor (see level_editor.h): a window over a mod's layer actors for the current level. Edits are
// queued to the game thread, where the working copy lives with the layers; the window draws a copy of it.

#include "features/misc/level_editor.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "render/math.h"
#include "runtime/mods.h"
#include "theme.h"

#include <imgui.h>

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
	}

	bool Open(const std::string& modId, std::string* error)
	{
		std::vector<EditActor> actors;
		if (!mirage::layers::Read(modId, actors, error)) return false;
		std::lock_guard lock(g_lock);
		g_mod = modId;
		g_actors = std::move(actors);
		g_dirty = false;
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
					if (m.loaded && ImGui::Selectable(m.id.c_str(), m.id == mod)) Run([id = m.id] { level_editor::Open(id); }), selected_ = -1;
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::TextDisabled("levels/%s.toml", map.empty() ? "(no level)" : map.c_str());
			if (mod.empty()) return;

			ImGui::BeginDisabled(!dirty);
			if (ImGui::Button("Save")) Run([] { level_editor::Save(); });
			ImGui::SameLine();
			if (ImGui::Button("Revert")) Run([mod] { level_editor::Open(mod); }), selected_ = -1;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(240.f);
			ImGui::InputText("##class", class_, sizeof class_);
			ImGui::SameLine();
			if (ImGui::Button("Add at view") && class_[0]) Run([cls = std::string(class_)] { level_editor::Add(cls); });

			ImGui::BeginChild("##list", {200.f, 0.f}, ImGuiChildFlags_Borders);
			for (int i = 0; i < static_cast<int>(actors.size()); ++i) {
				const bool spawned = i < static_cast<int>(live.size()) && live[static_cast<std::size_t>(i)];
				ImGui::PushID(i);
				if (ImGui::Selectable(actors[i].name.empty() ? "(unnamed)" : actors[i].name.c_str(), i == selected_)) selected_ = i;
				ImGui::SameLine(160.f);
				ImGui::TextDisabled("%s", spawned ? "" : "new");
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##entry");
			if (selected_ >= 0 && selected_ < static_cast<int>(actors.size())) DrawEntry(selected_, actors[selected_]);
			else ImGui::TextDisabled("Select an actor, or add one at the view.");
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
				selected_ = -1;
			}
			if (!a.properties.empty()) ImGui::TextDisabled("%zu propert%s, kept as saved", a.properties.size(), a.properties.size() == 1 ? "y" : "ies");
		}

		template <class F>
		static void Run(F&& f) { game::RunOnGameThread(std::forward<F>(f)); }

		std::mutex view_;
		std::vector<char> live_;
		std::string map_;
		int selected_ = -1;
		char class_[256] = "Engine.DynamicSMActor_Spawnable";
	};
	MEM_MODULE(LevelEditor)
}
