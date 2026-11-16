#pragma once
// Mods' own moves (step 8 of the next ten steps, the plan's step 22).
//
// The experiment (self-test "Mirage custom move (probe)") decided between the plan's three ways: a mod's move lives in
// a slot of the player's move table (TdPawn.Moves, indexed by EMovement) that no player move uses: the obsolete
// backflips (58, 59) and the obsolete RB moves and other None entries of the player's MoveClasses. A move object put
// there is reached by the game's own SetMove like any other: a TdMove_Falling in slot 58 started at the top of a jump
// ran (Jump > 58 > Walking, landing handled by the game), and putting the slot back left the jump exactly as it was.
// A new EMovement value past MOVE_MAX would reach the natives' switches out of range, so it isn't used.
//
// So a move is: a free slot, and a move object of the class it behaves as (a game move: TdMove_Falling for an air move,
// or a mod class extending TdMove from step 12) installed in the player's pawn there, again on every new pawn. The mod
// starts it (move_start: the game's SetMove), and the runtime calls it back as the player enters it, every tick while
// the player is in it, and as the player leaves it (whatever moved the player out: the game's landing, a grab, death).
// Removed (with the mod), the slot gets back what it had, and a player in the move is put in the game's falling or
// walking move. Game thread.

#include <functional>
#include <string>

namespace Classes
{
	class UClass;
}

namespace mirage::moves
{
	enum class Phase { Start = 0, Tick = 1, Stop = 2 };
	using Callback = std::function<void(Phase phase, float seconds)>;

	// A move of `owner` (a mod id) named `name`, behaving as `cls` (a TdMove class). Its slot (an EMovement number), or
	// 0 and `error`.
	int Register(const std::string& owner, const std::string& name, Classes::UClass* cls, Callback callback, std::string* error);
	// The player put into it now (the game's SetMove). False and `error` when it can't (no player, not installed, the
	// game refused the change).
	bool Start(const std::string& owner, int slot, std::string* error);
	// Taken out: the slot as it was; a player in it goes to the game's falling or walking move
	void Remove(int slot);
	void RemoveAll(const std::string& owner);
	// The registered move's name in `slot` ("" if none), and its mod's id
	std::string NameOf(int slot);
	std::string OwnerOf(int slot);
	// Every engine tick: installed in the player's pawn, enter / tick / leave callbacks
	void Tick(float seconds);
}
