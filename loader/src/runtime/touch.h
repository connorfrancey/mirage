#pragma once
// What a mod touches in the game, read without running it (step 10 of the next ten steps, the plan's step 37), and
// where two mods' touch sets overlap: shown in the mod manager and logged before anything loads.
//
// A touch set comes from:
//   - mod.toml: its input actions' keys;
//   - its content folders: defaults.toml (each object's property it writes) and textures/ (each texture it replaces);
//   - its Kismet layers (kismet/<map>.toml): the ops whose properties it sets, the links it takes out;
//   - its Lua and C# sources: the game functions its API calls name, a string literal or a constant holding one
//     (mirage.script.open: a script patch; mirage.hook / Mod.Hook: a hook; mirage.inject / Mod.Inject: an injection).
//     A native DLL's calls can't be read this way.
//
// Overlaps: two mods writing the same property, replacing the same texture, setting the same Kismet op's property or
// patching the same function are conflicts (the later one in the load order wins, or the two edits fight); a patch and
// an injection on one function, or two actions on one key, are notes (they work together, in an order). Two hooks or
// two injections on one function aren't reported: the runtime runs them all, in priority order.

#include <filesystem>
#include <string>
#include <vector>

namespace mirage::mods
{
	struct Manifest;
}

namespace mirage::touch
{
	enum class Kind { Property, Texture, KismetSet, KismetUnlink, Patch, Hook, Inject, ActionKey };
	const char* KindName(Kind k);

	struct Item
	{
		Kind kind;
		std::string what;		// The property's object.path, the texture's path, "map: op.property", the function, the key
		std::string where;		// The file it came from (relative to the mod's folder)
	};

	struct Set
	{
		std::string mod;
		std::vector<Item> items;
	};

	// A mod's touch set, from its folder (mod.toml read already)
	Set Read(const mods::Manifest& manifest);

	struct Overlap
	{
		bool conflict = false;						// false: a note
		Kind kind;
		std::string what;
		std::vector<std::string> mods;				// In load order
		std::string text;							// "texture A.B.C: replaced by x and y (y's wins: later in the load order)"
	};
	// The overlaps between `sets` (in load order), conflicts first
	std::vector<Overlap> Overlaps(const std::vector<Set>& sets);
}
