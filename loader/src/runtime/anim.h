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

#include <array>
#include <map>
#include <string>
#include <vector>

namespace Classes
{
	class AActor;
	class UAnimSequence;
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
	// "sequences": [{"name", "length", "frames", "rate", "custom", "notifies": [{"time", "notify", "comment"}]}]}]}]
	// (custom: a mod's animation, see Install)
	std::string SetsJson(Classes::AActor* actor);
	// CustomNodeType by name ("FullBody", "fullbody", "CNT_FullBody"): -1 if none
	int SlotFromName(const std::string& name);
	// How many sequences the AnimSet `path` ("Package.Name") has in its package file, read with UnrealLib (seconds:
	// never on the game or render thread). -1 and `error` if it can't be read.
	int ReadSetSequences(const std::string& path, std::string* error);

	// ---- Custom animations (anim_clip.cpp) --------------------------------------------------------------------------
	// A clip is an animation as raw keys: what a mod's content/animations/<Name>.json holds, what the Animation Editor
	// edits and what Bake gives. `frames` keys spread evenly over `length` (key i at i * length / (frames - 1)); each bone
	// has its local position and rotation (x, y, z, w) either once (held) or once a frame. Bones go by name, per mesh: the
	// first-person arms (Mesh1p) and the third-person body (Mesh3p) are different skeletons.
	//
	// The engine plays raw keys itself: a sequence whose compressed stream pointers are null is posed from its
	// RawAnimData (found by a probe: cooked sequences keep only the compressed stream). So a clip becomes an ordinary
	// sequence of the mesh's AnimSet, played by name like the game's own (Play, mod moves, the Animation Browser).
	struct BoneKeys
	{
		std::vector<std::array<float, 3>> pos;
		std::vector<std::array<float, 4>> rot;
	};
	struct Clip
	{
		std::string name;
		float length = 0.f, rate = 1.f;
		int frames = 0;
		std::map<std::string, std::map<std::string, BoneKeys>> meshes;		// "Mesh1p" / "Mesh3p" -> bone -> keys
	};
	// The JSON form: {"name", "length", "rate", "frames", "meshes": {"Mesh1p": {"LeftHand": {"pos": [[x, y, z]...],
	// "rot": [[x, y, z, w]...]}}}}. False and `error` for a clip that isn't well formed (key counts, numbers).
	bool ClipFromJson(const std::string& text, Clip* out, std::string* error);
	std::string ClipToJson(const Clip& clip);
	// `sequence` posed on each of the pawn's meshes `frames` times over its length (the full-body slot held at each
	// time, the bones read back from the evaluated pose), as a clip of the same name. The slot is stopped after. Game
	// thread.
	bool Bake(Classes::AActor* pawn, const std::string& sequence, int frames, Clip* out, std::string* error);
	// The clip as a sequence of the pawn's meshes, in each mesh's first AnimSet (tracks in the set's bone order; a bone
	// the clip doesn't have holds where it is in the mesh's pose now). A clip installed before under that name is
	// replaced; a game sequence of that name is left alone (false). Game thread.
	bool Install(Classes::AActor* pawn, const Clip& clip, std::string* error);
	// A mod's clips, installed on the player now and again whenever the player's pawn or its animation sets change
	// (Tick); Unregister takes them off the sets. Game thread.
	void Register(const std::string& mod, std::vector<Clip> clips);
	void Unregister(const std::string& mod);
	void Tick();
	// The registered clips: mod and name
	std::vector<std::pair<std::string, std::string>> Registered();
	// A registered clip by name (the editor's copy to change), false if there is none
	bool RegisteredClip(const std::string& name, Clip* out);
	// A sequence Install made (not the game's). Game thread.
	bool IsCustom(const Classes::UAnimSequence* sequence);
}
