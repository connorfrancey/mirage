#pragma once
// Mods' input actions (step 8 of the next ten steps, the plan's step 22): a mod declares them in mod.toml
// ([actions.dash] key = "Q"); when the engine gets that key (the input monitor sees what the engine really gets: the
// player's own input, and a TAS's recorded or replayed input, which comes in the same way), the action goes to that mod
// as MIRAGE_EVENT_ACTION (text = the action's name, value = 1 pressed, 0 released). The key event is queued as the
// engine processes it and delivered at the runtime's next tick, the same frame. Game thread.

#include <string>

namespace mirage::actions
{
	// Listens to the input monitor (after it is installed)
	void Install();
	// Every engine tick: the queued actions delivered
	void Tick();
	// How many were delivered since the menu started (the self-test reads it)
	unsigned Delivered();
}
