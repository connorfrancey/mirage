#include "runtime/physics.h"

#include "game/game.h"
#include "game/object_query.h"
#include "game/uobject.h"
#include "runtime/symbols.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <map>

namespace mirage::physics
{
	namespace
	{
		using Classes::FVector;
		FVector V(const Vec& v) { return FVector{v.x, v.y, v.z}; }
		Vec V(const FVector& v) { return Vec{v.X, v.Y, v.Z}; }

		constexpr float kMaxMassScale = 100.f;		// The engine's cap on RB_BodySetup.MassScale (measured, see SpawnProp)
		// The mass scale each body was given (keyed by its component)
		std::map<const void*, float> g_massScale;

		// UWorld::SingleLineCheck(FCheckResult& Hit, AActor* SourceActor, const FVector& End, const FVector& Start,
		// DWORD TraceFlags, const FVector& Extent, FLightingChannelContainer* LightingChannels): Actor.Trace's native.
		// TRUE when nothing was hit.
		using LineCheckFn = int(__thiscall*)(void* world, void* hit, Classes::AActor* source, const FVector* end,
											 const FVector* start, std::uint32_t flags, const FVector* extent, void* channels);
		// FCheckResult as the engine fills it (0x4C bytes)
		struct CheckResult
		{
			void* next;
			Classes::AActor* actor;			// +0x04
			FVector location;				// +0x08
			FVector normal;					// +0x14
			float time;						// +0x20
			std::int32_t item;				// +0x24
			void* material;					// +0x28
			void* physMaterial;				// +0x2C
			Classes::UPrimitiveComponent* component;	// +0x30
			Classes::FName bone;			// +0x34
			void* level;					// +0x3C
			std::int32_t levelIndex;		// +0x40
			std::uint32_t pad[2];
		};
		static_assert(offsetof(CheckResult, component) == 0x30 && offsetof(CheckResult, levelIndex) == 0x40);
		constexpr std::uint32_t kTraceWorld = 0x2086;		// What Actor.Trace asks for without actors
		constexpr std::uint32_t kTraceActorsToo = 0x20BF;	// ... and with them

		// The engine's UWorld: the persistent level's outer (WorldInfo -> PersistentLevel -> TheWorld)
		void* World()
		{
			Classes::AWorldInfo* info = game::World();
			if (!info || !info->Outer || !info->Outer->Outer) return nullptr;
			return info->Outer->Outer;
		}

		Classes::URB_BodySetup* SetupOf(Classes::UPrimitiveComponent* c)
		{
			if (!c || !c->IsA(Classes::UStaticMeshComponent::StaticClass())) return nullptr;
			Classes::UStaticMesh* mesh = static_cast<Classes::UStaticMeshComponent*>(c)->StaticMesh;
			return mesh ? mesh->BodySetup : nullptr;
		}
	}

	Classes::UPrimitiveComponent* BodyComponent(Classes::AActor* actor)
	{
		if (!actor || actor->bDeleteMe) return nullptr;
		Classes::UPrimitiveComponent* c = actor->CollisionComponent;
		return c && c->BodyInstance ? c : nullptr;
	}

	bool Get(Classes::AActor* actor, Body& out)
	{
		out = Body{};
		if (!actor || actor->bDeleteMe) return false;
		out.location = V(actor->Location);
		out.pitch = actor->Rotation.Pitch;
		out.yaw = actor->Rotation.Yaw;
		out.roll = actor->Rotation.Roll;
		Classes::UPrimitiveComponent* c = BodyComponent(actor);
		out.simulated = c && actor->Physics.GetValue() == Classes::EPhysics::PHYS_RigidBody;
		if (!c) {
			out.linear = V(actor->Velocity);
			return true;
		}
		Classes::URB_BodyInstance* body = c->BodyInstance;
		out.linear = V(body->GetUnrealWorldVelocity());
		out.angular = V(body->GetUnrealWorldAngularVelocity());
		out.gravityScale = body->CustomGravityFactor;
		out.awake = c->RigidBodyIsAwake(Classes::FName{});
		const auto it = g_massScale.find(c);
		out.massScale = it != g_massScale.end() ? it->second : 1.f;
		return true;
	}

	bool Set(Classes::AActor* actor, const Body& in, std::uint32_t which, std::string* error)
	{
		Classes::UPrimitiveComponent* c = BodyComponent(actor);
		if (!c) {
			if (error) *error = "the actor has no rigid body";
			return false;
		}
		if (which & kMass) {
			// A live body's mass can't be changed here: the engine reads the mass from the mesh's BodySetup when it makes
			// the body, and nothing reachable makes it again (detach and attach keep the same PhysX actor; a mesh swap
			// destroys it for good). A prop gets its mass when it is spawned (SpawnProp's massScale).
			if (error) *error = "a live body's mass can't be changed in this engine: give it when spawning the prop (physics_spawn's mass_scale)";
			return false;
		}
		const Classes::FName none{};
		if (which & kLocation) c->SetRBPosition(V(in.location), none);
		if (which & kRotation) c->SetRBRotation(Classes::FRotator{in.pitch, in.yaw, in.roll}, none);
		if (which & kLinear) c->SetRBLinearVelocity(V(in.linear), false);
		if (which & kAngular) c->SetRBAngularVelocity(V(in.angular), false);
		if (which & kGravity) c->BodyInstance->CustomGravityFactor = in.gravityScale;
		if (which & kAwake) {
			if (in.awake) c->WakeRigidBody(none);
			else c->PutRigidBodyToSleep(none);
		}
		return true;
	}

	bool Impulse(Classes::AActor* actor, const Vec& impulse, const Vec* at, bool velocityChange)
	{
		Classes::UPrimitiveComponent* c = BodyComponent(actor);
		if (!c) return false;
		c->WakeRigidBody(Classes::FName{});
		c->AddImpulse(V(impulse), at ? V(*at) : FVector{}, Classes::FName{}, velocityChange);
		return true;
	}

