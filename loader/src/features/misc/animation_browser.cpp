// The Animation Browser (see animation_browser.h). The rows and the slot's state are read on the game thread (OnTick,
// while the window is open) and drawn on the render thread from a copy under the lock.

#include "features/misc/animation_browser.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "runtime/anim.h"
#include "sdk/SDK.hpp"
#include "theme.h"

#include <fmt/format.h>
#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <map>
#include <mutex>

namespace
{
	struct Row
	{
		std::string name, set, meshes;		// meshes: "Mesh1p, Mesh3p"
		float length = 0.f;
		int frames = 0;
	};

	// CustomNodeType, in the engine's order
	const char* const kSlots[] = {"Canned", "CannedUpperBody", "FullBody", "FullBody_Dir", "UpperBody", "LowerBody", "Camera", "Weapon", "Face"};

	std::mutex g_lock;
	std::vector<Row> g_rows;
	mirage::anim::State g_state;
	std::string g_status;
	int g_slot = 2;				// FullBody: the first-person arms and the third-person body together
	float g_rate = 1.f;
	bool g_loop = false;
	bool g_refresh = true;
	const void* g_pawn = nullptr;		// The pawn the rows were read from: a new one (a level load, a restart) reads them again

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	// The pawn's sequences as rows, sorted by name; a sequence on both meshes once, with both named
	std::vector<Row> ReadRows(Classes::AActor* pawn)
	{
		std::vector<Row> rows;
		const auto sets = nlohmann::json::parse(mirage::anim::SetsJson(pawn), nullptr, false);
		if (!sets.is_array()) return rows;
		std::map<std::string, std::size_t> seen;		// set + sequence -> row
		for (const auto& m : sets) {
			const std::string mesh = m.value("mesh", std::string());
			for (const auto& set : m["sets"]) {
				const std::string setName = set.value("name", std::string());
				for (const auto& q : set["sequences"]) {
					const std::string name = q.value("name", std::string());
					const auto [it, added] = seen.try_emplace(setName + "." + name, rows.size());
					if (!added) {
						rows[it->second].meshes += ", " + mesh;
						continue;
					}
					rows.push_back({name, setName, mesh, q.value("length", 0.f), q.value("frames", 0)});
				}
			}
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return Lower(a.name) < Lower(b.name); });
		return rows;
	}

	class AnimationBrowser final : public Module
	{
	public:
		AnimationBrowser() : Module("Animation Browser", "Misc", "Every animation Faith can play, previewed on her with a click", 4) {}

		void OnTick() override
		{
			Classes::ATdPlayerPawn* pawn = game::Pawn();
			if (!pawn) return;
			std::lock_guard lock(g_lock);
			if (g_refresh || g_pawn != pawn) {
				g_rows = ReadRows(pawn);
				g_pawn = pawn;
				g_refresh = false;
			}
			g_state = mirage::anim::Get(pawn, g_slot);
		}

		void OnRender() override
		{
			if (!gui::visible) return;
			theme::Begin();
			ImGui::SetNextWindowPos({340.f, 120.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({640.f, 520.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({420.f, 240.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Animation Browser", &open, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		char filter_[128] = {};

		void Draw()
		{
			std::lock_guard lock(g_lock);
			ImGui::SetNextItemWidth(200.f);
			ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS " Filter", filter_, sizeof filter_);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(130.f);
			ImGui::Combo("Slot", &g_slot, kSlots, IM_ARRAYSIZE(kSlots));
			ImGui::SameLine();
			ImGui::SetNextItemWidth(100.f);
			ImGui::SliderFloat("Rate", &g_rate, 0.1f, 3.f, "%.2f");
			ImGui::SameLine();
			ImGui::Checkbox("Loop", &g_loop);
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_STOP " Stop")) game::RunOnGameThread([] { animation_browser::Stop(); });
			ImGui::SameLine();
			if (ImGui::Button(ICON_FA_ROTATE)) g_refresh = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Read the player's animations again");

			// The slot's sequence and where it is
			const float fraction = g_state.length > 0.f ? std::clamp(g_state.position / g_state.length, 0.f, 1.f) : 0.f;
			const std::string overlay = g_state.playing ? fmt::format("{}  {:.2f} / {:.2f} s", g_state.sequence, g_state.position, g_state.length)
														: std::string("nothing playing on this slot");
			ImGui::ProgressBar(g_state.playing ? fraction : 0.f, {-FLT_MIN, 0.f}, overlay.c_str());
			if (!g_status.empty()) ImGui::TextDisabled("%s", g_status.c_str());

			const std::string needle = Lower(filter_);
			std::vector<int> shown;
			for (int i = 0; i < static_cast<int>(g_rows.size()); ++i)
				if (needle.empty() || Lower(g_rows[i].name).find(needle) != std::string::npos || Lower(g_rows[i].set).find(needle) != std::string::npos)
					shown.push_back(i);
			ImGui::TextDisabled("%zu of %zu animations; click one to play it", shown.size(), g_rows.size());
			if (g_rows.empty()) ImGui::TextDisabled("No player in the world yet");

			if (!ImGui::BeginTable("##anims", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
				return;
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Animation", ImGuiTableColumnFlags_WidthStretch, 2.f);
			ImGui::TableSetupColumn("Set", ImGuiTableColumnFlags_WidthStretch, 1.5f);
			ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, 60.f);
			ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 50.f);
			ImGui::TableSetupColumn("Meshes", ImGuiTableColumnFlags_WidthStretch, 1.f);
			ImGui::TableHeadersRow();
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(shown.size()));
			while (clipper.Step()) {
				for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
					const Row& r = g_rows[shown[k]];
					ImGui::PushID(shown[k]);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (ImGui::Selectable(r.name.c_str(), g_state.playing && g_state.sequence == r.name, ImGuiSelectableFlags_SpanAllColumns))
						game::RunOnGameThread([name = r.name] {
							std::string error;
							animation_browser::Play(name, &error);
						});
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(r.set.c_str());
					ImGui::TableNextColumn();
					ImGui::Text("%.2f s", r.length);
					ImGui::TableNextColumn();
					ImGui::Text("%d", r.frames);
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", r.meshes.c_str());
					ImGui::PopID();
				}
			}
			ImGui::EndTable();
		}
	};
	MEM_MODULE(AnimationBrowser)
}

namespace animation_browser
{
	std::vector<std::string> Refresh()
	{
		std::vector<std::string> names;
		Classes::ATdPlayerPawn* pawn = game::Pawn();
		if (!pawn) return names;
		std::lock_guard lock(g_lock);
		g_rows = ReadRows(pawn);
		g_pawn = pawn;
		g_refresh = false;
		for (const auto& r : g_rows) names.push_back(r.name);
		return names;
	}

	bool Play(const std::string& sequence, std::string* error)
	{
		Classes::ATdPlayerPawn* pawn = game::Pawn();
		std::lock_guard lock(g_lock);
		std::string why = "no player";
		const bool ok = pawn && mirage::anim::Play(pawn, g_slot, sequence, g_rate, 0.15f, 0.15f, g_loop, false, &why);
		g_status = ok ? "playing " + sequence + " on " + kSlots[g_slot] : why;
		if (!ok && error) *error = why;
		return ok;
	}

	void Stop()
	{
		if (Classes::ATdPlayerPawn* pawn = game::Pawn()) {
			std::lock_guard lock(g_lock);
			mirage::anim::Stop(pawn, g_slot, 0.2f);
			g_status.clear();
		}
	}
}
