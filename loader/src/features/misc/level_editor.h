#pragma once
// The in-game Level Editor (Mirage step 27, the level editor's first feature): a mod's layer actors for the current
// level (runtime/layers.h, levels/<map>.toml) in a working copy. Moving, turning or scaling an entry moves its spawned
// actor now (layers::Place); adding, deleting, renaming and changing a class show after Save, which writes the file and
// spawns the layer again on the next tick.
//
// The window and the self-test call the same functions below, on the game thread only (layers are game-thread state).
// Actors() and the rest of the getters copy under a lock, so the window reads them from the render thread.
#include "render/math.h"
#include "runtime/layers.h"

#include <string>
#include <vector>

namespace level_editor
{
	// `modId`'s layer for the current level read into the working copy (empty if it has no file yet). False and `error`
	// if it doesn't parse.
	bool Open(const std::string& modId, std::string* error = nullptr);
	// An entry of class `cls` ("Engine.DynamicSMActor_Spawnable", "classes/Pad.uc") 400 units in front of the camera,
	// named Actor<n>. Its index.
	int Add(const std::string& cls);
	// Entry `i` changed: its spawned actor follows now when only where it is changed
	void Set(int i, const mirage::layers::EditActor& a);
	void Remove(int i);
	// The working copy written as the mod's layer. False and `error` if it can't be.
	bool Save(std::string* error = nullptr);
	// The spawned actor of entry `i` (nullptr: not spawned, or changed since the last save). Game thread.
	Classes::AActor* Live(int i);

	// ---- Selection, picking and the gizmo (the level editor's second feature) ----------------------------------------
	// The game's camera projected onto a w x h view: x, y in pixels; false behind the camera. Any thread.
	bool Project(const rmath::Camera& camera, const float world[3], float w, float h, float& x, float& y);
	// The entry whose spawned actor is nearest the ray from the camera through (x, y) of a w x h view, within 150 units
	// of it, selected and returned (-1: none, the selection cleared). Game thread.
	int Pick(float x, float y, float w, float h);
	// The selected entry's coordinate on world axis `axis` (0 X, 1 Y, 2 Z) set to `value`, snapped to `grid` units when
	// grid > 0; its spawned actor follows. Game thread.
	void Drag(int axis, float value, float grid = 0.f);

	// Any thread
	int Selected();
	void Select(int i);
	std::string Mod();
	std::vector<mirage::layers::EditActor> Actors();
	bool Dirty();
}
