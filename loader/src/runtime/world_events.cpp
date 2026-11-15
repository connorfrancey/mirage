#include "runtime/world_events.h"

#include "core/clock.h"
#include "game/game.h"
#include "game/object_query.h"
#include "runtime/mod_game.h"
#include "sdk/SDK.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <vector>

namespace mirage::world_events
{
	namespace
	{
		Stats g_stats;
		bool g_newLevel = true;							// The next tick takes the world as it is, silently
		game::ObjectId g_pawn;							// The player's pawn last tick
		std::vector<Classes::AActor*> g_raw;				// In play last tick, as the levels list them
		std::vector<Classes::AActor*> g_actors;			// The same, sorted by address
		bool g_tracking = false;						// g_actors is current (actor events were subscribed last tick)
		std::vector<std::string> g_visible;				// Streaming levels visible last tick, sorted

		void Restarts()
		{
			auto* pawn = game::Pawn();
			if (!pawn) return;
			const game::ObjectId id = game::IdOf(pawn);
			if (!g_newLevel && g_pawn.address && id != g_pawn) {	// A new pawn, even at the old one's address
				++g_stats.restarts;
				mods::DispatchRestart();
			}
			g_pawn = id;
		}

		void Streaming()
		{
			std::vector<std::string> visible;
			if (auto* world = static_cast<Classes::AWorldInfo*>(game::World()))
				for (std::size_t i = 0; i < world->StreamingLevels.Num(); ++i)
					if (Classes::ULevelStreaming* s = world->StreamingLevels[i]; s && s->bIsVisible && s->LoadedLevel)
						visible.push_back(s->PackageName.GetName());
			std::sort(visible.begin(), visible.end());
			if (!g_newLevel && visible != g_visible) {
				std::vector<std::string> in, out;
				std::set_difference(visible.begin(), visible.end(), g_visible.begin(), g_visible.end(), std::back_inserter(in));
				std::set_difference(g_visible.begin(), g_visible.end(), visible.begin(), visible.end(), std::back_inserter(out));
				for (const auto& name : out) {
					++g_stats.streamed;
					mods::DispatchLevelStreamed(false, name);
				}
				for (const auto& name : in) {
					++g_stats.streamed;
					mods::DispatchLevelStreamed(true, name);
				}
			}
			g_visible = std::move(visible);
		}

		void Actors()
		{
			if (!mods::HasSubscribers(MIRAGE_EVENT_ACTOR_ADDED) && !mods::HasSubscribers(MIRAGE_EVENT_ACTOR_REMOVED)) {
				g_tracking = false;		// Nobody asks: no work, and the next subscriber starts from that tick's world
				g_actors.clear();
				g_raw.clear();
				return;
			}
			const auto started = clk::Now();
			// The levels' lists as they are: the same as last tick (the usual case) is one compare of memory. UE3 takes
			// a destroyed actor out of its level's list at once, so a change of the list is every change there is.
			const std::vector<Classes::AActor*>& raw = game::ActorsInPlay();
			const auto done = [&] {
				++g_stats.compares;
				g_stats.lastCompareMs = clk::SecondsSince(started) * 1000.0;
				g_stats.totalCompareMs += g_stats.lastCompareMs;
				g_stats.maxCompareMs = std::max(g_stats.maxCompareMs, g_stats.lastCompareMs);
			};
			if (g_tracking && !g_newLevel && raw == g_raw) {
				done();
				g_stats.unchangedCompareMs += g_stats.lastCompareMs;
				return;
			}
			++g_stats.changed;
			g_raw = raw;
			std::vector<Classes::AActor*> now;
			now.reserve(raw.size());
			for (Classes::AActor* a : raw)
				if (!a->bDeleteMe) now.push_back(a);
			std::sort(now.begin(), now.end());
			if (g_tracking && !g_newLevel) {
				std::vector<Classes::AActor*> added, removed;
				std::set_difference(now.begin(), now.end(), g_actors.begin(), g_actors.end(), std::back_inserter(added));
				std::set_difference(g_actors.begin(), g_actors.end(), now.begin(), now.end(), std::back_inserter(removed));
				for (Classes::AActor* a : removed) {
					++g_stats.removed;
					mods::DispatchActor(false, a, std::string());	// Gone: not read (its memory may be another's)
				}
				for (Classes::AActor* a : added) {
					++g_stats.added;
					mods::DispatchActor(true, a, a->Class ? a->Class->GetName() : std::string());
				}
			}
			g_actors = std::move(now);
			g_tracking = true;
			g_stats.actors = g_actors.size();
			done();
		}
	}

	void OnLevelLoaded()
	{
		g_newLevel = true;
		g_pawn = {};
	}

	void Tick()
	{
		if (!game::World()) return;
		Restarts();
		Streaming();
		Actors();
		if (game::Pawn()) g_newLevel = false;	// The new level is in play once its player is
	}

	Stats GetStats() { return g_stats; }
}
