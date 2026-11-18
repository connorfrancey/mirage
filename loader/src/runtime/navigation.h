#pragma once
// Navigation for AI (Mirage step 28): the engine's own path builder run in the game. The editor's "Build Paths" ships
// in MirrorsEdge.exe: a Scout (TdScout) defines the paths (`AScout::DefinePaths`, reached through its `Exec`
// "DEFINEPATHS"), walking every NavigationPoint of the world and making the ReachSpecs between them that the AI paths
// over. Found with Ghidra: Exec is the Scout's vtable +0x44c (0x00ec1100), DefinePaths +0x438 (0x00ec48e0). Game thread.
#include <string>

namespace mirage::nav
{
	struct Stats
	{
		int points = 0;			// NavigationPoints in the world's list
		int reachSpecs = 0;		// ReachSpecs over all of them (their PathLists)
	};
	Stats Count();

	// The world's paths defined again, as the editor's Build Paths does. False and `error` if it can't be run (no world,
	// the Scout didn't spawn, its Exec isn't the one this was found for).
	bool BuildPaths(std::string* error = nullptr);
}
