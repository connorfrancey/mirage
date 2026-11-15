#pragma once
// Managed native detours (Mirage, step 6): mods detour the game's native code through the runtime, which owns the one
// hook per target (core/hooking.h) and chains the mods' detours on it in priority order.
//
//     static int (__thiscall* original)(void* self, int x) = nullptr;
//     int __fastcall Mine(void* self, void*, int x) { return original(self, x) + 1; }
//     detours::Add(target, &Mine, 10, reinterpret_cast<void**>(&original), "my-mod", &error);
//
// A chain on one target runs from the highest priority down: each detour's `*original` is the next detour, the last
// one's is the real function. When the chain changes (a detour added or removed), every `*original` is set again, so a
// mod keeps calling its `original` as before. Two mods on one target is logged (who, in what order); a target the menu
// hooks itself is refused.
//
// Every detour runs behind a guard (runtime/detour_guard.h): when it faults in its own code, the call goes on in the
// function it would have called, the detour is skipped from then on, and TakeFaults reports it (the runtime removes it
// and names the mod).
//
// A detour that is removed while another thread runs inside it (the game calling the function at that moment) is not
// waited for: remove detours when the code they hook isn't running (a mod's unload, which waits for its other calls).

#include <cstdint>
#include <string>
#include <vector>

namespace mirage::detours
{
	// `owner` names who it is for in messages ("hello"). Returns its id (> 0), or 0 and `error`.
	int Add(void* target, void* detour, int priority, void** original, const std::string& owner, std::string* error = nullptr);
	void Remove(int id);
	void RemoveAll();

	// The chain on `target`, first to last: "owner (priority)" each. Empty if none.
	std::vector<std::string> Chain(void* target);

	// Detours that faulted since the last call (the guard skips them already; the runtime removes them). Any thread.
	struct Fault
	{
		int id = 0;
		std::string owner;
		void* target = nullptr;
		std::uintptr_t address = 0;		// The faulting instruction (in the owner's code)
		unsigned long code = 0;
	};
	std::vector<Fault> TakeFaults();
}
