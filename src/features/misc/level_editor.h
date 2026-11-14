#pragma once
#include "runtime/layers.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The Level Editor (features/misc/level_editor.cpp): what its window does, also for the self-test. The actions work on
// the selected entry of the selected mod's layer, run on the game thread and save the layer (undo keeps the step).
namespace level_editor
{
	std::uint64_t Frames();					// Frames the window was drawn
	std::uint64_t GizmoFrames();			// Frames the gizmo was drawn on a selected actor
	void Select(const std::string& mod, const std::string& name);	// Edit `mod`'s layer, select entry `name`
	// Places an actor of `cls` where the camera looks (as the window's button does) in the selected mod's layer;
	// its entry's name. Render thread or any.
	std::string PlaceWhereLooking(const std::string& cls);

	// Any thread
	void Undo();
	void Redo();
	void Duplicate();			// A copy beside it, selected
	void DeleteSelected();
	void DropToFloor();			// Down onto the world under it (its collision's bottom on the floor)
	void BringHere();			// To where the camera looks
	void GoTo();				// The player to it
	void SetSnap(float grid, float angleDegrees, float scaleStep);	// 0: off
	void SnapSelected();		// Its location, rotation and scale onto the snapping steps
	std::vector<mirage::layers::EditActor> Entries();	// The layer as the editor has it
	int Selected();
	std::size_t UndoDepth();

	// Game thread
	std::vector<std::string> PlaceableClasses();		// Every loaded placeable, non-abstract actor class ("Engine.Trigger")
	struct Property
	{
		std::string name, value, kind;	// value: as the layer writes it ("1500.0", "true", "\"Name\"", "[1.0, 2.0, 3.0]")
	};
	// The editable (var()) properties of the selected mod's spawned entry `name`, with their values now
	std::vector<Property> PropertiesOf(const std::string& name);
}
