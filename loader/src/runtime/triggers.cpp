#include "runtime/triggers.h"

#include "game/game.h"
#include "game/object_cache.h"
#include "game/uobject.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace mirage::triggers
{
	namespace
	{
		struct Ref
		{
			Classes::AActor* actor = nullptr;
			std::size_t slot = 0;
			bool Alive() const { return game::AliveAt(actor, slot) && !actor->bDeleteMe; }
		};
		Ref RefOf(Classes::AActor* a) { return {a, game::IndexOf(a)}; }

		struct Trigger
		{
			std::string owner;
			Ref self;
			Desc desc;
			Classes::FName tag;					// desc.tag as a name (Index 0: any)
			Callback callback;
			std::vector<Ref> inside;
		};
		std::vector<std::unique_ptr<Trigger>> g_triggers;

		Trigger* Find(const void* actor)
		{
			for (auto& t : g_triggers)
				if (t->self.actor == actor && t->self.Alive()) return t.get();
			return nullptr;
		}

		bool Passes(const Trigger& t, Classes::AActor* other)
		{
			if (!other || other->bDeleteMe) return false;
			if (t.desc.filterClass && !other->IsA(t.desc.filterClass)) return false;
			// A tag the engine has no name for matches nothing (no actor can have it)
			if (!t.desc.tag.empty() && (t.tag.Index <= 0 || !(other->Tag.Index == t.tag.Index && other->Tag.Number == t.tag.Number)))
				return false;
			return true;
		}

	}

	Classes::AActor* Create(const std::string& owner, const Desc& desc, Callback callback, std::string* error)
	{
		auto fail = [&](const char* why) -> Classes::AActor* {
			if (error) *error = why;
			return nullptr;
		};
		if (!(desc.radius > 0.f) || !(desc.height > 0.f)) return fail("radius and height must be above 0");
		auto* pawn = game::Pawn();
		if (!pawn) return fail("no player to spawn from");
		// Mirror's Edge marks Trigger (and even Trigger_Dynamic) bNoDelete, placed-only, and Spawn refuses a class whose
		// defaults say so: the defaults' bit off for this spawn, and the new trigger's off for good (it is destroyed on
		// revert)
		auto* defaults = game::FindObject<Classes::AActor>("Trigger Engine.Default__Trigger");
		if (!defaults) return fail("Trigger's defaults weren't found");
		const bool noDelete = defaults->bNoDelete;
		defaults->bNoDelete = false;
		Classes::AActor* a = pawn->Spawn(Classes::ATrigger::StaticClass(), nullptr, Classes::FName{},
										 Classes::FVector{desc.x, desc.y, desc.z}, Classes::FRotator{}, nullptr, true);
		defaults->bNoDelete = noDelete;
		if (!a) return fail("the engine didn't spawn the trigger");
		a->bNoDelete = false;
		auto* trig = static_cast<Classes::ATrigger*>(a);
		if (trig->CylinderComponent) trig->CylinderComponent->SetCylinderSize(desc.radius, desc.height);
		auto t = std::make_unique<Trigger>();
		t->owner = owner;
		t->self = RefOf(a);
		t->desc = desc;
		if (!desc.tag.empty()) t->tag = Classes::FName(desc.tag.c_str());
		t->callback = std::move(callback);
		g_triggers.push_back(std::move(t));
		return a;
	}

	void Remove(Classes::AActor* trigger)
	{
		const auto it = std::find_if(g_triggers.begin(), g_triggers.end(), [&](const auto& t) { return t->self.actor == trigger; });
		if (it == g_triggers.end()) return;
		if ((*it)->self.Alive()) (*it)->self.actor->Destroy();
		g_triggers.erase(it);
	}

	void RemoveAll(const std::string& owner)
	{
		for (auto it = g_triggers.begin(); it != g_triggers.end();) {
			if ((*it)->owner != owner) {
				++it;
				continue;
			}
			if ((*it)->self.Alive()) (*it)->self.actor->Destroy();
			it = g_triggers.erase(it);
		}
	}

	int Count(Classes::AActor* trigger)
	{
		Trigger* t = Find(trigger);
		return t ? static_cast<int>(t->inside.size()) : -1;
	}

	void Tick()
	{
		std::erase_if(g_triggers, [](const auto& t) { return !t->self.Alive(); });		// Gone with their level
		// What the engine says each trigger touches now (its Touching list), against what it touched last tick. A callback
		// may remove triggers (its own too): by actor, looked up again each time.
		std::vector<Classes::AActor*> all;
		for (auto& t : g_triggers) all.push_back(t->self.actor);
		for (Classes::AActor* a : all) {
			Trigger* t = Find(a);
			if (!t) continue;
			std::vector<Ref> now;
			const auto& touching = a->Touching;
			for (std::size_t i = 0; i < touching.Num(); ++i)
				if (Classes::AActor* other = touching[i]; other && other != a && Passes(*t, other)) now.push_back(RefOf(other));
			const auto has = [](const std::vector<Ref>& v, const Classes::AActor* x) {
				return std::any_of(v.begin(), v.end(), [&](const Ref& r) { return r.actor == x; });
			};
			std::vector<Classes::AActor*> left, entered, stayed;
			for (const Ref& r : t->inside)
				if (!has(now, r.actor)) left.push_back(r.actor);
			for (const Ref& r : now)
				(has(t->inside, r.actor) ? stayed : entered).push_back(r.actor);
			t->inside = std::move(now);
			const Callback callback = t->callback;
			const bool stay = t->desc.stay;
			for (Classes::AActor* x : left) callback(a, x, Kind::Leave);
			for (Classes::AActor* x : entered) callback(a, x, Kind::Enter);
			if (stay)
				for (Classes::AActor* x : stayed) callback(a, x, Kind::Stay);
		}
	}
}
