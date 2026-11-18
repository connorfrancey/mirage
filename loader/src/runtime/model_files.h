#pragma once
// Models from mods (Mirage, step 31): a converted model (runtime/mesh_file.h) built into a package holding one
// StaticMesh, which the engine then loads like any cooked mesh (runtime/models.h). Engine-free: UnrealLib only, so the
// unit tests build packages too.
//
// The mesh is laid out the way the game's own are (read from them, tests/test_models.cpp): one LOD whose vertex
// streams hold every vertex twice (the second half the shadow volume's extruded copies, the shadow stream 0 then 1),
// the edges and a double-sided flag per triangle for shadow volumes, one fragment per section, InternalVersion 16, and
// a kDOP collision tree over all its triangles (UnrealLib's build_kdop), so the player and traces collide with the
// triangles themselves (UseSimpleLineCollision and UseSimpleBoxCollision off; there is no RB_BodySetup, so no rigid
// body). Each section's material is a game material named by the file's material name ("Package.Name"), else
// `defaultMaterial`.

#include "runtime/mesh_file.h"

#include <filesystem>
#include <string>

namespace UnrealLib
{
	class Linker;
}

namespace mirage::models
{
	// The game material every section without one of the game's gets
	inline constexpr const char* kDefaultMaterial = "EngineMaterials.DefaultMaterial";

	// `mesh` as package `packageName` (the file's stem) holding the StaticMesh `meshName`, written to `out`. `linker`
	// has the game's packages (the summary to copy, the materials). False and `error` if it can't be built (more than
	// 65,535 vertices: the game's index buffers are 16-bit).
	bool BuildPackage(UnrealLib::Linker& linker, const MeshData& mesh, const std::string& meshName, const std::filesystem::path& out,
					  std::string* error = nullptr, const std::string& defaultMaterial = kDefaultMaterial);
}
