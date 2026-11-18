// The in-game script editor (Mirage step 27; see script_editor.h), on Zep: notepad-style editing by default, Vim mode
// from the menu (or Ctrl+2; Ctrl+1 back), Lua highlighting, JetBrains Mono Nerd Font, the system clipboard.

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
#include "imgui/zep.h"

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
	// The editor's clipboard is the system's (Zep asks through messages)
	struct Clipboard final : Zep::IZepComponent
	{
		explicit Clipboard(Zep::ZepEditor& editor) : editor_(editor) {}
		void Notify(std::shared_ptr<Zep::ZepMessage> m) override
		{
			if (m->messageId == Zep::Msg::GetClipBoard) {
				const char* text = ImGui::GetCurrentContext() ? ImGui::GetClipboardText() : nullptr;
				m->str = text ? text : "";
				m->handled = true;
			} else if (m->messageId == Zep::Msg::SetClipBoard) {
				if (ImGui::GetCurrentContext()) ImGui::SetClipboardText(m->str.c_str());
				m->handled = true;
			}
		}
		Zep::ZepEditor& GetEditor() const override { return editor_; }
		Zep::ZepEditor& editor_;
	};

	std::recursive_mutex g_lock;
	std::unique_ptr<Zep::ZepEditor_ImGui> g_editor;		// One buffer: the open file (made again for each file)
	std::unique_ptr<Clipboard> g_clipboard;
	std::shared_ptr<Zep::RangeMarker> g_marker;			// The load error's line
	int g_markedLine = 0;
	bool g_fontsSet = false, g_vim = false;
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

	Zep::ZepBuffer* Buffer() { return g_editor ? g_editor->GetActiveBuffer() : nullptr; }
	std::string BufferText()
	{
		Zep::ZepBuffer* b = Buffer();
		return b ? b->GetBufferText(b->Begin(), b->End()) : std::string();
	}

	// The mod's load error marked on its line (a background, an indicator, a tooltip), or no mark
	void MarkError(int line, const std::string& problem)
	{
		Zep::ZepBuffer* b = Buffer();
		if (!b || line == g_markedLine) return;
		if (g_marker) b->ClearRangeMarkers({g_marker});
		g_marker.reset();
		g_markedLine = line;
		Zep::ByteRange range;
		if (line <= 0 || !b->GetLineOffsets(line - 1, range)) return;
		g_marker = std::make_shared<Zep::RangeMarker>(*b);
		g_marker->SetRange(range);
		g_marker->SetColors(Zep::ThemeColor::Error, Zep::ThemeColor::Text, Zep::ThemeColor::Error);
		g_marker->SetName("Load error");
		g_marker->SetDescription(problem);
		g_marker->displayType = Zep::RangeMarkerDisplayType::CompileError;
		b->AddRangeMarker(g_marker);
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
				ImGui::EndDisabled();
				if (ImGui::MenuItem(ICON_FA_KEYBOARD " Vim", nullptr, &g_vim) && g_editor)
					g_editor->SetGlobalMode(g_vim ? Zep::ZepMode_Vim::StaticName() : Zep::ZepMode_Standard::StaticName());
				ImGui::EndMenuBar();
			}
			if (g_file.empty() || !g_editor) {
				ImGui::TextDisabled("Open a mod's Lua file from the menu above");
				return;
			}
			if (!g_fontsSet) SetFonts();
			const ImGuiIO& io = ImGui::GetIO();
			const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
			if (focused && io.KeyCtrl) {
				if (ImGui::IsKeyPressed(ImGuiKey_S, false)) script_editor::Save();
				if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) RunSelection();
			}
			const int line = script_editor::ErrorLine();
			MarkError(line, line ? ProblemOf(g_mod) : std::string());
			ImGui::TextDisabled("%s / %s%s", g_mod.c_str(), g_file.c_str(), g_vim ? "   (Vim)" : "");
			if (!g_status.empty()) {
				ImGui::SameLine();
				ImGui::TextUnformatted(g_status.c_str());
			}
			// The editor fills the rest of the window
			const ImVec2 min = ImGui::GetCursorScreenPos();
			ImVec2 size = ImGui::GetContentRegionAvail();
			size = {std::max(1.f, size.x), std::max(1.f, size.y)};
			g_editor->SetDisplayRegion(Zep::NVec2f(min.x, min.y), Zep::NVec2f(min.x + size.x, min.y + size.y));
			g_editor->Display();
			if (focused && !(io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_S, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false))))
				g_editor->HandleInput();
			else if (!focused) g_editor->ResetCursorTimer();
			ImGui::Dummy(size);
		}

		// JetBrains Mono Nerd Font (or the console's monospace font) for every kind of text the editor draws (ImGui's first
		// font is the menu's)
		static void SetFonts()
		{
			g_fontsSet = true;
			ImFont* mono = font::Editor();
			if (!mono) return;
			auto& display = static_cast<Zep::ZepDisplay_ImGui&>(g_editor->GetDisplay());
			const int px = 15;
			for (auto type : {Zep::ZepTextType::UI, Zep::ZepTextType::Text, Zep::ZepTextType::Heading1, Zep::ZepTextType::Heading2, Zep::ZepTextType::Heading3})
				display.SetFont(type, std::make_shared<Zep::ZepFont_ImGui>(display, mono, px));
		}

		// The selection (or the line the cursor is on) in the mod's state, through the REPL; the answer in the status
		static void RunSelection()
		{
			Zep::ZepBuffer* b = Buffer();
			if (!b || !g_editor->GetActiveWindow()) return;
			std::string code;
			if (b->HasSelection()) {
				const auto sel = b->GetInclusiveSelection();
				code = b->GetBufferText(sel.first, sel.second.Peek(1));
			} else {
				const auto cursor = g_editor->GetActiveWindow()->GetBufferCursor();
				code = b->GetBufferText(b->GetLinePos(cursor, Zep::LineLocation::LineBegin), b->GetLinePos(cursor, Zep::LineLocation::LineLastNonCR));
			}
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
		// A fresh editor per file: one buffer, no tabs to manage. No threads of its own in the game's process.
		g_marker.reset();
		g_markedLine = 0;
		g_clipboard.reset();
		g_editor = std::make_unique<Zep::ZepEditor_ImGui>(paths::Dir(), Zep::NVec2f(1.f, 1.f), Zep::ZepEditorFlags::DisableThreads);
		g_clipboard = std::make_unique<Clipboard>(*g_editor);
		g_editor->RegisterCallback(g_clipboard.get());
		g_editor->InitWithText(fs::path(file).filename().string(), text.str());
		g_editor->SetGlobalMode(g_vim ? Zep::ZepMode_Vim::StaticName() : Zep::ZepMode_Standard::StaticName());	// After: a mode needs a window
		g_fontsSet = false;
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
		if (Zep::ZepBuffer* b = Buffer()) {
			b->SetText(text);
			g_markedLine = -1;		// The marker's range is gone with the old text: placed again
			g_marker.reset();
		}
	}

	bool Save()
	{
		std::lock_guard lock(g_lock);
		if (g_file.empty() || !Buffer()) return false;
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
