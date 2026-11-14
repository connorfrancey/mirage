// The Level Editor (Mirage, step 13): places actors into a mod's level layer (levels/<map>.toml, runtime/layers.h)
// while playing. Pick a mod and a class (the mod's own .uc classes or any class path), place one where the camera
// looks, then move, turn and scale it with the gizmo in the world (or type its values), and edit its properties. Every
// change is saved to the mod's folder and the layer is spawned again from it; Fly (No Clip) is the free camera.

#include "features/misc/level_editor.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "render/math.h"
#include "runtime/call.h"
#include "runtime/layers.h"
#include "runtime/mods.h"
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
	namespace fs = std::filesystem;

	std::atomic<std::uint64_t> g_frames{0}, g_gizmoFrames{0};

	// What the window edits: shared between the render thread (the window) and the game thread (reads, saves, moves)
	std::mutex g_mutex;
	std::string g_mod;							// The mod whose layer is edited
	std::string g_map;							// The level it was read for
	std::vector<layers::EditActor> g_actors;	// Its entries as last read or edited
	int g_selected = -1;
	std::string g_status;
	bool g_reload = true;						// Read the layer again (mod or level changed, a save)
	std::string g_pendingSelect;				// Select this entry once the layer is read

	void Status(std::string text)
	{
		std::lock_guard lock(g_mutex);
		g_status = std::move(text);
	}

	// Game thread: the layer read again
	void ReadNow()
	{
		std::string mod;
		{
			std::lock_guard lock(g_mutex);
			mod = g_mod;
		}
		std::vector<layers::EditActor> actors;
		std::string error;
		const bool ok = mod.empty() || layers::Read(mod, actors, &error);
		std::lock_guard lock(g_mutex);
		g_map = layers::CurrentMap();
		if (ok) g_actors = std::move(actors);
		else g_status = "the layer doesn't parse: " + error;
		if (!g_pendingSelect.empty())
			for (std::size_t i = 0; i < g_actors.size(); ++i)
				if (g_actors[i].name == g_pendingSelect) {
					g_selected = static_cast<int>(i);
					g_pendingSelect.clear();
				}
		if (g_selected >= static_cast<int>(g_actors.size())) g_selected = -1;
	}

	// Saves the entries (any thread: posted to the game thread)
	void Save()
	{
		std::string mod;
		std::vector<layers::EditActor> actors;
		{
			std::lock_guard lock(g_mutex);
			mod = g_mod;
			actors = g_actors;
		}
		game::RunOnGameThread([mod, actors] {
			std::string error;
			Status(layers::Write(mod, actors, &error) ? "saved levels/" + layers::CurrentMap() + ".toml" : "not saved: " + error);
		});
	}

	// Game thread: the spawned actor of entry `a` moved to where the entry says (the gizmo, the fields)
	void PlaceLive(const std::string& mod, const layers::EditActor& a)
	{
		layers::Place(layers::Find(mod, a.name), a.location, a.rotation, a.scale);
	}

	// Game thread: where the camera looks, on the world (a trace from the player), or 500 units ahead
	bool LookAt(float out[3])
	{
		const rmath::Camera c = game::GetCamera();
		auto* pawn = game::Pawn();
		if (!c.valid || !pawn) return false;
		constexpr float kTurn = 6.2831853f / 65536.f;
		const float pitch = static_cast<float>(c.pitch) * kTurn, yaw = static_cast<float>(c.yaw) * kTurn;
		const Classes::FVector dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
		const Classes::FVector start{c.pos.x, c.pos.y, c.pos.z};
		const Classes::FVector end{start.X + dir.X * 5000.f, start.Y + dir.Y * 5000.f, start.Z + dir.Z * 5000.f};
		mirage::call::Params trace(pawn, "Trace");
		if (trace.Valid() && trace.SetRaw("TraceEnd", &end, sizeof end) && trace.SetRaw("TraceStart", &start, sizeof start) &&
			trace.Set("bTraceActors", true) && mirage::call::Call(pawn, trace) && trace.GetObject("ReturnValue")) {
			Classes::FVector hit{}, normal{};
			trace.GetRaw("HitLocation", &hit, sizeof hit);
			trace.GetRaw("HitNormal", &normal, sizeof normal);
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

	std::string ShortClass(const std::string& cls)
	{
		std::string s = cls;
		if (s.size() > 3 && s.substr(s.size() - 3) == ".uc") s.resize(s.size() - 3);
		s = s.substr(s.find_last_of("./\\") + 1);
		return s.empty() ? "Actor" : s;
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
			bool reload;
			{
				std::lock_guard lock(g_mutex);
				reload = g_reload || layers::CurrentMap() != g_map;
				g_reload = false;
			}
			if (reload) ReadNow();
		}

		void OnRender() override
		{
			if (!gui::visible) return;		// A tool to work with the mouse: with the menu open
			++g_frames;
			DrawGizmo();
			theme::Begin();
			ImGui::SetNextWindowPos({340.f, 160.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({460.f, 520.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({360.f, 260.f}, {FLT_MAX, FLT_MAX});
			if (ImGui::Begin("Level Editor", nullptr, ImGuiWindowFlags_NoCollapse)) DrawWindow();
			ImGui::End();
			theme::End();
		}

	private:
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
			const auto mod = std::find_if(list.begin(), list.end(), [&](const auto& m) { return m.id == g_mod; });
			if (ImGui::BeginCombo("##classes", "Classes of the mod")) {
				std::error_code ec;
				if (mod != list.end())
					for (const auto& e : fs::directory_iterator(mod->dir / "classes", ec))
						if (e.path().extension() == ".uc") {
							const std::string rel = "classes/" + e.path().filename().string();
							if (ImGui::Selectable(rel.c_str())) std::snprintf(cls_, sizeof cls_, "%s", rel.c_str());
						}
				for (const char* c : {"Engine.Trigger", "Engine.PointLight", "Engine.Note"})
					if (ImGui::Selectable(c)) std::snprintf(cls_, sizeof cls_, "%s", c);
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputText("##class", cls_, sizeof cls_);
			if (ImGui::Button("Place where I look") && cls_[0]) {
				g_mutex.unlock();
				level_editor::PlaceWhereLooking(cls_);
				g_mutex.lock();
			}
			if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());
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
			bool changed = false;
			char name[96];
			std::snprintf(name, sizeof name, "%s", a.name.c_str());
			if (ImGui::InputText("Name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue)) {
				a.name = name;
				changed = true;
			}
			bool moved = ImGui::DragFloat3("Location", a.location, 2.f);
			moved |= ImGui::DragInt3("Rotation", a.rotation, 64.f);
			moved |= ImGui::DragFloat("Scale", &a.scale, 0.01f, 0.05f, 50.f);
			if (moved) PostPlace(a);
			changed |= moved && !ImGui::IsMouseDown(ImGuiMouseButton_Left);		// Typed: saved now; dragged: on release
			if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;

			ImGui::SeparatorText("Properties");
			for (std::size_t i = 0; i < a.properties.size(); ++i) {
				ImGui::PushID(static_cast<int>(i));
				char value[160];
				std::snprintf(value, sizeof value, "%s", a.properties[i].second.c_str());
				ImGui::SetNextItemWidth(160.f);
				if (ImGui::InputText(a.properties[i].first.c_str(), value, sizeof value, ImGuiInputTextFlags_EnterReturnsTrue)) {
					a.properties[i].second = value;
					changed = true;
				}
				ImGui::SameLine();
				if (ImGui::SmallButton("x")) {
					a.properties.erase(a.properties.begin() + static_cast<std::ptrdiff_t>(i));
					changed = true;
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::SetNextItemWidth(120.f);
			ImGui::InputText("##newkey", newKey_, sizeof newKey_);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120.f);
			ImGui::InputText("##newvalue", newValue_, sizeof newValue_);
			ImGui::SameLine();
			if (ImGui::Button("Add") && newKey_[0]) {
				a.properties.emplace_back(newKey_, newValue_);
				newKey_[0] = newValue_[0] = 0;
				changed = true;
			}
			ImGui::Separator();
			if (ImGui::Button("Delete")) {
				g_actors.erase(g_actors.begin() + g_selected);
				g_selected = -1;
				changed = true;
			}
			if (changed) {
				g_mutex.unlock();
				Save();
				g_mutex.lock();
			}
		}

		void PostPlace(const layers::EditActor& a)
		{
			game::RunOnGameThread([mod = g_mod, a] { PlaceLive(mod, a); });
		}

		// Arrows (move along X, Y, Z), a ring (turn) and a handle (scale) on the selected actor; dragging one moves the
		// actor live, letting go saves
		void DrawGizmo()
		{
			std::lock_guard lock(g_mutex);
			if (g_selected < 0 || g_selected >= static_cast<int>(g_actors.size())) {
				drag_ = -1;
				return;
			}
			auto& a = g_actors[static_cast<std::size_t>(g_selected)];
			ImVec2 c;
			if (!Project(a.location, c)) return;
			++g_gizmoFrames;
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const ImGuiIO& io = ImGui::GetIO();
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

			const auto near = [&](ImVec2 p, float r) { return std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < r; };
			if (drag_ < 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse) {
				for (int k = 0; k < 3; ++k)
					if (have[k] && near(ends[k], 12.f)) drag_ = k;
				if (drag_ < 0 && near(scaleHandle, 10.f)) drag_ = 4;
				if (drag_ < 0 && std::fabs(std::hypot(io.MousePos.x - ring.x, io.MousePos.y - ring.y) - 34.f) < 6.f) drag_ = 3;
			}
			if (drag_ < 0) return;
			if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {		// Let go: saved
				drag_ = -1;
				g_mutex.unlock();
				Save();
				g_mutex.lock();
				return;
			}
			const ImVec2 d = io.MouseDelta;
			if (d.x == 0.f && d.y == 0.f) return;
			if (drag_ < 3) {
				const ImVec2 axis{ends[drag_].x - c.x, ends[drag_].y - c.y};
				const float len2 = axis.x * axis.x + axis.y * axis.y;
				if (len2 > 1.f) a.location[drag_] += (d.x * axis.x + d.y * axis.y) / len2 * kLength;
			} else if (drag_ == 3) {
				a.rotation[1] = (a.rotation[1] + static_cast<int>(d.x * 120.f)) & 0xFFFF;
			} else {
				a.scale = std::clamp(a.scale * (1.f - d.y * 0.01f), 0.05f, 50.f);
			}
			PostPlace(a);
		}

		char cls_[160] = "";
		char newKey_[64] = "";
		char newValue_[96] = "";
		int drag_ = -1;			// 0..2: an axis, 3: the ring, 4: scale
		bool flying_ = false;	// No Clip was switched on from here
	};
	MEM_MODULE(LevelEditor)
}

std::uint64_t level_editor::Frames() { return g_frames.load(); }
std::uint64_t level_editor::GizmoFrames() { return g_gizmoFrames.load(); }

void level_editor::Select(const std::string& mod, const std::string& name)
{
	std::lock_guard lock(g_mutex);
	if (g_mod != mod) {
		g_mod = mod;
		g_actors.clear();
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
		const std::string base = ShortClass(cls);
		for (int n = 1;; ++n) {
			name = base + "_" + std::to_string(n);
			if (std::none_of(g_actors.begin(), g_actors.end(), [&](const layers::EditActor& a) { return a.name == name; })) break;
		}
	}
	if (mod.empty()) return {};
	game::RunOnGameThread([mod, name, cls] {
		layers::EditActor a;
		a.name = name;
		a.cls = cls;
		if (!LookAt(a.location)) {
			Status("no camera or player to place from");
			return;
		}
		if (auto* pc = game::Controller()) a.rotation[1] = pc->Rotation.Yaw & 0xFFFF;
		ReadNow();
		std::vector<layers::EditActor> actors;
		{
			std::lock_guard lock(g_mutex);
			g_actors.push_back(a);
			g_selected = static_cast<int>(g_actors.size()) - 1;
			actors = g_actors;
		}
		std::string error;
		Status(layers::Write(mod, actors, &error) ? "placed " + name : "not saved: " + error);
	});
	return name;
}
