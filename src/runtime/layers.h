#pragma once
// Level additions (Mirage, step 13): a mod places actors into an existing level without a new package. A layer is a
// TOML file in the mod's folder, levels/<map>.toml ("levels/edge_p.toml" for the Prologue), listing actors:
//
//     [[actor]]
//     name = "Pad1"                                   # optional: how scripts and the editor find it
//     class = "classes/MirageBouncePad.uc"            # a .uc file of the mod (step 12), or a class path "Engine.Trigger"
//     location = [19309.0, -3009.0, 8420.0]
//     rotation = [0, 16384, 0]                        # optional: pitch, yaw, roll (65536 = a full turn)
//     [actor.properties]                              # optional: dotted paths, as mirage.set takes them
//     Strength = 1800.0
//
// The actors are spawned when the level has loaded, and again (fresh) at every checkpoint restart: a restart doesn't
// reload the level, it gives the player a new pawn, and actors spawned into the level would otherwise keep their state.
// They go when the mod unloads, come back changed after a hot reload, and are gone with the level. Content mods (no
// code) can carry layers too. Game thread.

#include <string>
#include <vector>

namespace Classes
{
	class AActor;
}

namespace mirage::layers
{
	// Every engine tick in a level (game thread): spawns, respawns and removes as levels, restarts and mods change
	void Tick();
	// A level finished loading (the engine's level load callback): the next tick spawns the new level's layers
	void OnLevelLoaded();
	// The mod's actors removed now (its unload; the next tick spawns them again if it is loaded)
	void Remove(const std::string& modId);

	struct ActorInfo
	{
		std::string mod, name, cls;		// Its mod, its name in the layer ("" if none), the class as the layer gives it
		Classes::AActor* actor = nullptr;	// nullptr: it didn't spawn (`problem` says why)
		std::string problem;
	};
	// The current level's layer actors. Game thread.
	std::vector<ActorInfo> List();
	// How many times the layers were (re)spawned since the menu started: level loads and restarts
	int Generation();
}
