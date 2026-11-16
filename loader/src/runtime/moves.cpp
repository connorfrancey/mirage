#include "runtime/moves.h"

#include "game/game.h"
#include "game/object_cache.h"
#include "game/uobject.h"
#include "core/clock.h"
#include "runtime/engine_calls.h"
#include "runtime/symbols.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace mirage::moves
{
	namespace
	{
		// Slots no player move uses: the player pawn's MoveClasses are None there (TdPawn's defaults), the obsolete moves
		// first. Checked again on each pawn before use.
		constexpr int kSlots[] = {58, 59, 80, 83, 86, 87, 88, 89, 90, 92};

		struct Move
		{
			std::string owner, name;
			Classes::UClass* cls = nullptr;
			int slot = 0;
			Callback callback;
			Classes::ATdPawn* pawn = nullptr;		// The pawn it is installed in, and the object there
			game::ObjectId pawnId{};
			Classes::UTdMove* object = nullptr;
			Classes::UTdMove* original = nullptr;	// What the slot held before (None for the player)
			bool active = false;					// The player is in it (the start callback ran)
		};
		std::vector<std::unique_ptr<Move>> g_moves;
		TickStats g_stats;

		Move* Find(int slot)
		{
			for (auto& m : g_moves)
				if (m->slot == slot) return m.get();
			return nullptr;
		}

		// The engine's object constructor (as the script VM's `new` does)
		Classes::UObject* Construct(Classes::UClass* cls, Classes::UObject* outer)
		{
			using Fn = Classes::UObject*(__cdecl*)(Classes::UClass*, Classes::UObject*, int, int, std::uint32_t, std::uint32_t,
												   Classes::UObject*, void*, Classes::UObject*, void*);
			auto construct = reinterpret_cast<Fn>(symbols::Find("StaticConstructObject"));
			return construct ? construct(cls, outer, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr) : nullptr;
		}

		bool SlotFree(Classes::ATdPawn* pawn, int slot)
		{
			return slot < static_cast<int>(pawn->Moves.Num()) && slot < static_cast<int>(pawn->MoveClasses.Num()) &&
				   !pawn->MoveClasses[slot];
		}

		// The pawn on its way out of the move, to the game's own: falling in the air, walking on the ground
		void LeaveMove(Classes::ATdPawn* pawn)
		{
			const bool air = pawn->Physics.GetValue() == Classes::EPhysics::PHYS_Falling;
			engine::SetMove(pawn, air ? Classes::EMovement::MOVE_Falling : Classes::EMovement::MOVE_Walking);
		}

		bool Installed(const Move& m, Classes::ATdPawn* pawn)
		{
			return pawn && m.pawn == pawn && m.pawnId == game::IdOf(pawn) && m.object && m.slot < static_cast<int>(pawn->Moves.Num()) &&
				   pawn->Moves[m.slot] == m.object;
		}

		// Its object in the player's pawn (a new pawn after a restart gets a new one)
		bool Install(Move& m, Classes::ATdPawn* pawn)
		{
			if (Installed(m, pawn)) return true;
			if (!SlotFree(pawn, m.slot)) return false;
			auto* o = static_cast<Classes::UTdMove*>(Construct(m.cls, pawn));
			if (!o) return false;
			o->PawnOwner = pawn;
			m.original = pawn->Moves[m.slot];
			pawn->Moves[m.slot] = o;
			m.pawn = pawn;
			m.pawnId = game::IdOf(pawn);
			m.object = o;
			m.active = false;
			return true;
		}

		void Uninstall(Move& m)
		{
			auto* pawn = game::Pawn();
			if (!Installed(m, pawn)) return;
			if (static_cast<int>(pawn->MovementState.GetValue()) == m.slot) LeaveMove(pawn);
			pawn->Moves[m.slot] = m.original;
		}
	}

	int Register(const std::string& owner, const std::string& name, Classes::UClass* cls, Callback callback, std::string* error)
	{
		auto fail = [&](const std::string& why) {
			if (error) *error = why;
			return 0;
		};
		if (!cls) return fail("no move class");
		bool isMove = false;
		for (const Classes::UStruct* c = cls; c && !isMove; c = game::SuperOf(c)) isMove = c == Classes::UTdMove::StaticClass();
		if (!isMove) return fail(cls->GetName() + " isn't a move (a TdMove class)");
		auto* pawn = game::Pawn();
		int slot = 0;
		for (int s : kSlots)
			if (!Find(s) && (!pawn || SlotFree(pawn, s))) {
				slot = s;
				break;
			}
		if (!slot) return fail("every free move slot is taken");
		auto m = std::make_unique<Move>();
		m->owner = owner;
		m->name = name;
		m->cls = cls;
		m->slot = slot;
		m->callback = std::move(callback);
		if (pawn) Install(*m, pawn);
		spdlog::info("[mod {}] move {} in slot {} (as {})", owner, name, slot, cls->GetName());
		g_moves.push_back(std::move(m));
		return slot;
	}

	bool Start(const std::string& owner, int slot, std::string* error)
	{
		auto fail = [&](const char* why) {
			if (error) *error = why;
			return false;
		};
		Move* m = Find(slot);
		if (!m || m->owner != owner) return fail("not a move of this mod");
		auto* pawn = game::Pawn();
		if (!pawn) return fail("no player");
		if (!Install(*m, pawn)) return fail("its slot is taken in this pawn");
		if (static_cast<int>(pawn->MovementState.GetValue()) == slot) return true;
		engine::SetMove(pawn, static_cast<Classes::EMovement>(slot));
		if (static_cast<int>(pawn->MovementState.GetValue()) != slot) return fail("the game didn't change the move");
		m->active = true;
		const Callback cb = m->callback;
		cb(Phase::Start, 0.f);		// May remove the move: nothing of it is used after this
		return true;
	}

	void Remove(int slot)
	{
		const auto it = std::find_if(g_moves.begin(), g_moves.end(), [&](const auto& m) { return m->slot == slot; });
		if (it == g_moves.end()) return;
		Uninstall(**it);
		g_moves.erase(it);
	}

	void RemoveAll(const std::string& owner)
	{
		std::erase_if(g_moves, [&](const auto& m) {
			if (m->owner != owner) return false;
			Uninstall(*m);
			return true;
		});
	}

	std::string NameOf(int slot)
	{
		const Move* m = Find(slot);
		return m ? m->name : std::string();
	}

	TickStats Stats() { return g_stats; }
	void ResetStats() { g_stats = {}; }

	std::string OwnerOf(int slot)
	{
		const Move* m = Find(slot);
		return m ? m->owner : std::string();
	}

	void Tick(float seconds)
	{
		if (g_moves.empty()) return;
		auto* pawn = game::Pawn();
		if (!pawn) return;
		const int state = static_cast<int>(pawn->MovementState.GetValue());
		// By slot, looked up again after each callback: a callback may remove moves
		std::vector<int> slots;
		for (auto& m : g_moves) slots.push_back(m->slot);
		for (int slot : slots) {
			Move* m = Find(slot);
			if (!m) continue;
			if (!Installed(*m, pawn)) {
				m->active = false;		// A new pawn: whatever it was doing in the old one is over
				Install(*m, pawn);
			}
			const Callback cb = m->callback;
			const bool in = state == slot;
			if (in && !m->active) {
				m->active = true;		// Put there by something else than move_start (the game's SetMove)
				cb(Phase::Start, 0.f);
			} else if (in) {
				const auto t0 = clk::Now();
				cb(Phase::Tick, seconds);
				const double us = clk::SecondsSince(t0) * 1e6;
				++g_stats.calls;
				g_stats.totalUs += us;
				g_stats.maxUs = std::max(g_stats.maxUs, us);
			} else if (m->active) {
				m->active = false;
				cb(Phase::Stop, seconds);
			}
		}
	}
}
