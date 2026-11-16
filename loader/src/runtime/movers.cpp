#include "runtime/movers.h"

#include "game/game.h"
#include "game/object_cache.h"
#include "game/uobject.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace mirage::movers
{
	namespace
	{
		using Classes::FVector;

		struct Mover
		{
			std::string owner;
			Classes::AActor* actor = nullptr;
			std::size_t slot = 0;
			bool moving = false;
			FVector from{}, to{};
			double start = 0.0, seconds = 0.0;		// World time when sent, and how long the move takes
			bool Alive() const { return game::AliveAt(actor, slot) && !actor->bDeleteMe; }
		};
		std::vector<std::unique_ptr<Mover>> g_movers;

		Mover* Find(const Classes::AActor* actor)
		{
			for (auto& m : g_movers)
				if (m->actor == actor && m->Alive()) return m.get();
			return nullptr;
		}

		double Now()
		{
			auto* w = static_cast<Classes::AWorldInfo*>(game::World());
			return w ? w->TimeSeconds : 0.0;
		}

		// Moved by `delta` the engine's way (Actor.Move: an encroacher's move, carrying what stands on it); its Velocity
		// set for this tick (a based pawn reads it)
		void Step(Mover& m, const FVector& target, float dt)
		{
			const FVector at = m.actor->Location;
			const FVector delta{target.X - at.X, target.Y - at.Y, target.Z - at.Z};
			m.actor->Velocity = dt > 0.f ? FVector{delta.X / dt, delta.Y / dt, delta.Z / dt} : FVector{};
			if (delta.X != 0.f || delta.Y != 0.f || delta.Z != 0.f) m.actor->Move(delta);
		}
	}

	Classes::AActor* Create(const std::string& owner, const Desc& desc, std::string* error)
	{
		auto fail = [&](const char* why) -> Classes::AActor* {
			if (error) *error = why;
			return nullptr;
		};
		if (!desc.mesh) return fail("no mesh");
		auto* pawn = game::Pawn();
		if (!pawn) return fail("no player to spawn from");
		Classes::AActor* a = pawn->Spawn(Classes::ADynamicSMActor_Spawnable::StaticClass(), nullptr, Classes::FName{},
										 FVector{desc.location[0], desc.location[1], desc.location[2]},
										 Classes::FRotator{desc.rotation[0], desc.rotation[1], desc.rotation[2]}, nullptr, true);
		if (!a) return fail("the engine didn't spawn it");
		auto* sm = static_cast<Classes::ADynamicSMActor_Spawnable*>(a);
		sm->SetStaticMesh(desc.mesh, FVector{}, Classes::FRotator{}, FVector{desc.scale[0], desc.scale[1], desc.scale[2]});
		sm->bPawnCanBaseOn = true;
		sm->SetPhysics(Classes::EPhysics::PHYS_Interpolating);
		auto m = std::make_unique<Mover>();
		m->owner = owner;
		m->actor = a;
		m->slot = game::IndexOf(a);
		g_movers.push_back(std::move(m));
		spdlog::info("[mod {}] mover {} ({}, scale {} {} {})", owner, a->GetName(), desc.mesh->GetName(), desc.scale[0], desc.scale[1],
					 desc.scale[2]);
		return a;
	}

	bool MoveTo(Classes::AActor* mover, const float location[3], float seconds)
	{
		Mover* m = Find(mover);
		if (!m) return false;
		m->from = mover->Location;
		m->to = FVector{location[0], location[1], location[2]};
		m->start = Now();
		m->seconds = std::max(0.f, seconds);
		m->moving = true;
		if (m->seconds <= 0.0) {
			Step(*m, m->to, 0.f);
			m->moving = false;
		}
		return true;
	}

	int Moving(Classes::AActor* mover)
	{
		const Mover* m = Find(mover);
		return m ? (m->moving ? 1 : 0) : -1;
	}

	void Remove(Classes::AActor* mover)
	{
		const auto it = std::find_if(g_movers.begin(), g_movers.end(), [&](const auto& m) { return m->actor == mover; });
		if (it == g_movers.end()) return;
		if ((*it)->Alive()) (*it)->actor->Destroy();
		g_movers.erase(it);
	}

	void RemoveAll(const std::string& owner)
	{
		std::erase_if(g_movers, [&](const auto& m) {
			if (m->owner != owner) return false;
			if (m->Alive()) m->actor->Destroy();
			return true;
		});
	}

	void Tick()
	{
		std::erase_if(g_movers, [](const auto& m) { return !m->Alive(); });		// Gone with their level
		const double now = Now();
		for (auto& m : g_movers) {
			if (!m->moving) continue;
			const double t = std::clamp((now - m->start) / m->seconds, 0.0, 1.0);
			const float k = static_cast<float>(t);
			const FVector target{m->from.X + (m->to.X - m->from.X) * k, m->from.Y + (m->to.Y - m->from.Y) * k,
								 m->from.Z + (m->to.Z - m->from.Z) * k};
			auto* w = static_cast<Classes::AWorldInfo*>(game::World());
			const float dt = w ? std::max(0.0001f, static_cast<float>(w->DeltaSeconds)) : 0.f;
			Step(*m, target, dt);
			if (t >= 1.0) {
				m->moving = false;
				m->actor->Velocity = FVector{};
			}
		}
	}
}
