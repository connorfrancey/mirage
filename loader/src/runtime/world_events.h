#pragma once
// The world's lifecycle for mods (Mirage, step 14): actors coming into play and leaving it, streaming levels becoming
// visible and hidden, and checkpoint restarts, told to the mods as MIRAGE_EVENT_ACTOR_ADDED / _ACTOR_REMOVED /
// _LEVEL_STREAMED / _RESTART.
//
// Actors are found by comparing the levels' actor lists (game::ActorsOf: the persistent level and the visible streaming
// levels, read once per tick) with the last tick's: it sees every way an actor comes and goes (spawned from script or
// native code, destroyed, streamed, put back or taken away by a TAS rewind) with no hook in the engine. The comparison
// only runs while a mod subscribes to actor events. An actor made and destroyed within one tick isn't seen. After a level
// load the comparison starts over from the new level's actors (the level load event says the world changed).
//
// A restart is a new player pawn in the same level (the game's checkpoint restart, a death's respawn).
//
// Game thread.

#include <cstddef>
#include <cstdint>

namespace mirage::world_events
{
	void Tick();
	void OnLevelLoaded();	// A level finished loading: the next tick takes the new world as it is (no events for it)

	struct Stats
	{
		std::size_t actors = 0;				// In play at the last comparison
		std::uint64_t added = 0, removed = 0, streamed = 0, restarts = 0;	// Told since the start
		std::uint64_t compares = 0;			// Ticks the lists were compared (someone subscribed)
		std::uint64_t changed = 0;			// ... of them with a change (sorted and diffed); the rest: one memory compare
		double totalCompareMs = 0.0;		// All of them
		double unchangedCompareMs = 0.0;	// The ones without a change (compares - changed of them): the steady cost
		double lastCompareMs = 0.0;			// The last comparison's cost
		double maxCompareMs = 0.0;
	};
	Stats GetStats();
}
