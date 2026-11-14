#pragma once
// C# mods: the .NET runtime (x86, bundled in mirage\dotnet\) hosted through hostfxr, and Mirage.dll (managed/Mirage)
// that mods are written against. Started the first time a C# mod loads, never before: a game without C# mods never
// pays for it. Each mod gets its own collectible AssemblyLoadContext, so hot reload unloads it for real.
#include "mirage/api.h"

#include <cstdint>
#include <filesystem>
#include <string>

struct MirageMod;

#ifndef MIRAGE_DOTNET
#define MIRAGE_DOTNET 0
#endif

namespace mirage::dotnet
{
	// The build's scripting language (cmake/DotNet.cmake, MIRAGE_DOTNET): C# when on, Lua when off. New script mods and
	// the script editor's templates use it; Lua mods run in every build.
	constexpr bool kEnabled = MIRAGE_DOTNET != 0;
	inline const char* ScriptLanguage() { return kEnabled ? "C#" : "Lua"; }
	inline const char* ScriptFile() { return kEnabled ? "main.cs" : "main.lua"; }

	// Compiles a script mod's folder without loading it (the editor). `file` + `text`: one file's unsaved contents.
	struct CheckResult
	{
		int errors = -1;					// -1: couldn't check (no runtime)
		std::string output;					// The diagnostics, one per line ("main.cs(3,9): error CS1002: ; expected")
	};
	CheckResult CheckScripts(const std::filesystem::path& folder, const std::filesystem::path& file, const std::string& text);

	// Loads `assembly` (a mod's C# DLL) for `mod`. "" when it loaded, else why not.
	std::string LoadMod(MirageMod* mod, const std::filesystem::path& assembly);
	void UnloadMod(MirageMod* mod);

	struct Status
	{
		bool started = false;
		std::string problem;				// Why it couldn't start (empty: started, or not asked yet)
		std::string version;				// The runtime's ("10.0.15")
		double startMs = 0.0;				// Time the runtime took to start
		std::int64_t managedBytes = -1;		// The GC heap now (-1: unknown)
		int stillLoaded = 0;				// Mods unloaded whose assemblies are still alive (they kept a reference)
		std::uint64_t addressBefore = 0, addressAfter = 0;	// Free address space before and after starting it
	};
	Status GetStatus(bool measure = false);	// `measure`: also runs a GC to count contexts that didn't unload (slow)

	std::filesystem::path Root();			// mirage\dotnet
}
