// Models (runtime/model_files.h): a mod's model, converted by `mirage model --convert`, built into a package holding a
// StaticMesh; read back with UnrealLib. Game meshes are the reference where the game's packages are found.

#include "detect.h"
#include "runtime/model_files.h"
#include "runtime/texture_files.h"

#include <UnrealLib/UnrealLib.hpp>
#include <UnrealLib/Assets/SkeletalMesh.hpp>
#include <UnrealLib/Assets/StaticMesh.hpp>
#include <UnrealLib/Linker/ReflectionSchema.hpp>
#include <UnrealLib/Writer/PackageWriter.hpp>

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
    auto cube = Cube();
    cube.sections[0].image = std::string("\x89PNG\0 not really", 18);
    cube.sections[0].roughness = "roughness";
    const fs::path file = fs::temp_directory_path() / "mirage_test_cube.mmesh";
    std::string error;
    ASSERT_TRUE(mirage::models::WriteMeshFile(file.string(), cube, &error)) << error;
    mirage::models::MeshData back;
    ASSERT_TRUE(mirage::models::ReadMeshFile(file.string(), back, &error)) << error;
    EXPECT_EQ(back.vertices.size(), 24u);
    EXPECT_EQ(back.indices, cube.indices);
    ASSERT_EQ(back.sections.size(), 1u);
    EXPECT_EQ(back.sections[0].material, "NoSuchPackage.NoSuchMaterial");
    EXPECT_EQ(back.sections[0].image, cube.sections[0].image);
    EXPECT_EQ(back.sections[0].specular, "");
    EXPECT_EQ(back.sections[0].roughness, "roughness");
    EXPECT_EQ(std::memcmp(back.vertices.data(), cube.vertices.data(), 24 * sizeof(mirage::models::MeshVertex)), 0);
    EXPECT_TRUE(back.bones.empty());
    EXPECT_TRUE(back.skin.empty());
    // Cut short: refused
    fs::resize_file(file, fs::file_size(file) - 4);
    EXPECT_FALSE(mirage::models::ReadMeshFile(file.string(), back, &error));
    EXPECT_NE(error.find("short"), std::string::npos) << error;

    // A skinned model keeps its bones and each vertex's influences; a bone past the names is refused
    cube.bones = {"LeftHand", "RightHand"};
    cube.skin.assign(cube.vertices.size(), {});
    cube.skin[3] = {{1, 0, 0, 0}, {0.75f, 0.25f, 0, 0}};
    ASSERT_TRUE(mirage::models::WriteMeshFile(file.string(), cube, &error)) << error;
    ASSERT_TRUE(mirage::models::ReadMeshFile(file.string(), back, &error)) << error;
    EXPECT_EQ(back.bones, cube.bones);
    ASSERT_EQ(back.skin.size(), 24u);
    EXPECT_EQ(back.skin[3].bone[0], 1);
    EXPECT_EQ(back.skin[3].weight[1], 0.25f);
    cube.skin[3].bone[0] = 2;
    ASSERT_TRUE(mirage::models::WriteMeshFile(file.string(), cube, &error)) << error;
    EXPECT_FALSE(mirage::models::ReadMeshFile(file.string(), back, &error));
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
    EXPECT_EQ(m.properties.get_bool("UseSimpleRigidBodyCollision"), std::optional<bool>(true));
    // Its rigid body: a subobject with its corners as one convex element, and no cooked copy
    ASSERT_TRUE(m.body_setup.is_export());
    EXPECT_EQ(p->object_path(m.body_setup), "Cube.RB_BodySetup_0");
    EXPECT_EQ(p->class_name(m.body_setup), "RB_BodySetup");
    ReflectionSchema schema(linker);
    for (const char* name : {"Core", "Engine"})
        if (const Package* s = linker.find_package(name)) schema.add_package(*s);
    const ObjectProperties body = linker.read_properties({p, m.body_setup}, &schema);
    const Property* geometry = find_property(body.properties, "AggGeom");
    ASSERT_NE(geometry, nullptr);
    PropertyFormatOptions fo;
    fo.names = &p->names();
    const std::string text = format_property_value(geometry->value, fo);
    // Its points: the cube's 8 corners, each once
    std::size_t points = 0;
    for (std::size_t at = text.find("(X="); at != std::string::npos; at = text.find("(X=", at + 1)) ++points;
    EXPECT_EQ(points, 8u) << text;
    EXPECT_NE(text.find("(X=-50.0,Y=-50.0,Z=-50.0)"), std::string::npos) << text;
    EXPECT_NE(text.find("(X=50.0,Y=50.0,Z=50.0)"), std::string::npos) << text;
    const auto data = p->export_data(m.body_setup.export_index());
    EXPECT_EQ(data.size() - body.end_offset, 4u);       // PreCachedPhysData: an empty array
    fs::remove_all(dir);
}

