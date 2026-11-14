#pragma once
#include <cstdint>
#include <string>

// The Level Editor's window (features/misc/level_editor.cpp), for the self-test
namespace level_editor
{
	std::uint64_t Frames();					// Frames the window was drawn
	std::uint64_t GizmoFrames();			// Frames the gizmo was drawn on a selected actor
	void Select(const std::string& mod, const std::string& name);	// Edit `mod`'s layer, select entry `name`
	// Places an actor of `cls` where the camera looks (as the window's button does) in the selected mod's layer;
	// its entry's name. Render thread or any.
	std::string PlaceWhereLooking(const std::string& cls);
}