	bool Force(Classes::AActor* actor, const Vec& force, const Vec* at)
	{
		Classes::UPrimitiveComponent* c = BodyComponent(actor);
		if (!c) return false;
		c->WakeRigidBody(Classes::FName{});
		c->AddForce(V(force), at ? V(*at) : FVector{}, Classes::FName{});
		return true;
	}

	Classes::AActor* SpawnProp(Classes::UStaticMesh* mesh, const Vec& location, int pitch, int yaw, int roll, float massScale,
							   std::string* error)
	{
		auto fail = [&](const char* why) -> Classes::AActor* {
			if (error) *error = why;
			return nullptr;
		};
		if (!mesh) return fail("no mesh");
		if (!mesh->BodySetup) return fail("the mesh has no collision for a rigid body (RB_BodySetup)");
		auto* pawn = game::Pawn();
		if (!pawn) return fail("no player to spawn from");
		auto* a = pawn->Spawn(Classes::AKActorSpawnable::StaticClass(), nullptr, Classes::FName{}, V(location),
							  Classes::FRotator{pitch, yaw, roll}, nullptr, true);
		if (!a) return fail("the engine didn't spawn it (inside something?)");
		auto* prop = static_cast<Classes::AKActorSpawnable*>(a);
		if (!(massScale > 0.f)) massScale = 1.f;
		// The body is made here, from the mesh's BodySetup: its MassScale scaled for this one body only. The engine
		// caps MassScale at 100 (measured: a box of 50 weighs the same at 2x and 4x): the scale is capped here too, so
		// it reads back as what the body got.
		Classes::URB_BodySetup* setup = mesh->BodySetup;
		const float own = setup->MassScale;
		if (own > 0.f && own * massScale > kMaxMassScale) {
			spdlog::warn("Physics: {}x {}'s mass is over the engine's cap (MassScale {} x {} > {}): {:.2f}x", massScale, mesh->GetName(),
						 own, massScale, kMaxMassScale, kMaxMassScale / own);
			massScale = kMaxMassScale / own;
		}
		setup->MassScale = own * massScale;
		prop->SetStaticMesh(mesh, FVector{}, Classes::FRotator{}, FVector{1.f, 1.f, 1.f});
		if (prop->Physics.GetValue() != Classes::EPhysics::PHYS_RigidBody) prop->SetPhysics(Classes::EPhysics::PHYS_RigidBody);
		setup->MassScale = own;
		Classes::UPrimitiveComponent* c = BodyComponent(prop);
		if (!c) {
			prop->Destroy();
			return fail(massScale != 1.f ? "the engine made no rigid body for it at that mass scale" : "the engine made no rigid body for it");
		}
		g_massScale[c] = massScale;
		c->WakeRigidBody(Classes::FName{});
		return prop;
	}

	Collision GetCollision(Classes::AActor* actor)
	{
		Collision c;
		if (!actor) return c;
		c.collideActors = actor->bCollideActors;
		c.blockActors = actor->bBlockActors;
		c.blockRigidBody = actor->CollisionComponent && actor->CollisionComponent->BlockRigidBody;
		return c;
	}

	void SetCollision(Classes::AActor* actor, const Collision& c)
	{
		if (!actor) return;
		actor->SetCollision(c.collideActors, c.blockActors, actor->bIgnoreEncroachers);
		if (actor->CollisionComponent) actor->CollisionComponent->SetBlockRigidBody(c.blockRigidBody);
	}

	bool TraceAvailable() { return symbols::Find("UWorld::SingleLineCheck") != 0; }

	bool Trace(const Vec& start, const Vec& end, const Vec* extent, std::uint32_t flags, Classes::AActor* ignore, Hit& out)
	{
		out = Hit{};
		static auto check = reinterpret_cast<LineCheckFn>(symbols::Find("UWorld::SingleLineCheck"));
		void* world = World();
		if (!check || !world) return false;
		CheckResult r{};
		r.time = 1.f;
		r.item = -1;
		r.levelIndex = -1;
		const FVector s = V(start), e = V(end), x = extent ? V(*extent) : FVector{};
		const int nothing = check(world, &r, ignore, &e, &s, (flags & kActors) ? kTraceActorsToo : kTraceWorld, &x, nullptr);
		out.hit = !nothing && r.time < 1.f;
		if (!out.hit) return true;
		out.actor = r.actor;
		out.component = r.component;
		out.location = V(r.location);
		out.normal = V(r.normal);
		out.time = r.time;
		if (r.bone.Index > 0) out.bone = r.bone.GetName();
		return true;
	}

	std::vector<Classes::AActor*> Overlap(const Vec& center, float radius, const void* cls)
	{
		std::vector<Classes::AActor*> out;
		auto* filter = static_cast<Classes::UClass*>(const_cast<void*>(cls));
		for (Classes::AActor* a : game::ActorsOf(filter ? filter : Classes::AActor::StaticClass())) {
			if (!a || a->bDeleteMe || !a->CollisionComponent) continue;
			const auto& b = a->CollisionComponent->Bounds;
			// The sphere against the bounds' box: the box's nearest point to the centre
			const float dx = std::fmax(std::fabs(center.x - b.Origin.X) - b.BoxExtent.X, 0.f);
			const float dy = std::fmax(std::fabs(center.y - b.Origin.Y) - b.BoxExtent.Y, 0.f);
			const float dz = std::fmax(std::fabs(center.z - b.Origin.Z) - b.BoxExtent.Z, 0.f);
			if (dx * dx + dy * dy + dz * dz <= radius * radius) out.push_back(a);
		}
		return out;
	}
}
