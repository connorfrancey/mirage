// Models (runtime/model_files.h): a mod's model, converted by `mirage model --convert`, built into a package holding a
// StaticMesh; read back with UnrealLib. Game meshes are the reference where the game's packages are found.

#include "detect.h"
#include "runtime/model_files.h"
#include "runtime/texture_files.h"

#include <UnrealLib/UnrealLib.hpp>
#include <UnrealLib/Assets/StaticMesh.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
using namespace UnrealLib;
using namespace UnrealLib::Assets;

namespace
{
    fs::path Cooked()
    {
        if (const char* dir = std::getenv("UNREALLIB_MIRRORSEDGE_DIR"); dir && *dir) return dir;
        for (const auto& install : detect::FindInstalls())
        {
            const fs::path cooked = install.exe.parent_path().parent_path() / "TdGame" / "CookedPC";
            if (fs::exists(cooked / "Core.u")) return cooked;
        }
        return {};
    }
}

namespace
{
    // A cube of 100 units: 24 vertices (4 a face, each with its own normal), 12 triangles, clockwise seen from outside
    mirage::models::MeshData Cube()
    {
        mirage::models::MeshData m;
        const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (const auto& f : n)
        {
            // Two axes across the face
            float u[3] = {f[1] != 0 || f[2] != 0 ? 1.f : 0.f, f[0] != 0 ? 1.f : 0.f, 0};
            const float w[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
            const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
            for (const auto& c : {std::pair{-1.f, -1.f}, std::pair{1.f, -1.f}, std::pair{1.f, 1.f}, std::pair{-1.f, 1.f}})
            {
                mirage::models::MeshVertex v;
                for (int k = 0; k < 3; ++k)
                {
                    v.position[k] = 50.f * (f[k] + c.first * u[k] + c.second * w[k]);
                    v.normal[k] = f[k];
                    v.tangent[k] = u[k];
                }
                v.tangent[3] = 1;
                v.uv[0] = (c.first + 1) / 2;
                v.uv[1] = (c.second + 1) / 2;
                m.vertices.push_back(v);
            }
            for (const std::uint32_t i : {0u, 2u, 1u, 0u, 3u, 2u}) m.indices.push_back(base + i);
        }
        m.sections.push_back({0, 12, "NoSuchPackage.NoSuchMaterial"});
        return m;
    }
}

TEST(Models, MeshFileRoundTrip)
{
    const auto cube = Cube();
    const fs::path file = fs::temp_directory_path() / "mirage_test_cube.mmesh";
    std::string error;
    ASSERT_TRUE(mirage::models::WriteMeshFile(file.string(), cube, &error)) << error;
    mirage::models::MeshData back;
    ASSERT_TRUE(mirage::models::ReadMeshFile(file.string(), back, &error)) << error;
    EXPECT_EQ(back.vertices.size(), 24u);
    EXPECT_EQ(back.indices, cube.indices);
    ASSERT_EQ(back.sections.size(), 1u);
    EXPECT_EQ(back.sections[0].material, "NoSuchPackage.NoSuchMaterial");
    EXPECT_EQ(std::memcmp(back.vertices.data(), cube.vertices.data(), 24 * sizeof(mirage::models::MeshVertex)), 0);
    // Cut short: refused
    fs::resize_file(file, fs::file_size(file) - 4);
    EXPECT_FALSE(mirage::models::ReadMeshFile(file.string(), back, &error));
    EXPECT_NE(error.find("size"), std::string::npos) << error;
    fs::remove(file);
}

TEST(Models, BuildsAPackageTheGameCouldLoad)
{
    const fs::path cooked = Cooked();
    if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found";
    const fs::path dir = fs::temp_directory_path() / "mirage_test_models";
    fs::remove_all(dir);
    const fs::path out = dir / "MirageCube_1234.upk";
    std::string error;
    {
        Linker linker;
        linker.add_search_path(cooked, true);
        ASSERT_TRUE(mirage::models::BuildPackage(linker, Cube(), "Cube", out, &error)) << error;
    }
    Linker linker;
    linker.add_search_path(cooked, true);
    linker.add_search_path(dir, false);
    const Package* p = linker.find_package("MirageCube_1234");
    ASSERT_NE(p, nullptr);
    const auto e = p->find_export("Cube");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(p->class_name(*e), "StaticMesh");
    const StaticMesh m = read_static_mesh(linker, {p, *e});
    ASSERT_EQ(m.lods.size(), 1u);
    const StaticMeshLod& l = m.lods[0];
    EXPECT_EQ(l.num_vertices, 24);
    EXPECT_EQ(l.positions.size(), 48u);        // With the shadow volume's copies
    EXPECT_EQ(l.indices.size(), 36u);
    EXPECT_EQ(l.edges.size() / 16, 30u);       // Split vertices: each face its own 4 sides and diagonal
    EXPECT_NEAR(m.bounds.box_extent.x, 50.f, 1e-3f);
    EXPECT_NEAR(m.bounds.origin.z, 0.f, 1e-3f);
    // The unknown material fell back to the default, a game material the engine resolves
    ASSERT_EQ(l.sections.size(), 1u);
    EXPECT_EQ(p->object_path(l.sections[0].material), "EngineMaterials.DefaultMaterial");
    // Collision: a kDOP tree over all 12 triangles, and the flags that make the engine use it
    EXPECT_EQ(m.kdop_triangles.size(), 12u);
    const auto nodes = decode_kdop_nodes(m);
    ASSERT_FALSE(nodes.empty());
    EXPECT_NEAR(nodes[0].min.x, -50.f, 1e-3f);
    EXPECT_NEAR(nodes[0].max.z, 50.f, 1e-3f);
    std::set<std::uint16_t> covered;
    for (const auto& n : nodes)
        if (n.leaf)
            for (std::uint16_t t = n.second; t < n.second + n.first; ++t) EXPECT_TRUE(covered.insert(t).second);
    EXPECT_EQ(covered.size(), 12u);
    EXPECT_EQ(m.properties.get_bool("UseSimpleLineCollision"), std::optional<bool>(false));
    EXPECT_EQ(m.properties.get_bool("UseSimpleBoxCollision"), std::optional<bool>(false));
    fs::remove_all(dir);
}

// A game mesh replaced (runtime/texture_files.h's BuildOverlay, step 31): the Prologue's garbage bag in its art map
// becomes the cube, keeping the original's material and RB_BodySetup, with its own collision tree and the simple
// collision switched off
TEST(Models, ReplacesAMeshInAMapPackage)
{
    const fs::path cooked = Cooked();
    if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found";
    const fs::path map = cooked / "Maps" / "SP01" / "Edge_Pt1_Art.me1";
    const std::string target = "p_waste.garbagebag_01.s_garbagebag_01";

    // The model: any file, its converted cube beside it with the file's hash
    const fs::path dir = fs::temp_directory_path() / "mirage_test_mesh_replacement";
    fs::create_directories(dir);
    const fs::path model = dir / "cube.obj";
    std::ofstream(model) << "# the cube's source\n";
    auto cube = Cube();
    cube.sourceHash = mirage::models::HashFile(model.string());
    std::string error;
    ASSERT_TRUE(mirage::models::WriteMeshFile(model.string() + ".mmesh", cube, &error)) << error;

    const auto overlay = mirage::textures::BuildOverlay(map, {{target, model}});
    ASSERT_TRUE(overlay.problems.empty()) << overlay.problems.front();
    ASSERT_EQ(overlay.replaced, std::vector<std::string>{target});
    const fs::path copy = dir / map.filename();
    std::ofstream(copy, std::ios::binary).write(reinterpret_cast<const char*>(overlay.bytes.data()),
                                                static_cast<std::streamsize>(overlay.bytes.size()));

    const auto read = [&](const fs::path& file) {
        Linker linker;
        linker.add_search_path(cooked, true);
        const Package& p = linker.load(file);
        const auto e = p.find_export("P_Waste.GarbageBag_01.S_GarbageBag_01");
        EXPECT_TRUE(e.has_value());
        StaticMesh m = read_static_mesh(linker, {&p, *e});
        const std::string material = p.object_path(m.lods[0].sections[0].material);
        return std::pair{std::move(m), material};
    };
    const auto [before, beforeMaterial] = read(map);
    const auto [after, afterMaterial] = read(copy);
    EXPECT_NEAR(before.bounds.sphere_radius, 39.6f, 0.1f);    // The bag's, as the self-test expects it
    EXPECT_NEAR(after.bounds.sphere_radius, 86.6f, 0.1f);     // The cube's corner: 50 * sqrt(3)
    EXPECT_EQ(after.lods[0].num_vertices, 24);
    EXPECT_EQ(after.kdop_triangles.size(), 12u);
    EXPECT_EQ(afterMaterial, beforeMaterial);
    EXPECT_EQ(after.body_setup.value, before.body_setup.value);
    EXPECT_EQ(after.properties.get_bool("UseSimpleLineCollision"), std::optional<bool>(false));
    EXPECT_EQ(after.properties.get_bool("UseSimpleBoxCollision"), std::optional<bool>(false));

    // A stale conversion: refused with the reason
    std::ofstream(model, std::ios::app) << "# edited after converting\n";
    const auto stale = mirage::textures::BuildOverlay(map, {{target, model}});
    ASSERT_EQ(stale.problems.size(), 1u);
    EXPECT_NE(stale.problems[0].find("changed since it was converted"), std::string::npos) << stale.problems[0];
    fs::remove_all(dir);
}
