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
}
