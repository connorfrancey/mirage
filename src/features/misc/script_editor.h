#pragma once
// The script editor (features/misc/script_editor.cpp): write script mods in the game. The build's scripting language
// (runtime/mod_dotnet.h: C# when MEMENU_DOTNET is on, else Lua) is what new mods are made in; both kinds can be edited.
// The parts the self-test drives without clicking.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace script_editor
{
	std::uint64_t Frames();		// Frames its window was drawn

	struct Diagnostic
	{
		std::string file;		// "main.cs"
		int line = 0, column = 0;	// 1-based; 0: unknown
		bool error = true;		// Else a warning
		std::string text;		// The whole line as the compiler wrote it
	};
	struct CheckResult
	{
		bool ran = false;		// False: the check itself couldn't run (`note` says why)
		std::vector<Diagnostic> diagnostics;
		std::string note;		// "13 ms", or why it didn't run
		int Errors() const;
	};
	// Compiles `file`'s unsaved `text` with the rest of the mod in `folder` (C#), or checks its syntax (Lua). Any thread
	// (the editor runs it on a worker).
	CheckResult Check(const std::filesystem::path& folder, const std::filesystem::path& file, const std::string& text);
	// Compiler output -> diagnostics: "main.cs(5,17): error CS0029: ..." and "main.lua:5: ..."
	std::vector<Diagnostic> ParseDiagnostics(const std::string& output);

	// A new script mod `id` in `modsDir`: mod.toml and main.cs (or main.lua), from the template. "" or why not.
	std::string CreateModFiles(const std::filesystem::path& modsDir, const std::string& id);
	std::string Template(const std::string& id);	// The new mod's main file
}
