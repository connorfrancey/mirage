// The mod manager (Mirage, step 9): every mod found, loaded or not, with why; its details, modules, dependencies,
// warnings, crash record and log; and what can be done with it: reload it now, switch it on or off for the next start,
// let a crashed one load again, open its folder.

#include "features/misc/mod_manager.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "log.h"
#include "runtime/mods.h"
#include "theme.h"

#include <Windows.h>
#include <shellapi.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <deque>
#include <string>
#include <vector>

namespace
{
	std::atomic<std::uint64_t> g_frames{0};
	std::atomic<std::size_t> g_rows{0};

	class ModManager final : public Module
	{
	public:
		ModManager() : Module("Mod Manager", "Misc", "Every mod, loaded or not: details, switches, dependencies, crashes, log", 1) {}

		void OnRender() override
		{
			Pull();
			if (!gui::visible) return;		// A window to work in: only with the menu open
			++g_frames;
			theme::Begin();
			ImGui::SetNextWindowPos({300.f, 120.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({760.f, 460.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({480.f, 260.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Mod Manager", &open, ImGuiWindowFlags_NoCollapse)) {
				const auto mods = mirage::mods::List();
				g_rows = mods.size();
				if (selected_.empty() && !mods.empty()) selected_ = mods.front().id;
				ImGui::BeginChild("##list", {230.f, 0.f}, ImGuiChildFlags_Borders);
				for (const auto& m : mods) DrawRow(m);
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("##details", {0.f, 0.f}, ImGuiChildFlags_Borders);
				const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == selected_; });
				if (it != mods.end()) DrawDetails(*it);
				else ImGui::TextDisabled("No mods: put mod folders in memenu\\mods");
				ImGui::EndChild();
			}
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		// The mod's state as a color and a word
		static ImVec4 StateColor(const mirage::mods::Info& m)
		{
			if (m.crashed) return {0.95f, 0.35f, 0.3f, 1.f};
			if (m.offNextStart && !m.loaded) return {0.55f, 0.55f, 0.58f, 1.f};
			if (m.loaded) return m.warnings.empty() ? ImVec4{0.4f, 0.85f, 0.45f, 1.f} : ImVec4{0.95f, 0.8f, 0.3f, 1.f};
			return {0.95f, 0.6f, 0.25f, 1.f};
		}
		static const char* StateText(const mirage::mods::Info& m)
		{
			if (m.crashed) return "crashed";
			if (m.loaded) return m.offNextStart ? "loaded, off next start" : m.warnings.empty() ? "loaded" : "loaded, with warnings";
			if (m.offNextStart) return "switched off";
			return "not loaded";
		}

		void DrawRow(const mirage::mods::Info& m)
		{
			ImGui::PushID(m.id.c_str());
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddCircleFilled({at.x + 6.f, at.y + ImGui::GetTextLineHeight() * 0.5f + 1.f}, 4.f,
														ImGui::GetColorU32(StateColor(m)));
			ImGui::SetCursorScreenPos({at.x + 16.f, at.y});
			const std::string label = (m.name.empty() ? m.id : m.name) + "  " + m.version;
			if (ImGui::Selectable(label.c_str(), selected_ == m.id)) selected_ = m.id;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s): %s", m.id.c_str(), m.kind.c_str(), StateText(m));
			ImGui::PopID();
		}

		void List(const char* title, const std::vector<std::string>& items)
		{
			if (items.empty()) return;
			ImGui::TextDisabled("%s", title);
			ImGui::Indent();
			for (const auto& i : items) ImGui::TextWrapped("%s", i.c_str());
			ImGui::Unindent();
		}

		void DrawDetails(const mirage::mods::Info& m)
		{
			ImGui::TextUnformatted((m.name.empty() ? m.id : m.name).c_str());
			ImGui::SameLine();
			ImGui::TextDisabled("%s  (%s, %s)", m.version.c_str(), m.id.c_str(), m.kind.empty() ? "?" : m.kind.c_str());
			ImGui::TextColored(StateColor(m), "%s", StateText(m));
			if (m.loaded) {
				ImGui::SameLine();
				ImGui::TextDisabled("#%d in the load order, %s phase%s", m.loadIndex + 1, mirage::mods::PhaseName(m.phase),
									m.reloads ? (", reloaded " + std::to_string(m.reloads) + "x").c_str() : "");
			}
			if (!m.problem.empty()) ImGui::TextWrapped("%s", m.problem.c_str());
			if (!m.description.empty()) ImGui::TextWrapped("%s", m.description.c_str());
			if (!m.authors.empty()) ImGui::TextDisabled("by %s", m.authors.c_str());

			// What can be done with it
			ImGui::Separator();
			if (m.loaded && ImGui::Button("Reload")) {
				const std::string id = m.id;
				game::RunOnGameThread([id] { mirage::mods::Reload(id); });
			}
			if (m.loaded) ImGui::SameLine();
			if (ImGui::Button(m.offNextStart ? "Switch on (next start)" : "Switch off")) mirage::mods::SetEnabledNextStart(m.id, m.offNextStart);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(m.offNextStart ? "Loaded again at the next start" : "Its modules go off now, and it isn't loaded at the next start");
			if (m.crashed) {
				ImGui::SameLine();
				if (ImGui::Button("Allow again")) mirage::mods::ForgetCrash(m.id);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Out of crashes.toml: tried again at the next start");
			}
			ImGui::SameLine();
			if (ImGui::Button("Folder")) ShellExecuteW(nullptr, L"open", m.dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);

			// Its modules, switched here like in their columns
			if (!m.modules.empty()) {
				ImGui::Separator();
				ImGui::TextDisabled("Modules");
				for (const auto& name : m.modules)
					if (Module* mod = modules::Find(name)) {
						bool on = mod->Enabled();
						if (ImGui::Checkbox(name.c_str(), &on)) mod->SetEnabled(on);
						ImGui::SameLine();
						ImGui::TextDisabled("%s", mod->Tab().c_str());
					}
			}
			if (!m.required.empty() || !m.optional.empty() || !m.incompatible.empty() || !m.dependents.empty()) ImGui::Separator();
			List("Requires", m.required);
			List("Works with (optional)", m.optional);
			List("Incompatible with", m.incompatible);
			List("Required by", m.dependents);
			if (!m.warnings.empty()) {
				ImGui::Separator();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.8f, 0.3f, 1.f});
				List("Warnings", m.warnings);
				ImGui::PopStyleColor();
			}

			// Its log: the lines that name it
			ImGui::Separator();
			ImGui::TextDisabled("Log");
			ImGui::BeginChild("##log", {0.f, 0.f}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
			const std::string tag = "[mod " + m.id + "]", named = " " + m.id + " ";
			int shown = 0;
			for (const auto& e : lines_)
				if (e.text.find(tag) != std::string::npos || (e.text.rfind("Mods: ", 0) == 0 && (e.text + " ").find(named) != std::string::npos)) {
					const bool bad = e.level >= logging::Level::Warn;
					if (bad) ImGui::PushStyleColor(ImGuiCol_Text, e.level >= logging::Level::Error ? ImVec4{0.95f, 0.4f, 0.35f, 1.f} : ImVec4{0.95f, 0.8f, 0.3f, 1.f});
					ImGui::Text("%s  %s", e.time.c_str(), e.text.c_str());
					if (bad) ImGui::PopStyleColor();
					++shown;
				}
			if (shown == 0) ImGui::TextDisabled("Nothing logged by it this session");
			ImGui::EndChild();
		}

		// The menu's log, kept here (the last lines)
		void Pull()
		{
			std::vector<logging::Entry> fresh;
			lastId_ = logging::CopySince(lastId_, fresh);
			for (auto& e : fresh) lines_.push_back(std::move(e));
			while (lines_.size() > 3000) lines_.pop_front();
		}

		std::string selected_;
		std::uint64_t lastId_ = 0;
		std::deque<logging::Entry> lines_;
	};
	MEM_MODULE(ModManager)
}

std::uint64_t mod_manager::Frames() { return g_frames.load(); }
std::size_t mod_manager::Rows() { return g_rows.load(); }
