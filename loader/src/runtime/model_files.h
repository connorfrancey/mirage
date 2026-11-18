#pragma once
// Models from mods (Mirage, step 31): a converted model (runtime/mesh_file.h) built into a package holding one
// StaticMesh, which the engine then loads like any cooked mesh (runtime/models.h). Engine-free: UnrealLib only, so the
// unit tests build packages too.
//
// The mesh is laid out the way the game's own are (read from them, tests/test_models.cpp): one LOD whose vertex
// streams hold every vertex twice (the second half the shadow volume's extruded copies, the shadow stream 0 then 1),
// the edges and a double-sided flag per triangle for shadow volumes, one fragment per section, InternalVersion 16, and
// a kDOP collision tree over all its triangles (UnrealLib's build_kdop), so the player and traces collide with the
// triangles themselves (UseSimpleLineCollision and UseSimpleBoxCollision off). A new mesh also gets an RB_BodySetup
// for rigid bodies (physics props): a convex box around it, cooked by PhysX when a body is made. Each section's
// material is a game material named by the file's material name ("Package.Name"), else `defaultMaterial`.

#include "runtime/mesh_file.h"

#include <UnrealLib/Assets/StaticMesh.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace UnrealLib
{
	class Linker;
}

namespace mirage::models
{
	// The game material every section without one of the game's gets
	inline constexpr const char* kDefaultMaterial = "EngineMaterials.DefaultMaterial";

	// The ObjectIndex (in the package being written) of section `index`'s material; `name` is the file's material name
	using MaterialFor = std::function<UnrealLib::ObjectIndex(std::size_t index, const std::string& name)>;

	// `mesh` laid out the way the game's meshes are (above), its collision tree built; `material` gives each section's.
	// The properties, BodySetup and the rest are left at their defaults for the caller (a new package, or a replaced
	// mesh keeping the original's). Throws std::runtime_error for no triangles or more than 32,767 vertices.
	UnrealLib::Assets::StaticMesh MakeStaticMesh(const MeshData& mesh, const std::string& name, const MaterialFor& material);

	// `mesh` as package `packageName` (the file's stem) holding the StaticMesh `meshName`, written to `out`. `linker`
	// has the game's packages (the summary to copy, the materials). False and `error` if it can't be built (more than
	// 32,767 vertices: the game's 16-bit index buffers also hold the shadow copies).
	bool BuildPackage(UnrealLib::Linker& linker, const MeshData& mesh, const std::string& meshName, const std::filesystem::path& out,
					  std::string* error = nullptr, const std::string& defaultMaterial = kDefaultMaterial);
}
