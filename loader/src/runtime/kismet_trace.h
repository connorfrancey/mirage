#pragma once
// The Kismet execution trace (step 5 of the next ten steps, the plan's step 18): which ops of the loaded levels
// activated, on which tick, how many times. While any mod has it on, the ops' ActivateCount (the engine counts every
// activation; an event's in its TriggerCount) is compared after each tick with the tick before; off, nothing runs.
// The ops are listed again at level loads, Kismet edits and every few seconds (a streamed sublevel's sequences).
// Inputs aren't recorded (the engine clears its impulses within the tick). Game thread.

#include <cstdint>
#include <string>

namespace mirage::kismet_trace
{
	// Every engine tick (game thread)
	void Tick();
	// A level finished loading / an op was made: the ops are listed again
	void Invalidate();

	// `on` for the mod; the trace runs while any mod has it on. Its unread entries go when it turns it off.
	void Enable(const std::string& modId, bool on);
	// The mod's entries since it last read them, as JSON [{"tick", "op", "class", "times"}], up to 4096 kept (the
	// oldest go first). Copies up to `capacity` and returns the full length; the entries are taken only when it all fit.
	std::uint32_t Read(const std::string& modId, char* buffer, std::uint32_t capacity);
	// The mod unloaded
	void Forget(const std::string& modId);

	struct Stats
	{
		bool on = false;
		int ops = 0;				// Ops watched
		double lastTickUs = 0.0;	// The last tick's comparison, microseconds
		std::uint64_t activations = 0;
	};
	Stats Get();
}
