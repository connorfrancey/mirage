// The mod manager (Mirage, step 9): every mod found, loaded or not, with why; its details, modules, dependencies,
// warnings, crash record and log; and what can be done with it: reload it now, switch it on or off for the next start,
// let a crashed one load again, open its folder.

#include "features/misc/mod_manager.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "log.h"
#include "runtime/changes.h"
#include "runtime/inject.h"
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
				// Safe mode (step 10): why nothing loaded, and that the next start is a normal one
				if (const std::string why = mirage::mods::SafeModeReason(); !why.empty()) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.95f, 0.8f, 0.3f, 1.f});
					ImGui::TextWrapped("Safe mode: no mod was loaded this time: %s. The next start loads them as usual (hold Shift "
									   "as the game starts for another safe start).", why.c_str());
					ImGui::PopStyleColor();
				}
				// Conflicts between the mods that load (found before they loaded)
				const auto overlaps = mirage::mods::Overlaps();
				int conflicts = 0;
				for (const auto& o : overlaps) conflicts += o.conflict ? 1 : 0;
				if (!overlaps.empty()) {
					ImGui::PushStyleColor(ImGuiCol_Text, conflicts ? ImVec4{0.95f, 0.55f, 0.3f, 1.f} : ImVec4{0.6f, 0.75f, 0.9f, 1.f});
					const bool listOpen = ImGui::TreeNode("##overlaps", "%d conflict%s, %d overlap%s between mods (found before they loaded)", conflicts,
													  conflicts == 1 ? "" : "s", static_cast<int>(overlaps.size()) - conflicts,
													  overlaps.size() - conflicts == 1 ? "" : "s");
					ImGui::PopStyleColor();
					if (listOpen) {
						for (const auto& o : overlaps) ImGui::BulletText("%s%s", o.conflict ? "Conflict: " : "", o.text.c_str());
						ImGui::TreePop();
					}
				}
				ImGui::BeginChild("##list", {230.f, 0.f}, ImGuiChildFlags_Borders);
				for (const auto& m : mods) DrawRow(m);
				ImGui::EndChild();
				ImGui::SameLine();
				ImGui::BeginChild("##details", {0.f, 0.f}, ImGuiChildFlags_Borders);
				const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == selected_; });
				if (it != mods.end()) DrawDetails(*it);
				else ImGui::TextDisabled("No mods: put mod folders in mirage\\mods");
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

		// The engine's name of a key ImGui saw pressed ("" for keys an action can't use): what mod.toml's [actions] take
		std::string EngineKeyName(ImGuiKey k)
		{
			if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return std::string(1, static_cast<char>('A' + (k - ImGuiKey_A)));
			if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return "F" + std::to_string(1 + (k - ImGuiKey_F1));
			static const char* const digits[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
			if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return digits[k - ImGuiKey_0];
			switch (k) {
				case ImGuiKey_Space: return "SpaceBar";
				case ImGuiKey_LeftShift: return "LeftShift";
				case ImGuiKey_RightShift: return "RightShift";
				case ImGuiKey_LeftCtrl: return "LeftControl";
				case ImGuiKey_RightCtrl: return "RightControl";
				case ImGuiKey_LeftAlt: return "LeftAlt";
				case ImGuiKey_RightAlt: return "RightAlt";
				case ImGuiKey_Tab: return "Tab";
				case ImGuiKey_Enter: return "Enter";
				case ImGuiKey_Backspace: return "BackSpace";
				case ImGuiKey_CapsLock: return "CapsLock";
				case ImGuiKey_UpArrow: return "Up";
				case ImGuiKey_DownArrow: return "Down";
				case ImGuiKey_LeftArrow: return "Left";
				case ImGuiKey_RightArrow: return "Right";
				case ImGuiKey_Home: return "Home";
				case ImGuiKey_End: return "End";
				case ImGuiKey_PageUp: return "PageUp";
				case ImGuiKey_PageDown: return "PageDown";
				case ImGuiKey_Delete: return "Delete";
				case ImGuiKey_GraveAccent: return "Tilde";
				case ImGuiKey_MouseLeft: return "LeftMouseButton";
				case ImGuiKey_MouseRight: return "RightMouseButton";
				case ImGuiKey_MouseMiddle: return "MiddleMouseButton";
				case ImGuiKey_MouseX1: return "ThumbMouseButton";
				case ImGuiKey_MouseX2: return "ThumbMouseButton2";
				default: return {};
			}
		}

		std::string g_capturing;		// "<mod>/<action>" waiting for a key (render thread)

		// Its input actions: the key each is on, changed by pressing another (saved in the mod's config.toml)
		void DrawActions(const mirage::mods::Info& m)
		{
			if (m.actions.empty()) return;
			ImGui::Separator();
			ImGui::TextDisabled("Input actions");
			for (const auto& a : m.actions) {
				const std::string id = m.id + "/" + a.name;
				ImGui::PushID(id.c_str());
				ImGui::TextUnformatted(a.name.c_str());
				if (!a.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", a.description.c_str());
				ImGui::SameLine(ImGui::GetFontSize() * 12.f);
				if (g_capturing == id) {
					ImGui::Button("Press a key (Esc: cancel)");
					if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_capturing.clear();
					else
						for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
							if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false))
								if (const std::string name = EngineKeyName(static_cast<ImGuiKey>(k)); !name.empty()) {
									mirage::mods::SetActionKey(m.id, a.name, name == a.defaultKey ? "" : name);
									g_capturing.clear();
									break;
								}
				} else if (ImGui::Button(a.key.c_str())) {
					g_capturing = id;
				}
				if (ImGui::IsItemHovered() && g_capturing != id) ImGui::SetTooltip("Click, then press the key or button to put it on");
				if (a.key != a.defaultKey) {
					ImGui::SameLine();
					if (ImGui::SmallButton("Default")) mirage::mods::SetActionKey(m.id, a.name, "");
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Back on %s (the mod's own)", a.defaultKey.c_str());
				}
				ImGui::PopID();
			}
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
			DrawActions(m);
			// Where it overlaps with other mods (its touch set against theirs, read before any of them ran)
			{
				bool header = false;
				for (const auto& o : mirage::mods::Overlaps()) {
					if (std::find(o.mods.begin(), o.mods.end(), m.id) == o.mods.end()) continue;
					if (!header) {
						ImGui::Separator();
						ImGui::TextDisabled("Overlaps with other mods");
						header = true;
					}
					if (o.conflict) ImGui::TextColored({0.95f, 0.55f, 0.3f, 1.f}, "Conflict: %s", o.text.c_str());
					else ImGui::BulletText("%s", o.text.c_str());
				}
			}
			// Its script injections (step 11), and the other mods injecting into the same functions: the order they run in
			const auto injections = mirage::inject::List();
			bool header = false;
			for (const auto& i : injections) {
				if (i.owner != m.id) continue;
				if (!header) {
					ImGui::Separator();
					ImGui::TextDisabled("Script injections");
					header = true;
				}
				ImGui::BulletText("%s at %s (priority %d, %d place%s)", i.function.c_str(), i.point.c_str(), i.priority, i.places,
								  i.places == 1 ? "" : "s");
				std::string others;
				for (const auto& o : injections)
					if (o.function == i.function && o.owner != m.id && others.find(o.owner) == std::string::npos)
						others += (others.empty() ? "" : ", ") + o.owner + " (" + std::to_string(o.priority) + ")";
				if (!others.empty()) {
					ImGui::SameLine();
					ImGui::TextColored({0.95f, 0.8f, 0.3f, 1.f}, "also: %s", others.c_str());
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Other mods inject into this function: at one place, higher priority runs first");
				}
			}
			// Its transactions (step 15): what it changed reversibly, each put back with a click (or all at its unload)
			const auto changes = mirage::changes::List(m.id);
			if (!changes.empty()) {
				ImGui::Separator();
				ImGui::TextDisabled("Changes (put back when it unloads)");
				for (auto t = changes.rbegin(); t != changes.rend(); ++t) {
					ImGui::PushID(t->id);
					const std::string label = t->name + " (" + std::to_string(t->paths.size()) + " value" +
											  (t->paths.size() == 1 ? "" : "s") + (t->reverted ? ", put back)" : ")");
					const bool open = ImGui::TreeNode(label.c_str());
					if (!t->reverted) {
						ImGui::SameLine();
						if (ImGui::SmallButton("Put back"))
							game::RunOnGameThread([mod = m.id, id = t->id] { mirage::changes::Revert(mod, id); });
					}
					if (open) {
						for (const auto& path : t->paths) ImGui::BulletText("%s", path.c_str());
						ImGui::TreePop();
					}
					ImGui::PopID();
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
