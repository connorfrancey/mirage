#pragma once
// Maps from mods (Mirage, step 13): a mod's maps/<Name>.me1 (or .upk) is a level the game can load by its package name
// (`open <Name>`, StartGame, a streaming level), and it shows in the game's level select and the menu's Levels list
// like the game's own maps. Its details are in maps/<Name>.toml, all optional:
//
//     title = "Rooftops"                           # the level select's name (default: the package name)
//     description = "A short run over the roofs"
//     game = "TdGame.TdSPStoryGame"                # the game mode it starts in (this is the default)
//     [[checkpoint]]                               # checkpoints (TdCheckpoint actors in the map, by their name)
//     name = "Roof_Start"
//     title = "The roofs"
//
// The level select's entries are the game's own data providers (UIDataProvider_TdMaps), made by the game's data store
// from config sections: the runtime adds the mods' sections where the store asks the config for them
// (GConfig->GetPerObjectConfigSections), has the store build its list again, and fills the new providers in. Nothing is
// written to the game's ini files.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mirage::maps
{
	// Finds the engine functions and hooks the config lookup (after game::Init). False if they weren't found: mod maps
	// then load by name but don't show in the level select.
	bool Install();
	// GMalloc, the engine's allocator (its variable's address, found by Install): nullptr before
	void** GMallocVariable();
	// GMalloc's Realloc (8-byte aligned): memory for arrays the engine owns and frees itself. nullptr before Install or
	// when it fails; `bytes` 0 frees `p`.
	void* EngineRealloc(void* p, std::size_t bytes);
	// Game thread, every tick: registers the maps of mods that loaded (and drops those of mods that unloaded)
	void Tick();

	struct MapInfo
	{
		std::string mod, package;				// Its mod, the package's name ("Rooftops")
		std::filesystem::path file;
		std::string title, description, gameMode;
		std::vector<std::pair<std::string, std::string>> checkpoints;	// The map's name for it, its title
		bool listed = false;					// Its provider is in the game's level select data
		std::string problem;
	};
	// The mods' maps as last registered. Any thread.
	std::vector<MapInfo> List();
}
