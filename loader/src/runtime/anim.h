#pragma once
// Animation from mods (step 9 of the next ten steps, the plan's step 23): the game's own playback API on its pawns and
// what their meshes can play.
//
// The survey (the decompiled TdGame script): a TdPawn plays a sequence on one of its custom animation slots
// (CustomNodeType: Canned, CannedUpperBody, FullBody, FullBody_Dir, UpperBody, LowerBody, Camera, Weapon, Face) with
// the native PlayCustomAnim(Type, AnimName, Rate, BlendInTime, BlendOutTime, bLooping, bOverride, bRootMotion,
// bRootRotation), stops it with StopCustomAnim(Type, BlendOutTime), and GetCustomAnimation(Type) gives the slot's
// AnimNodeSequence (its sequence, time, rate, playing). It is what every TdMove's PlayMoveAnim calls, on the
// first-person (Mesh1p) and third-person (Mesh3p) meshes together. A sequence is found by name in the mesh's AnimSets
// (the pawn's own, and the weapon's that UpdateAnimSets puts in slots 1 and 2). Calls go through mirage::call (the
// engine's 4-byte UBOOLs). Game thread.

#include <string>

namespace Classes
{
	class AActor;
}

namespace mirage::anim
{
	struct State
	{
		bool valid = false;			// The slot has a sequence node
		bool playing = false, looping = false;
		std::string sequence;		// The sequence's name ("" none)
		float position = 0.f, length = 0.f, rate = 0.f;
	};

	// Plays `sequence` on `slot` (a CustomNodeType). False and `error` when it can't: not a TdPawn, the call failed, or
	// the mesh has no sequence of that name (the slot then doesn't play it).
	bool Play(Classes::AActor* pawn, int slot, const std::string& sequence, float rate, float blendIn, float blendOut,
			  bool loop, bool rootMotion, std::string* error);
	bool Stop(Classes::AActor* pawn, int slot, float blendOut);
	State Get(Classes::AActor* pawn, int slot);
	// The animation sets the actor's skeletal meshes have, as JSON: [{"mesh": "Mesh1p", "sets": [{"name", "path",
	// "sequences": [{"name", "length", "frames", "rate", "notifies": [{"time", "notify", "comment"}]}]}]}]
	std::string SetsJson(Classes::AActor* actor);
	// CustomNodeType by name ("FullBody", "fullbody", "CNT_FullBody"): -1 if none
	int SlotFromName(const std::string& name);
	// How many sequences the AnimSet `path` ("Package.Name") has in its package file, read with UnrealLib (seconds:
	// never on the game or render thread). -1 and `error` if it can't be read.
	int ReadSetSequences(const std::string& path, std::string* error);
}
