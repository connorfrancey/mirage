#pragma once
// The in-game script editor (Mirage step 27): a loaded mod's Lua files opened in a code editor (ImGuiColorTextEdit:
// Lua highlighting, line numbers; no find and replace yet), saved back to the mod's folder, where the hot
// reload picks them up (Lua mods reload by default), with the mod's load error marked on its line. "Run selection"
// sends the selected lines (or the current line) to the console's REPL in the mod's state (runtime/mods.h Eval).
//
// The window and the self-test call the same functions below (render thread or any thread with the window closed: the
// editor's state has its own lock).
#include <string>
#include <vector>

namespace script_editor
{
	// The .lua files of loaded mod `modId`, relative to its folder ("main.lua", "lib/util.lua")
	std::vector<std::string> Files(const std::string& modId);
	// `file` of `modId` opened. False if it can't be read.
	bool Open(const std::string& modId, const std::string& file);
	std::string Text();
	void SetText(const std::string& text);
	// Written back to the file. False if it can't be written.
	bool Save();
	// The line (1-based) the open mod's load error names in the open file, 0 if none
	int ErrorLine();

	unsigned long long Frames();
}
