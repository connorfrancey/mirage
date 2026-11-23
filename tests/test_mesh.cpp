#include "render/mesh.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace rmesh;

TEST(Mesh, PackColorPacksArgbAndClamps)
{
    EXPECT_EQ(PackColor(1.f, 0.f, 0.f), 0xFFFF0000u);
    EXPECT_EQ(PackColor(0.f, 1.f, 0.f, 0.f), 0x0000FF00u);
    EXPECT_EQ(PackColor(2.f, -1.f, 0.f, 1.f), 0xFFFF0000u);
}

TEST(Mesh, BoxIs12TrianglesAnd12OutlineEdges)
{
    MeshData data;
    Builder b(data);
    Style style;
    style.fill = true;
    style.outline = true;
    b.SetStyle(style);
    b.Box(rmath::Identity(), {10, 20, 30});

    EXPECT_EQ(b.Shapes(), 1u);
    EXPECT_EQ(data.tris.size(), 12u * 3);
    EXPECT_EQ(data.lines.size(), 12u * 2);    // Only the cube's edges: the diagonals of its faces are not creases
    for (const Vertex& v : data.tris) {
        EXPECT_TRUE(v.x == 10.f || v.x == -10.f) << "x = " << v.x;
        EXPECT_TRUE(v.y == 20.f || v.y == -20.f) << "y = " << v.y;
        EXPECT_TRUE(v.z == 30.f || v.z == -30.f) << "z = " << v.z;
    }
}

TEST(Mesh, WireframeHasNoFacesFillOnlyHasNoLines)
{
    MeshData wire, solid;
    Builder bw(wire), bs(solid);
    Style s;
    s.fill = false;
    bw.SetStyle(s);
    bw.Sphere(rmath::Identity(), 50.f);
    s.fill = true;
    s.outline = false;
    bs.SetStyle(s);
    bs.Sphere(rmath::Identity(), 50.f);

    EXPECT_TRUE(wire.tris.empty());
    EXPECT_FALSE(wire.lines.empty());
    EXPECT_FALSE(solid.tris.empty());
    EXPECT_TRUE(solid.lines.empty());
}

TEST(Mesh, OutOfRangeIndicesAreSkippedAndCounted)
{
    MeshData data;
    Builder b(data);
    b.Mesh({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2, 0, 1, 7});
    EXPECT_EQ(data.tris.size(), 3u);
    EXPECT_EQ(b.GetCounters().invalidTriangles, 1u);
}

TEST(Mesh, LinesKeepTheirColor)
{
    MeshData data;
    Builder b(data);
    b.Line({0, 0, 0}, {1, 2, 3}, 0xFF123456u);
    ASSERT_EQ(data.lines.size(), 2u);
    EXPECT_FLOAT_EQ(data.lines[1].x, 1.f);
    EXPECT_EQ(data.lines[0].color, 0xFF123456u);
}

TEST(Mesh, TubeFollowsItsPoints)
{
    rmesh::MeshData out;
    rmesh::Builder b(out);
    b.SetStyle({0xFF808080u, true, false, 0});
    b.Tube({{0.f, 0.f, 100.f}, {500.f, 0.f, 50.f}, {1000.f, 0.f, 100.f}}, 4.f, 6);
    ASSERT_EQ(out.tris.size(), 2u * 6u * 6u);       // Two triangles per side between each pair of points
    EXPECT_TRUE(out.lines.empty());
    float lowest = 1e9f, minX = 1e9f, maxX = -1e9f;
    for (const auto& v : out.tris) {
        lowest = std::min(lowest, v.z);
        minX = std::min(minX, v.x);
        maxX = std::max(maxX, v.x);
        EXPECT_EQ(v.color >> 24, 0xFFu);            // Alpha kept by the shading
    }
    EXPECT_NEAR(lowest, 50.f - 4.f, 1.f);           // The lowest point, minus about the radius
    EXPECT_NEAR(minX, 0.f, 4.5f);                   // From the first point to the last
    EXPECT_NEAR(maxX, 1000.f, 4.5f);
    b.Tube({{0.f, 0.f, 0.f}}, 4.f);                 // One point: nothing
    EXPECT_EQ(out.tris.size(), 2u * 6u * 6u);
}
