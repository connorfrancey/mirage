#pragma once
// The in-game editors' core (Mirage, step 16): one selection shared by every editor window, an object's properties read
// as rows for an inspector, typed writes that are undoable, and the actors in play for an explorer.
//
// The selection is a handle (step 14): it is never a dangling pointer, and reads as nothing once the object is gone.
// Writes go through a transaction of the change log (step 15) under the id "editor", so Undo puts the old value back
// exactly. Reading and writing game objects happens on the game thread; the windows draw the rows they were given.
//
// Docking: the menu's ImGui is the release branch, without docking. The editors are ordinary windows (their places are
// kept by ImGui's own layout file); docking would mean the docking branch for the whole menu (left for later).

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Classes
{
	class UObject;
}

namespace editor
{
	// ---- The selection (any thread to set; read on the game thread) -------------------------------------------------
	void Select(Classes::UObject* object);			// nullptr: none
	Classes::UObject* Selected();					// Game thread: the selected object while it is still that object
	std::uint64_t SelectionSerial();				// Changes with every Select (windows refresh on it)
	std::string SelectedLabel();					// "TdPlayerPawn_0 (TdPlayerPawn)", any thread

	// ---- An object's properties as rows (game thread) ---------------------------------------------------------------
	struct Row
	{
		std::string path;			// "GroundSpeed", "Location.X", "Moves[3]"
		std::string name;			// The last part ("X", "[3]")
		std::string kind;			// "float", "int", "byte", "bool", "name", "string", "object", "struct", "array", ...
		std::string value;			// As text
		int depth = 0;
		bool expandable = false;	// A struct or an array (its members / elements come when `expanded` has its path)
		bool editable = false;		// float, int, byte, bool, name
		bool editorOnly = false;	// var(): the class lets an editor change it
		Classes::UObject* ref = nullptr;	// Object kinds: what it points to (to select it)
	};
	// Every property of `object` (sorted by name; top-level names filtered by `filter`, case-insensitive), with the
	// members of the structs and the elements of the arrays whose paths are in `expanded` (up to 64 elements)
	std::vector<Row> Rows(Classes::UObject* object, const std::string& filter, const std::set<std::string>& expanded);

	// ---- Writes and undo (game thread) -------------------------------------------------------------------------------
	// Sets `path` of `object` from text (a number, true/false, a name), in an undoable step. False and `error` if not.
	bool SetValue(Classes::UObject* object, const std::string& path, const std::string& text, std::string* error = nullptr);
	bool Undo();									// The last write put back; false if there is none
	std::size_t UndoDepth();						// Any thread

	// ---- The world (game thread) ------------------------------------------------------------------------------------
	struct ActorRow
	{
		std::string name, cls;
		float distance = 0.f;		// From the player (0 if none)
		Classes::UObject* actor = nullptr;
	};
	std::vector<ActorRow> Actors();					// Every actor in play (step 14's lists), nearest first
	Classes::UObject* LookedAt();					// The actor under the crosshair (a trace from the camera), or nullptr

	// ---- The windows, for the self-test ------------------------------------------------------------------------------
	enum class Window { Explorer, Inspector };
	void NoteFrame(Window w);
	std::uint64_t Frames(Window w);
}
