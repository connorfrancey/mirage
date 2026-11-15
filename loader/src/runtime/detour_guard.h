#pragma once
// A crash inside a mod's native detour doesn't take the game down (step 2): every detour runs behind a thunk.
//
// The runtime can't put a __try around a detour (it doesn't know the function's parameters or calling convention), so
// the thunk notes, per thread, the registers and the stack as they are when the detour is entered, and puts an exit
// stub in place of the return address. When the detour returns, the stub drops the note and returns to the caller.
// When the detour faults in its own module, a vectored exception handler puts the registers and the stack back as
// they were at the entry and goes on in the function the detour would have called (its *original): the call happens
// as if the detour weren't there. The detour is skipped from then on, and the fault is kept for the runtime to report
// (TakeFaults: the mod is named and its detour removed on the game thread).
//
// What it can't save: a fault in other code the detour called (the game's own, with arguments the detour passed it) is
// a crash like any other; the detour's own stack frames are abandoned, not unwound (no destructors run).

#include <cstdint>
#include <vector>

namespace mirage::detours::guard
{
	// The thunk in front of `detour` (id: the detour's, for the fault report): what the hook, or the detour before it,
	// calls. `original` is where the mod reads the function it calls on; at a fault the call goes on there. Thunks
	// are never freed (a thread may be in one).
	void* Make(void* detour, void** original, int id);
	// The thunk runs `*original` directly from now on (its detour removed, or faulted)
	void Disable(void* thunk);

	struct Fault
	{
		int id = 0;						// The detour's
		std::uintptr_t address = 0;		// The faulting instruction
		unsigned long code = 0;			// The exception code
	};
	// The faults since the last call (a few per call at most). Any thread.
	std::vector<Fault> TakeFaults();
}
