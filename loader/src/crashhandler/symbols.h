#pragma once
#include <cstdint>

#include "crashhandler/analysis.h"

// Names for code addresses in a crash report.
//
//   - Modules with a PDB (Mirage.dll, a mod's DLL built with one) and the system DLLs: dbghelp, with source lines.
//   - The game's executable has no symbols: an address in it is named after the nearest engine function the runtime
//     knows, a native UnrealScript function (its UFunction::Func: "Engine.Actor.Trace (native)") or one the game layer
//     found by pattern ("ProcessEvent", GMalloc's functions). The table is built on the game thread (Refresh) and
//     only read after a crash, without locks: the crashed thread may hold any of the runtime's.
namespace crashhandler::symbols
{
	// Game thread: the engine's names again (after the game layer is up, and at level loads, when classes may have
	// loaded). Cheap when the native functions didn't change.
	void Refresh();

	// dbghelp for one crash report, on the reporter thread (dbghelp is single-threaded: nothing else uses it then)
	class Session
	{
	public:
		Session();
		~Session();
		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;
	private:
		bool ok_ = false;
	};

	// Everything known about `address`. Reads module lists and symbol files, never game memory.
	Place Describe(std::uint32_t address);

	// GMalloc's Malloc, Realloc and Free (from the last Refresh): the address is in one of them
	bool InAllocator(std::uint32_t address);
}
