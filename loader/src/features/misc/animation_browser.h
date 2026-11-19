#pragma once
// The Animation Browser: every sequence the player's meshes can play (runtime/anim SetsJson: Faith's AnimSets on the
// first- and third-person meshes, the weapon's when she holds one), filtered by name, played with a click on a custom
// animation slot through the game's PlayCustomAnim (runtime/anim Play), with the slot's progress shown.
//
// The window and the self-test call the same functions below, on the game thread (they touch the pawn).
#include <string>
#include <vector>

namespace animation_browser
{
	// The player's sequences read again: their names, each once (a sequence both meshes have is one row). Empty without
	// a player.
	std::vector<std::string> Refresh();
	// `sequence` played on the window's slot, rate and loop setting. False and `error` when it can't be.
	bool Play(const std::string& sequence, std::string* error);
	void Stop();
}