// A new mesh's rigid body follows the model's own shape, not its box: a pyramid (the cube's top pulled to one point)
// gets its 5 corners, where a box would have 8 and stand on corners the model doesn't have
TEST(Models, RigidBodyFollowsTheModelsShape)
{
    const fs::path cooked = Cooked();
    if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found";
    const fs::path dir = fs::temp_directory_path() / "mirage_test_models_hull";
    fs::remove_all(dir);
    auto pyramid = Cube();
    for (auto& v : pyramid.vertices)
        if (v.position[2] > 0) v.position[0] = v.position[1] = 0;
    std::string error;
    {
        Linker linker;
        linker.add_search_path(cooked, true);
        ASSERT_TRUE(mirage::models::BuildPackage(linker, pyramid, "Pyramid", dir / "MiragePyramid_1234.upk", &error)) << error;
    }
    Linker linker;
    linker.add_search_path(cooked, true);
    linker.add_search_path(dir, false);
    const Package* p = linker.find_package("MiragePyramid_1234");
    ASSERT_NE(p, nullptr);
    const StaticMesh m = read_static_mesh(linker, {p, *p->find_export("Pyramid")});
    ReflectionSchema schema(linker);
    for (const char* name : {"Core", "Engine"})
        if (const Package* s = linker.find_package(name)) schema.add_package(*s);
    const ObjectProperties body = linker.read_properties({p, m.body_setup}, &schema);
    const Property* geometry = find_property(body.properties, "AggGeom");
    ASSERT_NE(geometry, nullptr);
    PropertyFormatOptions fo;
    fo.names = &p->names();
    const std::string text = format_property_value(geometry->value, fo);
    std::size_t points = 0;
    for (std::size_t at = text.find("(X="); at != std::string::npos; at = text.find("(X=", at + 1)) ++points;
    EXPECT_EQ(points, 5u) << text;
    EXPECT_NE(text.find("(X=0.0,Y=0.0,Z=50.0)"), std::string::npos) << text;
    EXPECT_EQ(text.find("(X=50.0,Y=50.0,Z=50.0)"), std::string::npos) << text;     // No box corner
    fs::remove_all(dir);
}

