#pragma once
// Physics and world queries (step 6 of the next ten steps, the plan's step 20). Rigid bodies through the engine's own
// script functions (UPrimitiveComponent's SetRBLinearVelocity, AddImpulse, AddForce, WakeRigidBody, ...); props are
// KActorSpawnable actors given a static mesh; traces call UWorld::SingleLineCheck directly (runtime/symbols.toml), the
// function Actor.Trace ends in, without the script VM in between. Game thread.
//
// Mass: the engine reads it from the mesh's RB_BodySetup (MassScale) when it makes a body, and nothing reachable makes
// a body again afterwards (detaching and attaching the component keeps the same PhysX actor; swapping the mesh destroys
// it for good: tried in the self-test's development). So mass is given when a prop is spawned: SpawnProp scales its
// mesh's MassScale for that one body and puts it back at once. A live body's mass can't be changed.

#include <cstdint>
#include <string>
#include <vector>

namespace Classes
{
	class AActor;
	class UPrimitiveComponent;
	class UStaticMesh;
}

namespace mirage::physics
{
	struct Vec
	{
		float x = 0, y = 0, z = 0;
	};

	struct Body
	{
		Vec location;
		int pitch = 0, yaw = 0, roll = 0;
		Vec linear, angular;		// Unreal units a second; angular in radians a second
		float massScale = 1.f;		// The mass scale it was spawned with (1: the mesh's own mass; not a prop: 1)
		float gravityScale = 1.f;	// RB_BodyInstance.CustomGravityFactor
		bool awake = false;
		bool simulated = false;		// Physics == PHYS_RigidBody with a body
	};

	// The actor's collision component when it has a rigid body (KActor, a prop); nullptr otherwise
	Classes::UPrimitiveComponent* BodyComponent(Classes::AActor* actor);
	bool Get(Classes::AActor* actor, Body& out);

	enum Field : std::uint32_t
	{
		kLocation = 1, kRotation = 2, kLinear = 4, kAngular = 8, kMass = 16, kGravity = 32, kAwake = 64,
	};
	// The fields in `which` from `in`. False if the actor has no body (or the mass couldn't be changed: `error`).
	// kMass: refused (see above)
	bool Set(Classes::AActor* actor, const Body& in, std::uint32_t which, std::string* error);
	bool Impulse(Classes::AActor* actor, const Vec& impulse, const Vec* at, bool velocityChange);
	bool Force(Classes::AActor* actor, const Vec& force, const Vec* at);

	// A KActorSpawnable at `location` with `mesh` (it needs collision for a body), simulated and awake, its mass the
	// mesh's times `massScale`. nullptr and `error` if not.
	Classes::AActor* SpawnProp(Classes::UStaticMesh* mesh, const Vec& location, int pitch, int yaw, int roll, float massScale,
							   std::string* error);

	struct Collision
	{
		bool collideActors = false, blockActors = false, blockRigidBody = false;
	};
	Collision GetCollision(Classes::AActor* actor);
	void SetCollision(Classes::AActor* actor, const Collision& c);

	// ---- Queries -------------------------------------------------------------------------------------------------------
	struct Hit
	{
		bool hit = false;
		Classes::AActor* actor = nullptr;				// The world's own geometry: the level's WorldInfo
		Classes::UPrimitiveComponent* component = nullptr;
		Vec location, normal;
		float time = 1.f;								// Where along the trace, 0..1
		std::string bone;								// A skeletal mesh's bone, else ""
	};
	enum TraceFlags : std::uint32_t
	{
		kActors = 1,			// Actors too (else the world's geometry: BSP, static meshes, terrain)
	};
	// A line (extent null or zero) or a box swept along it (extent: half sizes). `ignore`: an actor the trace doesn't hit.
	// False if the engine's line check isn't known (the log says why).
	bool Trace(const Vec& start, const Vec& end, const Vec* extent, std::uint32_t flags, Classes::AActor* ignore, Hit& out);
	bool TraceAvailable();

	// Actors in play whose collision bounds touch the sphere (bounds, not shapes: a box around a ramp overlaps more than
	// the ramp does), of `cls` or a subclass (null: any)
	std::vector<Classes::AActor*> Overlap(const Vec& center, float radius, const void* cls);
}
