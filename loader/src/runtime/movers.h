#pragma once
// Movers from mods (step 7 of the next ten steps, Scenario F's interactive object): a moving platform, lift or door.
// The game's own movers are InterpActors driven by Matinee from Kismet; a mod's is a DynamicSMActor_Spawnable (the
// game's spawnable static-mesh actor, which blocks and which pawns can stand on) with PHYS_Interpolating, as an
// InterpActor has: the engine treats it as an encroacher, which pushes what it moves into instead of stopping on it.
// The runtime moves it each tick through the engine's own Actor.Move toward where the mod sent it, at a constant speed
// over the time asked (the world's clock: the same ticks on every run), and gives it the Velocity of that tick, which
// a pawn standing on it reads. Game thread.

#include <string>

namespace Classes
{
	class AActor;
	class UStaticMesh;
}

namespace mirage::movers
{
	struct Desc
	{
		Classes::UStaticMesh* mesh = nullptr;
		float location[3]{};
		int rotation[3]{};			// Pitch, yaw, roll
		float scale[3]{1.f, 1.f, 1.f};
	};

	// A mover of `owner` (a mod id). nullptr and `error` if it can't be made.
	Classes::AActor* Create(const std::string& owner, const Desc& desc, std::string* error);
	// Sent toward `location`, arriving after `seconds` (0: at once). False if `mover` isn't one.
	bool MoveTo(Classes::AActor* mover, const float location[3], float seconds);
	// 1 moving, 0 still, -1 not a mover
	int Moving(Classes::AActor* mover);
	// Taken out (destroyed)
	void Remove(Classes::AActor* mover);
	void RemoveAll(const std::string& owner);
	// Every engine tick: moving movers advanced; gone ones dropped
	void Tick();
}