// A section the model gives its own texture, normal map and glTF roughness map (and no game material): the package holds
// them, DXT1 with every mip inline, and a MaterialInstanceConstant of the game's lit character material showing them
TEST(Models, TexturedSectionsGetTheirOwnMaterial)
{
    const fs::path cooked = Cooked();
    if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found";
    const fs::path dir = fs::temp_directory_path() / "mirage_test_models_textured";
    fs::remove_all(dir);
    const fs::path out = dir / "MirageCrate_1234.upk";
    auto cube = Cube();
    // An 8x4 uncompressed TGA (stb reads it like a PNG): the left half red, the right half green
    std::string tga(18, '\0');
    tga[2] = 2;
    tga[12] = 8;
    tga[14] = 4;
    tga[16] = 32;
    tga[17] = 0x28;     // Top-left origin, 8 alpha bits
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) tga += x < 4 ? std::string("\x00\x00\xFF\xFF", 4) : std::string("\x00\xFF\x00\xFF", 4);
    cube.sections[0].image = tga;
    // A normal map in glTF's convention, 4x4: every pixel (R 128, G 200, B 255), its green up
    std::string normal(18, '\0');
    normal[2] = 2;
    normal[12] = 4;
    normal[14] = 4;
    normal[16] = 32;
    normal[17] = 0x28;
    for (int i = 0; i < 16; ++i) normal += std::string("\xFF\xC8\x80\xFF", 4);     // B G R A
    cube.sections[0].normal = normal;
    // glTF's metallic-roughness image, the same pixels: roughness (green) 200, so a specular map of 255 - 200
    cube.sections[0].roughness = normal;
    std::string error;
    {
        Linker linker;
        linker.add_search_path(cooked, true);
        ASSERT_TRUE(mirage::models::BuildPackage(linker, cube, "Cube", out, &error)) << error;
    }
    Linker linker;
    linker.add_search_path(cooked, true);
    linker.add_search_path(dir, false);
    const Package* p = linker.find_package("MirageCrate_1234");
    ASSERT_NE(p, nullptr);
    const StaticMesh m = read_static_mesh(linker, {p, *p->find_export("Cube")});
    ASSERT_EQ(m.lods.at(0).sections.size(), 1u);
    const ObjectIndex material = m.lods[0].sections[0].material;
    ASSERT_TRUE(material.is_export());
    EXPECT_EQ(p->object_path(material), "Cube_Material_0");
    EXPECT_EQ(p->class_name(material), "MaterialInstanceConstant");
    ReflectionSchema schema(linker);
    for (const char* name : {"Core", "Engine"})
        if (const Package* s = linker.find_package(name)) schema.add_package(*s);
    const MaterialInfo info = read_material(linker, {p, material}, &schema);
    EXPECT_EQ(p->object_path(info.parent), mirage::models::kTexturedParent);
    ASSERT_EQ(info.parameters.size(), 3u);
    EXPECT_EQ(info.parameters[0].name, mirage::models::kTexturedParameter);
    ASSERT_TRUE(info.parameters[0].texture.is_export());
    EXPECT_EQ(p->object_path(info.parameters[0].texture), "Cube_Texture_0");
    EXPECT_EQ(p->export_data(material.export_index()).size(), linker.read_properties({p, material}).end_offset);  // No native data
    const Texture2D tex = read_texture2d(linker, {p, info.parameters[0].texture}, &schema);
    EXPECT_EQ(tex.size_x, 8);
    EXPECT_EQ(tex.size_y, 4);
    EXPECT_EQ(tex.format, PixelFormat::DXT1);
    EXPECT_EQ(tex.properties.get_bool("NeverStream"), std::optional<bool>(true));
    ASSERT_EQ(tex.mips.size(), 4u);     // 8x4 down to 1x1
    const auto top = load_mip(*p, tex, 0);
    const Image rgba = decode_pixels(top, tex.format, 8, 4);
    EXPECT_GT(rgba.rgba[0 * 4 + 0], 200);     // Left: red
    EXPECT_LT(rgba.rgba[0 * 4 + 1], 50);
    EXPECT_GT(rgba.rgba[7 * 4 + 1], 200);     // Right: green
    EXPECT_LT(rgba.rgba[7 * 4 + 0], 50);
    // The last mip's bulk data offset is where its bytes are: the engine reads it from there
    EXPECT_EQ(load_mip(*p, tex, 3).size(), 8u);
    // The normal map: stored as the game's are, its green turned to the game's convention (down)
    EXPECT_EQ(info.parameters[1].name, mirage::models::kNormalParameter);
    ASSERT_TRUE(info.parameters[1].texture.is_export());
    EXPECT_EQ(p->object_path(info.parameters[1].texture), "Cube_Normal_0");
    const Texture2D n = read_texture2d(linker, {p, info.parameters[1].texture}, &schema);
    EXPECT_EQ(n.format, PixelFormat::DXT1);
    EXPECT_EQ(n.srgb, false);
    EXPECT_EQ(n.compression_settings, "TC_Normalmap");
    EXPECT_EQ(n.lod_group, "TEXTUREGROUP_WorldNormalMap");
    EXPECT_EQ(n.properties.get_float("UnpackMin", 2), std::optional<float>(-1.f));
    const Image np = decode_pixels(load_mip(*p, n, 0), n.format, 4, 4);
    EXPECT_NEAR(np.rgba[0], 128, 8);
    EXPECT_NEAR(np.rgba[1], 55, 8);     // 255 - 200
    EXPECT_NEAR(np.rgba[2], 255, 8);
    // The roughness map made a grey specular map, 1 - roughness
    EXPECT_EQ(info.parameters[2].name, mirage::models::kSpecularParameter);
    ASSERT_TRUE(info.parameters[2].texture.is_export());
    EXPECT_EQ(p->object_path(info.parameters[2].texture), "Cube_Specular_0");
    const Texture2D sp = read_texture2d(linker, {p, info.parameters[2].texture}, &schema);
    const Image spp = decode_pixels(load_mip(*p, sp, 0), sp.format, 4, 4);
    for (int k = 0; k < 3; ++k) EXPECT_NEAR(spp.rgba[k], 55, 8);
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
    // Its rigid body, the same subobject, has the model's shape: the cube's 8 corners, no cooked copy of the bag's
    const auto body = [&](const fs::path& file) {
        Linker linker;
        linker.add_search_path(cooked, true);
        const Package& p = linker.load(file);
        ReflectionSchema schema(linker);
        for (const char* name : {"Core", "Engine"})
            if (const Package* s = linker.find_package(name)) schema.add_package(*s);
        const ObjectIndex index = read_static_mesh(linker, {&p, *p.find_export("P_Waste.GarbageBag_01.S_GarbageBag_01")}).body_setup;
        const ObjectProperties props = linker.read_properties({&p, index}, &schema);
        const Property* geometry = find_property(props.properties, "AggGeom");
        PropertyFormatOptions fo;
        fo.names = &p.names();
        return std::pair{geometry ? format_property_value(geometry->value, fo) : std::string(),
                         p.export_data(index.export_index()).size() - props.end_offset};
    };
    const auto [shape, native] = body(copy);
    std::size_t points = 0;
    for (std::size_t at = shape.find("(X="); at != std::string::npos; at = shape.find("(X=", at + 1)) ++points;
    EXPECT_EQ(points, 8u) << shape;
    EXPECT_NE(shape.find("(X=50.0,Y=50.0,Z=50.0)"), std::string::npos) << shape;
    EXPECT_EQ(native, 4u);      // PreCachedPhysData: an empty array
    EXPECT_NE(body(map).first, shape);

    // A stale conversion: refused with the reason
    std::ofstream(model, std::ios::app) << "# edited after converting\n";
    const auto stale = mirage::textures::BuildOverlay(map, {{target, model}});
    ASSERT_EQ(stale.problems.size(), 1u);
    EXPECT_NE(stale.problems[0].find("changed since it was converted"), std::string::npos) << stale.problems[0];
    fs::remove_all(dir);
}


