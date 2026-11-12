#pragma once
#include <string>
#include <vector>

// Save states: the Save State (one, the Save State / Load State keys) and named save states kept per level in this mod's
// savestates\<level>.json, so they survive restarting the game. A state is the runtime's player state
// (MirageApi::player_state_capture / _apply). Saving or loading a named one also makes it the Save State, so the Load
// State key keeps going back to it.
//
// The list follows the level the player is in: it is read from that level's file when the level changes. Thread safe;
// the ones that touch the player run on the game thread.
namespace save_library
{
	// The Save State. Game thread.
	bool SaveCurrent(std::string* error = nullptr);
	bool LoadCurrent(std::string* error = nullptr);
	bool HasCurrent();
	void ClearCurrent();

	struct Entry
	{
		std::string name;
		std::string created;    // "2026-11-04 14:03"
		float x = 0.f, y = 0.f, z = 0.f;
		float speed = 0.f;      // Horizontal, units per second
	};

	// The states of `map` (empty: the level the list last saw). Reads its file the first time.
	std::vector<Entry> List(const std::string& map = {});
	std::string Map();          // The level the list is for, lower case; empty outside a level

	int Selected();             // -1: none
	void Select(int index);
	void SelectNext(int step);  // Wraps around

	// Game thread. False with the reason in `error`.
	bool SaveNew(std::string* error = nullptr);         // Saves the player as a new state and selects it
	bool Overwrite(int index, std::string* error = nullptr);
	bool Load(int index, std::string* error = nullptr);

	void Rename(int index, const std::string& name);
	void Remove(int index);
	void MoveBy(int index, int step);   // Up (-1) or down (+1) in the list

	// The folder in the mod's folder the files go to ("savestates"; the self-test points it elsewhere)
	void SetFolder(const std::string& folder);
	std::string Folder();
}
