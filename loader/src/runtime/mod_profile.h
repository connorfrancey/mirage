#pragma once
// The profiler per mod (step 27): the time each mod's code takes, by callback. Every call into a mod passes one of two
// gates (detail::Enter for hooks, events, natives, triggers and moves; ModModule::Call for a module's callbacks), and
// both record here how long it ran. Once a second (real time, checked each engine tick) the calls of that second are summed per mod and per
// callback: a mod's milliseconds per engine tick and its slowest callback, shown in the mod manager. A mod over the
// budget for a whole second is named in the log (once, until it is under again).
//
// Not counted: a native mod's own detours and mid-function hooks (the engine calls them directly, no gate) and the
// mod's load and unload. No game or engine here (unit tested in tests/test_mod_profile.cpp).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mirage::mods::profile
{
	// A mod's code per engine tick, averaged over a second, above which it is named (2 ms: an eighth of a 62 fps frame)
	inline constexpr double kBudgetMs = 2.0;

	struct Callback
	{
		std::string what;					// "tick event", "hook Engine.Actor.Touch", "Double Jump on_tick"
		std::uint64_t calls = 0;			// Since the mod loaded
		double totalMs = 0.0, maxMs = 0.0;	// Since the mod loaded: all calls, and the longest one
		double lastSecondMs = 0.0;			// In the last full second
	};
	struct Mod
	{
		std::string id;
		double msPerTick = 0.0;				// Its code per engine tick over the last full second
		bool overBudget = false;
		std::vector<Callback> callbacks;	// Slowest in the last second first
	};

	// A call into `mod`'s code took `ms`. Any thread.
	void Record(std::string_view mod, std::string_view what, double ms);
	// An engine tick ended at `seconds` (game time, or any clock that only goes forward). Game thread.
	void Tick(double seconds);
	// Every mod that ran code since it loaded, highest per tick first. Any thread.
	std::vector<Mod> List();
	// `mod` unloaded (or reloads): its numbers start again. Any thread.
	void Forget(std::string_view mod);
	// Everything forgotten (tests).
	void Reset();
}