// A skeletal mesh replaced: Faith's first-person arms (the game's layout, which models::MakeSkeletalMesh follows: one
// LOD, a chunk a section with CPU copies of its vertices, bForceCPUSkinning) become a cube skinned to her hands, half to
// each and the four corners of one face shared between them, keeping her skeleton, materials and properties
TEST(Models, ReplacesASkeletalMesh)
{
    const fs::path cooked = Cooked();
    if (cooked.empty()) GTEST_SKIP() << "Mirror's Edge not found";
    const fs::path package = cooked / "Characters" / "CH_TKY_Crim_Fixer_1P.upk";
    const std::string target = "ch_tky_crim_fixer_1p.sk_upperbody";
    const auto read = [&](const fs::path& file) {
        Linker linker;
        linker.add_search_path(cooked, true);
        const Package& p = linker.load(file);
        const ObjectRef ref{&p, *p.find_export("SK_UpperBody")};
        SkeletalMesh m = read_skeletal_mesh(linker, ref);
        // What is read writes back as stored
        const std::size_t native = open_asset(linker, ref).native_offset;
        const auto data = p.export_data(ref.index.export_index());
        PackageModel names = PackageModel::from_package(p);
        const auto written = write_skeletal_mesh(m, p.context(), p.profile().assets, [&](std::string_view t) { return names.make_name(t); },
                                                 static_cast<std::uint64_t>(p.exports()[ref.index.export_index()].serial_offset) + native);
        EXPECT_TRUE(std::ranges::equal(written, data.subspan(native))) << file.filename();
        return m;
    };
    const SkeletalMesh before = read(package);
    // The game's arms, as the builder follows them
    EXPECT_EQ(before.skeleton.size(), 74u);
    ASSERT_EQ(before.lods.size(), 1u);
    EXPECT_EQ(before.lods[0].chunks.size(), 2u);
    EXPECT_EQ(before.lods[0].chunks[0].stored_rigid_vertices, static_cast<std::size_t>(before.lods[0].chunks[0].rigid_vertices));
    EXPECT_EQ(before.lods[0].shadow_indices.size(), before.lods[0].indices.size());
    EXPECT_EQ(before.properties.get_bool("bForceCPUSkinning"), std::optional<bool>(true));

    const fs::path dir = fs::temp_directory_path() / "mirage_test_skeletal_replacement";
    fs::create_directories(dir);
    const fs::path model = dir / "arms.glb";
    std::ofstream(model) << "# the arms' source\n";
    auto cube = Cube();
    cube.sourceHash = mirage::models::HashFile(model.string());
    cube.bones = {"lefthand", "RightHand"};		// Any case
    cube.skin.assign(cube.vertices.size(), {});
    for (std::size_t v = 0; v < cube.vertices.size(); ++v)
    {
        const float x = cube.vertices[v].position[0];
        if (v < 4) cube.skin[v] = {{0, 1, 0, 0}, {0.5f, 0.5f, 0, 0}};		// The +X face: both hands
        else cube.skin[v] = {{static_cast<std::uint16_t>(x < 0 ? 0 : 1), 0, 0, 0}, {2.f, 0, 0, 0}};		// Weights normalized
    }
    std::string error;
    ASSERT_TRUE(mirage::models::WriteMeshFile(model.string() + ".mmesh", cube, &error)) << error;

    const auto overlay = mirage::textures::BuildOverlay(package, {{target, model}});
    ASSERT_TRUE(overlay.problems.empty()) << overlay.problems.front();
    ASSERT_EQ(overlay.replaced, std::vector<std::string>{target});
    const fs::path copy = dir / package.filename();
    std::ofstream(copy, std::ios::binary).write(reinterpret_cast<const char*>(overlay.bytes.data()),
                                                static_cast<std::streamsize>(overlay.bytes.size()));
    const SkeletalMesh after = read(copy);
    EXPECT_EQ(after.skeleton.size(), 74u);
    EXPECT_EQ(after.name_index_map, before.name_index_map);
    EXPECT_EQ(after.materials.size(), before.materials.size());
    EXPECT_EQ(after.properties.get_bool("bForceCPUSkinning"), std::optional<bool>(true));
    EXPECT_NEAR(after.bounds.sphere_radius, 86.6f, 0.1f);		// The cube's corner: 50 * sqrt(3)
    ASSERT_EQ(after.lods.size(), 1u);
    const SkeletalLod& lod = after.lods[0];
    EXPECT_EQ(lod.vertices.size(), 24u);
    EXPECT_EQ(lod.indices.size(), 36u);
    EXPECT_EQ(lod.shadow_indices.size(), 36u);
    EXPECT_EQ(lod.shadow_triangle_double_sided, std::vector<std::uint8_t>(12, 1));
    EXPECT_EQ(lod.num_edges, 30);		// A face's own 4 vertices: its 4 sides open, its diagonal shared, 6 faces
    EXPECT_EQ(lod.required_bones.size(), 74u);
    ASSERT_EQ(lod.sections.size(), 1u);
    EXPECT_EQ(lod.sections[0].material_index, before.lods[0].sections[0].material_index);
    ASSERT_EQ(lod.chunks.size(), 1u);
    const SkeletalChunk& chunk = lod.chunks[0];
    EXPECT_EQ(chunk.bone_map, (std::vector<std::int16_t>{19, 48}));		// LeftHand, RightHand: as the vertices use them
    EXPECT_EQ(chunk.rigid_vertices, 20);
    EXPECT_EQ(chunk.soft_vertices, 4);
    EXPECT_EQ(chunk.max_influences, 2);
    EXPECT_EQ(chunk.stored_rigid_bytes.size(), 20u * (12 + 12 + 3 * 8 + 1));
    EXPECT_EQ(chunk.stored_soft_bytes.size(), 4u * (12 + 12 + 3 * 8 + 8));
    // Rigid vertices first, each on its hand with all the weight; the shared face's on both hands, half each
    for (std::size_t v = 0; v < 24; ++v)
    {
        const SkinnedVertex& sv = lod.vertices[v];
        EXPECT_EQ(sv.weights[0] + sv.weights[1] + sv.weights[2] + sv.weights[3], 255) << v;
        if (v < 20) EXPECT_EQ(sv.bones[0], sv.position.x < 0 ? 19 : 48) << v;
        else EXPECT_TRUE(sv.weights[0] >= 127 && sv.weights[1] >= 127) << v;
    }

    // A bone her skeleton doesn't have: not replaced, and said why
    cube.bones[1] = "Tail";
    ASSERT_TRUE(mirage::models::WriteMeshFile(model.string() + ".mmesh", cube, &error)) << error;
    const auto refused = mirage::textures::BuildOverlay(package, {{target, model}});
    ASSERT_EQ(refused.problems.size(), 1u);
    EXPECT_NE(refused.problems[0].find("Tail"), std::string::npos) << refused.problems[0];
    fs::remove_all(dir);
}
