#pragma once
// Models from mods in the game (Mirage, step 31): a mod's glTF, FBX or OBJ file, converted by the CLI (`mirage model
// <file> --convert` writes <file>.mmesh, runtime/mesh_file.h), built into a package holding a StaticMesh on a worker
// thread (runtime/model_files.h) and loaded by the engine like a cooked mesh (runtime/classes.h).
//
// The package goes to <mirage>\models\, outside the mod's folder (the hot reload watcher would see it), named after the
// file and a hash of the converted model: "Suzanne_1a2b3c4d.upk", its mesh named after the file ("Suzanne"). The
// engine keeps a loaded package for the session: a changed model is a new package (hot reload), the same one loads the
// mesh it already has, and a package already built isn't built again.
//
// A level layer places one with an actor entry's `mesh` (runtime/layers.h):
//
//     [[actor]]
//     name = "Monkey"
//     class = "Engine.DynamicSMActor_Spawnable"
//     mesh = "models/suzanne.glb"           # or "<mod id>:models/x.glb", another mod's
//     location = [0.0, 0.0, 0.0]

#include <filesystem>
#include <string>

#include "runtime/texture_files.h"

#include <vector>

namespace Classes
{
	class UStaticMesh;
}

namespace mirage::models
{
	// The mesh of model file `file` (its .mmesh next to it), built on a worker the first time: nullptr with `pending`
	// true until it is (ask again on a later tick). nullptr and `why` if there's no converted model, it was converted
	// from another version of the file (its source hash), or it doesn't build or load. Game thread.
	Classes::UStaticMesh* MeshOf(const std::filesystem::path& file, std::string* why, bool* pending);

	// Skeletal meshes replaced in memory. A content mod's replacement of a skeletal mesh that is already loaded (Faith's
	// first-person arms, CH_TKY_Crim_Fixer_1P.SK_UpperBody, come with the game's scripts, before Mirage) can't go
	// through the overlay (runtime/texture_overlays.h): its package isn't opened again. Instead the package is rewritten
	// the same way (textures::BuildOverlay) on a worker, under a name of its own
	// ("CH_TKY_Crim_Fixer_1P_1a2b3c4d.upk" in <mirage>\models\), loaded beside the original, and every SkeletalMeshComponent drawing the
	// original draws the copy's mesh: a live one through SetSkeletalMesh, a template (a class's default subobject, which
	// new pawns copy) by its property. Turned off, they draw the original again. Game thread.
	void ActivateSkeletal(const std::string& owner, const std::vector<textures::Replacement>& replacements);
	void DeactivateSkeletal(const std::string& owner);
	// Copies built since the last tick are loaded and swapped in. Game thread, every tick.
	void TickSkeletal();
	// A copy is still being built (for the self-test)
	bool SkeletalPending();
}
