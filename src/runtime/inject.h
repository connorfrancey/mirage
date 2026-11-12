#pragma once
// Script injection (Mirage, step 11): a mod's code called from inside an UnrealScript function's bytecode, at a point
// chosen in it (its head, before each return, around a call to a named function, before a statement), not only before
// or after the whole function like a function hook.
//
// How: the runtime takes one free slot of the engine's native function table (GNatives) for a stub native. At each
// point it splices `if (stub(site)) return ReturnValue;` into the bytecode (UnrealLib's assembler re-lays the jumps;
// jumps that went to the statement now go to the injection, so "before X" runs on every path to X). The stub runs
// the point's callbacks in order with the call's frame: they read and write parameters and locals by name
// (hooks::Call::Param, the function's struct holds both), and setting `skip` makes the function return there with
// what "ReturnValue" holds. Inside a ForEach a return would leave the iterator on its stack: there the injection is
// a plain call and `skip` is ignored (logged once).
//
// A function's injections are applied from its bytecode as it was before the first one (a script patch of step 8
// included): every change rebuilds it from there, and when the last one goes the function gets exactly that back.
// Game thread only (the VM runs there; the stub reads the tables without a lock).

#include "runtime/fhooks.h"

#include <string>
#include <vector>

namespace mirage::inject
{
	// Where in the function (the text form is what mods pass):
	//   "head"                 before the first statement
	//   "return"               before every return
	//   "before:<Function>"    before each statement that calls <Function> (a name: "GetMobilityMultiplier");
	//   "after:<Function>"     after it; "#n" picks the n-th such statement only ("before:SetPhysics#2")
	//   "token:<i>"            before the statement holding token i (the index script_dump prints)
	struct Point
	{
		enum class Kind { Head, Return, BeforeCall, AfterCall, Token } kind = Kind::Head;
		std::string function;		// BeforeCall / AfterCall
		int occurrence = 0;			// 0: every one; n: the n-th (1-based)
		int token = -1;				// Token
	};
	bool ParsePoint(const std::string& text, Point& out, std::string* error);

	// Injects `callback` into `function` ("TdGame.TdMove_Jump.StartMove") at `point`. Callbacks at one place run by
	// priority, higher first, then in the order they were added; the first to set `skip` returns. `owner` (a mod's id)
	// shows in the log and the mod manager. Returns the injection's id (> 0), 0 and `error` when it can't be applied
	// (the function isn't loaded or has no script, the point matches nothing, the stub isn't available): then nothing
	// changes.
	int Add(const std::string& function, const std::string& point, int priority, hooks::Callback callback, void* user,
			const std::string& owner, std::string* error = nullptr);
	void Remove(int id);
	void RemoveAll();		// Every function back to its own bytecode, the stub's slot freed: before the menu unloads

	// Whether injection works in this game (the native table and a free slot were found), and if not, why
	bool Ready(std::string* why = nullptr);

	// ---- The engine's native table, for mod natives (runtime/natives.h) ------------------------------------------------
	// A free slot of GNatives (-1 and `error`: none), its entry set to `exec` (a __thiscall (UObject*, FFrame&, Result)).
	// Released slots go back to the engine's "undefined" entry. Game thread.
	int ClaimNativeSlot(void* exec, std::string* error);
	void ReleaseNativeSlot(int slot);
	void ReleaseNativeSlots();		// All of them: at the menu's unload, after every script patch is undone
	// The running frame a native was called with: where its arguments are (`code`) and the object it runs on
	struct FrameView
	{
		std::uint8_t** code = nullptr;
		Classes::UObject* object = nullptr;
	};
	bool Frame(Classes::UObject* self, void* frame, FrameView& out);
	// One argument expression of the caller's bytecode evaluated into `result` (UE3's FFrame::Step)
	void Step(const FrameView& view, void* frame, void* result);
	bool SkipEndOfParameters(const FrameView& view);	// After the last argument

	// The injections now, for the mod manager. Any thread (a snapshot published at each change).
	struct Info
	{
		int id = 0;
		std::string function, point, owner;
		int priority = 0;
		int places = 0;		// Statements it was spliced at
	};
	std::vector<Info> List();

	struct Stats
	{
		std::uint64_t calls = 0;		// Stub calls
		double microseconds = 0.0;		// In the stub (callbacks included)
		int slot = -1;					// The native index taken
	};
	Stats GetStats();
}
