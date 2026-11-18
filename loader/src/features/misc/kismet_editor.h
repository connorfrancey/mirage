#pragma once
// The Kismet node editor (Mirage step 27, the editor part of the plan's step 19): one of the loaded levels' sequences
// drawn as a graph (ops as boxes where the level's designers put them, ObjPosX/Y; output links as wires; variables as
// small nodes), panned and zoomed. Edits apply to the running level at once: a wire dragged from an output to an input
// links them, a wire right-clicked unlinks, an op of a class is added at the view. They are kept as a list, and "Save"
// turns them into the chosen mod's Kismet layer (kismet/<map>.toml, appended: runtime/kismet_layers.h) after undoing
// them live, so the layer is what applies them from then on, on every load; "Discard" undoes them.
//
// The window and the self-test call the same functions below. They run on the game thread (the window queues them
// there); the graph the window draws is a copy refreshed from the game thread while it is open.
#include <string>
#include <vector>

#include "runtime/kismet.h"

namespace kismet_editor
{
	// The sequence `path` opened (one of kismet::Sequences()). False if there is none.
	bool Open(const std::string& path);
	// The open sequence as it is now (empty path: none open)
	mirage::kismet::Sequence Graph();

	// The edits, applied live. Ops are named by path; `output` / `input` are indices.
	bool Link(const std::string& from, int output, const std::string& to, int input, std::string* error = nullptr);
	bool Unlink(const std::string& from, int output, const std::string& to, int input, std::string* error = nullptr);
	// A new op of `cls` ("Engine.SeqAct_Log", or a short name found in Engine, GameFramework or TdGame) at (x, y) in the
	// sequence's coordinates; its path, or "" (`error`)
	std::string Add(const std::string& cls, int x, int y, std::string* error = nullptr);
	// The edits so far, as text ("link SeqEvent_Touch_3[0] -> SeqAct_Log_2[0]")
	std::vector<std::string> Edits();
	// Every edit undone in the running level
	void Discard();
	// The edits written to `modId`'s kismet/<map>.toml (added to what is there) and undone live, so the mod's layer
	// applies them (within a second). False and `error` if there is nothing to save or no such mod.
	bool Save(const std::string& modId, std::string* error = nullptr);

	// For the self-test: frames the window was drawn
	unsigned long long Frames();
}
