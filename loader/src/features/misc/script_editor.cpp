// The in-game script editor (Mirage step 27; see script_editor.h), on ImGuiColorTextEdit (goossens' fork): Lua
// highlighting, line numbers, find and replace (Ctrl+F), JetBrains Mono Nerd Font, ImGui's clipboard (the system's).

#include "features/misc/script_editor.h"

#include "core/module.h"
#include "font.h"
#include "game/game.h"
#include "gui.h"
#include "paths.h"
#include "runtime/mods.h"
#include "theme.h"

#include <IconsFontAwesome6.h>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <TextEditor.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace fs = std::filesystem;

namespace
{
	std::recursive_mutex g_lock;
	std::unique_ptr<TextEditor> g_editor;		// The open file's text (made with the first file)
	int g_markedLine = 0;						// The load error's line (1-based) the editor marks, 0 none, -1 to place again
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
	// "main.lua: main.lua:12: unexpected symbol near 'x'" -> 12 when it names `file`
	int LineIn(const std::string& problem, const std::string& file)
	{
		const std::string name = fs::path(file).filename().string() + ":";
		for (auto at = problem.find(name); at != std::string::npos; at = problem.find(name, at + 1)) {
			const std::size_t digits = at + name.size();
			if (digits < problem.size() && std::isdigit(static_cast<unsigned char>(problem[digits]))) return std::atoi(problem.c_str() + digits);
		}
		return 0;
	}

	std::string BufferText() { return g_editor ? g_editor->GetText() : std::string(); }

	// The mod's load error marked on its line (its number and text in red, the error as their tooltip), or no mark
	void MarkError(int line, const std::string& problem)
	{
		if (!g_editor || line == g_markedLine) return;
		g_editor->ClearMarkers();
		g_markedLine = line;
		if (line <= 0 || static_cast<std::size_t>(line) > g_editor->GetLineCount()) return;
		g_editor->AddMarker(static_cast<std::size_t>(line - 1), IM_COL32(255, 90, 90, 255), IM_COL32(120, 30, 30, 160), problem, problem);
	}

	class ScriptEditor final : public Module
	{
	public:
		ScriptEditor() : Module("Script Editor", "Misc", "Edit a mod's Lua files in the game: saved files hot reload, errors marked, selections run", 4) {}

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
							 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
				Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		static bool Dirty()
		{
			std::lock_guard lock(g_lock);
			return !g_file.empty() && BufferText() != g_saved;
		}

		void Draw()
		{
			std::lock_guard lock(g_lock);
			if (ImGui::BeginMenuBar()) {
				if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN " Open")) {
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
				if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save", "Ctrl+S")) script_editor::Save();
				if (ImGui::MenuItem(ICON_FA_PLAY " Run selection", "Ctrl+Enter")) RunSelection();
				if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS " Find", "Ctrl+F") && g_editor) g_editor->OpenFindReplaceWindow();
				ImGui::EndDisabled();
				ImGui::EndMenuBar();
			}
			if (g_file.empty() || !g_editor) {
				ImGui::TextDisabled("Open a mod's Lua file from the menu above");
				return;
			}
			const ImGuiIO& io = ImGui::GetIO();
			const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			if (focused && io.KeyCtrl) {
				if (ImGui::IsKeyPressed(ImGuiKey_S, false)) script_editor::Save();
				if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) RunSelection();
			}
			const int line = script_editor::ErrorLine();
			MarkError(line, line ? ProblemOf(g_mod) : std::string());
			ImGui::TextDisabled("%s / %s", g_mod.c_str(), g_file.c_str());
			if (!g_status.empty()) {
				ImGui::SameLine();
				ImGui::TextUnformatted(g_status.c_str());
			}
			// The editor fills the rest of the window, in JetBrains Mono (or the console's monospace font). Ctrl+Enter is
			// Run selection here, the editor's "insert a line below": read-only while it is held, the editor leaves it be.
			const bool runKey = io.KeyCtrl && (ImGui::IsKeyDown(ImGuiKey_Enter) || ImGui::IsKeyDown(ImGuiKey_KeypadEnter));
			g_editor->SetReadOnlyEnabled(runKey);
			ImFont* mono = font::Editor();
			if (mono) ImGui::PushFont(mono, 15.f);
			g_editor->Render("##script", ImGui::GetContentRegionAvail());
			if (mono) ImGui::PopFont();
			g_editor->SetReadOnlyEnabled(false);
		}

		// The selection (or the line the cursor is on) in the mod's state, through the REPL; the answer in the status
		static void RunSelection()
		{
			if (!g_editor) return;
			const std::string code = g_editor->MainCursorHasSelection() ? g_editor->GetSectionText(g_editor->GetMainCursorSelection())
																		: g_editor->GetLineText(g_editor->GetMainCursorPosition().line);
			if (code.find_first_not_of(" \t\r\n") == std::string::npos) return;
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
		if (!g_editor) {
			g_editor = std::make_unique<TextEditor>();
			g_editor->SetLanguage(TextEditor::Language::Lua());
			g_editor->SetTabSize(4);
			g_editor->SetFindReplaceEnabled(true);
		}
		g_editor->SetText(text.str());
		g_editor->ClearMarkers();
		g_markedLine = 0;
		g_mod = modId;
		g_file = file;
		g_path = path;
		g_saved = BufferText();		// As the editor gives it back, so "unsaved" means edited
		g_status.clear();
		return true;
	}

	std::string Text()
	{
		std::lock_guard lock(g_lock);
		return BufferText();
	}

	void SetText(const std::string& text)
	{
		std::lock_guard lock(g_lock);
		if (g_editor) {
			g_editor->SetText(text);
			g_editor->ClearMarkers();
			g_markedLine = -1;		// Placed again on the new text
		}
	}

	bool Save()
	{
		std::lock_guard lock(g_lock);
		if (g_file.empty() || !g_editor) return false;
		const std::string text = BufferText();
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
