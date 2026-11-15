#pragma once
// Triggers from mods (step 7 of the next ten steps, the plan's step 21). A trigger is the engine's own Trigger actor (a
// cylinder: its CylinderComponent sized as asked), so the engine finds what touches it and keeps it in the trigger's
// Touching list; each tick the runtime compares that list with the last tick's: enter, leave, and (when asked) stay.
// The engine doesn't send its Touch event to a Trigger spawned at runtime (only to placed ones: measured), so the list
// is what's read; the cost is a few entries a trigger a tick. Filters by class (IsA) and tag (Actor.Tag). Box and
// sphere triggers aren't possible here: a volume needs a brush model, which isn't made at runtime. Game thread.

#include <cstdint>
#include <functional>
#include <string>

namespace Classes
{
	class AActor;
	class UClass;
}

namespace mirage::triggers
{
	enum class Kind { Enter = 0, Leave = 1, Stay = 2 };
	using Callback = std::function<void(Classes::AActor* trigger, Classes::AActor* other, Kind kind)>;

	struct Desc
	{
		float x = 0, y = 0, z = 0;
		float radius = 100.f, height = 100.f;	// The cylinder: height is half its height, as CylinderComponent has it
		Classes::UClass* filterClass = nullptr;	// Only actors of this class (or a subclass); null: any
		std::string tag;						// Only actors with this Tag; "": any
		bool stay = false;						// Call Kind::Stay every tick for each actor inside
	};

	// A trigger of `owner` (a mod id). nullptr and `error` if it can't be made.
	Classes::AActor* Create(const std::string& owner, const Desc& desc, Callback callback, std::string* error);
	// Taken out (destroyed): its callbacks stop at once
	void Remove(Classes::AActor* trigger);
	// Every trigger of `owner` taken out (its unload)
	void RemoveAll(const std::string& owner);
	// Actors inside now (-1: not one of ours)
	int Count(Classes::AActor* trigger);
	// Every engine tick: enter, leave and stay callbacks; gone triggers dropped
	void Tick();
}
