// The in-game script editor (Mirage step 27; see script_editor.h).

#include "features/misc/script_editor.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "runtime/mods.h"
#include "theme.h"

#include <TextEditor.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <cctype>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
	std::recursive_mutex g_lock;
	TextEditor g_editor;
	std::string g_mod, g_file;			// What is open ("" none)
	fs::path g_path;
	std::string g_saved;				// The text as it is on disk
	std::string g_status;
	std::atomic<unsigned long long> g_frames{0};

	fs::path DirOf(const std::string& modId)
	{
		for (const auto& m : mirage::mods::List())
			if (m.id == modId && m.loaded) return m.dir;
		return {};
	}
	std::string ProblemOf(const std::string& modId)
	{
		for (const auto& m : mirage::mods::List())
			if (m.id == modId) return m.problem;
		return {};
	}
	// "main.lua:12: unexpected symbol near 'x'" -> 12 when it names `file`
	int LineIn(const std::string& problem, const std::string& file)
	{
		const std::string name = fs::path(file).filename().string() + ":";
		for (auto at = problem.find(name); at != std::string::npos; at = problem.find(name, at + 1)) {
			const std::size_t digits = at + name.size();
			if (digits < problem.size() && std::isdigit(static_cast<unsigned char>(problem[digits]))) return std::atoi(problem.c_str() + digits);
		}
		return 0;
	}

	class ScriptEditor final : public Module
	{
	public:
		ScriptEditor() : Module("Script Editor", "Misc", "Edit a mod's Lua files in the game: saved files hot reload, errors marked, selections run", 4)
		{
			g_editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
			g_editor.SetPalette(TextEditor::GetDarkPalette());
		}

		void OnRender() override
		{
			if (!gui::visible) return;
			++g_frames;
			theme::Begin();
			ImGui::SetNextWindowPos({300.f, 100.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({820.f, 560.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({420.f, 240.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			const bool dirty = Dirty();
			if (ImGui::Begin(dirty ? "Script Editor *###ScriptEditor" : "Script Editor###ScriptEditor", &open,
							 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar))
				Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		static bool Dirty()
		{
			std::lock_guard lock(g_lock);
			return !g_file.empty() && g_editor.GetText() != g_saved;
		}

		void Draw()
		{
			std::lock_guard lock(g_lock);
			if (ImGui::BeginMenuBar()) {
				if (ImGui::BeginMenu("Open")) {
					for (const auto& m : mirage::mods::List()) {
						if (!m.loaded) continue;
						const auto files = script_editor::Files(m.id);
						if (files.empty()) continue;
						if (ImGui::BeginMenu(m.id.c_str())) {
							for (const auto& f : files)
								if (ImGui::MenuItem(f.c_str())) script_editor::Open(m.id, f);
							ImGui::EndMenu();
						}
					}
					ImGui::EndMenu();
				}
				ImGui::BeginDisabled(g_file.empty());
				if (ImGui::MenuItem("Save", "Ctrl+S")) script_editor::Save();
				if (ImGui::MenuItem("Run selection", "Ctrl+Enter")) RunSelection();
				ImGui::EndDisabled();
				ImGui::EndMenuBar();
			}
			if (g_file.empty()) {
				ImGui::TextDisabled("Open a mod's Lua file from the menu above");
				return;
			}
			const ImGuiIO& io = ImGui::GetIO();
			if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io.KeyCtrl) {
				if (ImGui::IsKeyPressed(ImGuiKey_S, false)) script_editor::Save();
				if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) RunSelection();
			}
			// The mod's load error, on its line
			const int line = script_editor::ErrorLine();
			TextEditor::ErrorMarkers markers;
			if (line > 0) markers[line] = ProblemOf(g_mod);
			g_editor.SetErrorMarkers(markers);
			const auto cursor = g_editor.GetCursorPosition();
			ImGui::TextDisabled("%s / %s   line %d, column %d", g_mod.c_str(), g_file.c_str(), cursor.mLine + 1, cursor.mColumn + 1);
			if (!g_status.empty()) {
				ImGui::SameLine();
				ImGui::TextUnformatted(g_status.c_str());
			}
			g_editor.Render("##code", {0.f, 0.f}, true);
		}

		// The selection (or the line the cursor is on) in the mod's state, through the REPL; the answer in the status
		void RunSelection()
		{
			std::string code = g_editor.HasSelection() ? g_editor.GetSelectedText() : g_editor.GetCurrentLineText();
			if (code.empty()) return;
			game::RunOnGameThread([mod = g_mod, code] {
				const auto r = mirage::mods::Eval(mod, code);
				std::string text = r.ok ? (r.value.empty() ? std::string("ran") : "= " + r.value) : "error: " + r.error;
				if (!r.printed.empty()) text += "  (printed: " + r.printed.substr(0, r.printed.size() - 1) + ")";
				std::lock_guard lock(g_lock);
				g_status = text;
			});
		}
	};
	MEM_MODULE(ScriptEditor)
}

namespace script_editor
{
	std::vector<std::string> Files(const std::string& modId)
	{
		std::vector<std::string> out;
		const fs::path dir = DirOf(modId);
		std::error_code ec;
		if (dir.empty() || !fs::is_directory(dir, ec)) return out;
		for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (it->is_regular_file(ec) && it->path().extension() == ".lua")
				out.push_back(fs::relative(it->path(), dir, ec).generic_string());
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	bool Open(const std::string& modId, const std::string& file)
	{
		const fs::path path = DirOf(modId) / fs::path(file);
		std::ifstream in(path, std::ios::binary);
		if (!in) return false;
		std::stringstream text;
		text << in.rdbuf();
		std::lock_guard lock(g_lock);
		g_mod = modId;
		g_file = file;
		g_path = path;
		g_saved = text.str();
		g_editor.SetText(g_saved);
		g_saved = g_editor.GetText();		// As the editor gives it back (line endings), so "unsaved" means edited
		g_status.clear();
		return true;
	}

	std::string Text()
	{
		std::lock_guard lock(g_lock);
		return g_editor.GetText();
	}

	void SetText(const std::string& text)
	{
		std::lock_guard lock(g_lock);
		g_editor.SetText(text);
	}

	bool Save()
	{
		std::lock_guard lock(g_lock);
		if (g_file.empty()) return false;
		const std::string text = g_editor.GetText();
		std::ofstream out(g_path, std::ios::binary | std::ios::trunc);
		if (!out) {
			g_status = "can't write " + g_path.string();
			return false;
		}
		out << text;
		out.close();
		g_saved = text;
		g_status = "saved: the mod reloads when its files settle";
		spdlog::info("Script Editor: {} saved ({} bytes)", g_path.string(), text.size());
		return true;
	}

	int ErrorLine()
	{
		std::lock_guard lock(g_lock);
		return g_file.empty() ? 0 : LineIn(ProblemOf(g_mod), g_file);
	}

	unsigned long long Frames() { return g_frames.load(); }
}
