#pragma once
// Reversible changes (Mirage, step 15): a mod's transactions of writes to game objects, undone when it asks, and when
// it unloads, reloads or crashes.
//
// A transaction (change_begin ... change_end, Lua mirage.transaction) records the value each object property had before
// the first write to it inside the transaction (object_set; the same property written again keeps its first old
// value). Writes outside a transaction are the mod's to manage, as before: gameplay writes (teleporting the player,
// setting its health) must not be undone by a hot reload. Reverting puts the old values back, newest transaction first,
// last write first, through the same writer; an object that is gone since (its handle no longer matches: destroyed, its
// level unloaded) is skipped and counted.
//
// The open transaction is per thread: one opens and closes inside one callback on the game thread. Lists for the mod
// manager are read from any thread.

#include "mirage/api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mirage::changes
{
	// Game thread. The transaction's id (> 0); 0 if one is open on this thread already
	int Begin(const std::string& modId, const std::string& name);
	void End();
	// Whether object_set should record (a transaction is open on this thread), and the record (old value before the write)
	bool Recording();
	void Record(MirageObject* object, const char* path, const MirageValue& old);

	struct Result
	{
		int restored = 0;		// Values put back
		int gone = 0;			// Skipped: the object is gone
	};
	// Game thread. `id` -1: every transaction of the mod not reverted yet, newest first
	Result Revert(const std::string& modId, int id);
	// The mod unloads: everything reverted (game thread), and its list cleared
	Result RevertAndForget(const std::string& modId);

	struct Info
	{
		int id = 0;
		std::string name;
		std::vector<std::string> paths;		// "TdPlayerPawn_0.GroundSpeed", one per value recorded
		bool reverted = false;
	};
	std::vector<Info> List(const std::string& modId);	// Oldest first. Any thread
	std::string ListJson(const std::string& modId);		// The same as JSON: [{"id", "name", "values": [...], "reverted"}]

	// Wired by mod_game.cpp: the runtime's object handle and writer (a revert must not be recorded)
	struct Writer
	{
		int32_t(MIRAGE_CALL* handle)(MirageObject*, MirageHandle*) = nullptr;
		MirageObject*(MIRAGE_CALL* resolve)(const MirageHandle*) = nullptr;
		int32_t(MIRAGE_CALL* set)(MirageObject*, const char*, const MirageValue*) = nullptr;
		uint32_t(MIRAGE_CALL* name)(MirageObject*, char*, uint32_t) = nullptr;
	};
	void SetWriter(const Writer& w);
}
