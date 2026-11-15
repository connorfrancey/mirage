#pragma once
// Kismet layers (step 5 of the next ten steps, the plan's step 18): a mod rewires a level's Kismet from data, no code.
// A layer is kismet/<map>.toml in the mod's folder ("kismet/edge_p.toml" for the Prologue):
//
//     [[op]]                                  # a new op
//     name = "Ping"                           # the layer's name for it (the links below use it)
//     class = "classes/MyAction.uc"           # a .uc file of the mod (step 12), or a class ("Engine.SeqAct_Toggle")
//     sequence = "Main_Sequence"              # optional: the sequence it goes in (default: the level's root sequence)
//     [op.properties]                         # optional: dotted paths, as level layers take them
//     bCallHandler = false
//
//     [[unlink]]                              # a link taken out
//     from = "SeqEvent_Touch_3"               # an op of the level: its name, or the end of its path when names repeat
//                                             # (a name in the map's own level and in a sublevel: the map's)
//     output = "Touched"                      # a description or an index ("0"); default: the first
//     to = "SeqAct_Toggle_2"
//     input = "Turn On"
//
//     [[link]]                                # a link added (same keys; `from` and `to` may be the layer's own ops)
//     from = "SeqEvent_Touch_3"
//     to = "Ping"
//
//     [[set]]                                 # properties of an op of the level
//     op = "SeqEvent_Touch_3"
//     [set.properties]
//     MaxTriggerCount = 0
//
// Applied in that order (ops, unlinks, links, sets) once the level has loaded. A checkpoint restart keeps the level's
// sequences, so the edits stay; a level load applies the layer again. Undone when the mod unloads, before a hot reload
// or a changed file applies it again. An entry naming what isn't there (or more than one op) is skipped and listed.
// Game thread.

#include <string>
#include <vector>

namespace mirage::kismet_layers
{
	// Every engine tick (game thread): applies, re-applies and undoes as levels and mods change
	void Tick();
	// A level finished loading: the next tick applies the new level's layers
	void OnLevelLoaded();
	// The mod's edits undone now (its unload)
	void Remove(const std::string& modId);

	struct Entry
	{
		std::string mod;
		std::string what;			// "op Ping (MirageSeqAct_Ping)", "link SeqEvent_Touch_3[0] -> Ping[0]"
		std::string problem;		// Why it was skipped ("" when applied)
	};
	// The current level's layer entries, applied and skipped. Game thread.
	std::vector<Entry> List();
}
