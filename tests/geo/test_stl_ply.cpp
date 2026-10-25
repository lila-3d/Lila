#include "TestUtil.h"

#include "geo/ColorSets.h"
#include "geo/PlyIO.h"
#include "geo/Primitives.h"
#include "geo/StlIO.h"

#include <cstring>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
std::vector<StlSolid> cube()
{
    std::vector<StlSolid> s(1);
    s[0].name = "box";
    s[0].mesh = makeCube({.width = 10.0, .height = 20.0, .depth = 30.0});
    return s;
}

Index hardCount(const Mesh& m)
{
    std::span<const bool> hard = m.edgeAttrs().get<bool>(attr::kHard);
    Index n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        n += m.edgeValid(e) && !hard.empty() && hard[e];
    return n;
}
} // namespace

TEST_CASE("stl: binary round trip welds a closed mesh")
{
    const std::string data = writeStl(cube());
    CHECK(data.size() == 84 + 12 * 50);
    CHECK(data.compare(0, 5, "solid") != 0);
    StlReadResult r = readStl(data);
    REQUIRE(r.ok());
    CHECK(r.binary);
    REQUIRE(r.solids.size() == 1);
    const Mesh& m = r.solids[0].mesh;
    CHECK(m.numPoints() == 8);
    CHECK(m.numFaces() == 12);
    CHECK(boundaryEdgeCount(m) == 0);
    CHECK(isValid(m));
    CHECK(signedVolume(m) == doctest::Approx(6000.0));
    CHECK(hardCount(m) == 12); // the cube's edges, not the triangle diagonals
}

TEST_CASE("stl: ascii round trip keeps solids apart")
{
    std::vector<StlSolid> solids = cube();
    solids.push_back({"second", makeCube({.width = 1.0, .height = 1.0, .depth = 1.0})});
    const std::string text = writeStl(solids, {.binary = false});
    CHECK(text.starts_with("solid box\n"));
    StlReadResult r = readStl(text);
    REQUIRE(r.ok());
    CHECK(!r.binary);
    REQUIRE(r.solids.size() == 2);
    CHECK(r.solids[0].name == "box");
    CHECK(r.solids[1].name == "second");
    CHECK(r.solids[1].mesh.numPoints() == 8);
    CHECK(signedVolume(r.solids[1].mesh) == doctest::Approx(1.0));
}

TEST_CASE("stl: binary whose header starts with solid still reads as binary")
{
    std::string data = writeStl(cube());
    std::memcpy(data.data(), "solid box", 9);
    StlReadResult r = readStl(data);
    REQUIRE(r.ok());
    CHECK(r.binary);
    CHECK(r.solids[0].mesh.numFaces() == 12);
}

TEST_CASE("stl: no weld leaves separate triangles")
{
    StlReadResult r = readStl(writeStl(cube()), {.weld = false});
    REQUIRE(r.ok());
    CHECK(r.solids[0].mesh.numPoints() == 36);
}

TEST_CASE("stl: per-triangle colors round trip")
{
    std::vector<StlSolid> s = cube();
    colors::add(s[0].mesh, "colorSet", glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    StlReadResult r = readStl(writeStl(s, {.colors = true}));
    REQUIRE(r.ok());
    CHECK(r.colors);
    std::span<const glm::vec4> c = colors::values(r.solids[0].mesh, "colorSet");
    REQUIRE(!c.empty());
    CHECK(c[0].r == doctest::Approx(1.0f));
    CHECK(c[0].g == doctest::Approx(0.0f));
}

TEST_CASE("stl: garbage and truncation fail cleanly")
{
    CHECK(!readStl("hello").ok());
    std::string data = writeStl(cube());
    data.resize(200);
    CHECK(!readStl(data).ok());
}

TEST_CASE("ply: binary round trip keeps polygons, uvs, normals and colors")
{
    Mesh m = makeCube({.width = 10.0, .height = 10.0, .depth = 10.0});
    colors::add(m, "colorSet", glm::vec4(0.25f, 0.5f, 1.0f, 1.0f));
    for (bool binary : {true, false})
    {
        CAPTURE(binary);
        PlyReadResult r = readPly(writePly(m, {.binary = binary}));
        REQUIRE(r.ok());
        CHECK(!r.pointCloud);
        CHECK(r.mesh.numFaces() == 6);
        CHECK(r.mesh.faceSize(0) == 4);
        CHECK(r.mesh.cornerAttrs().has(attr::kNormal));
        CHECK(r.mesh.cornerAttrs().has(attr::uvName(0)));
        std::span<const glm::vec4> c = colors::values(r.mesh, "colorSet");
        REQUIRE(!c.empty());
        CHECK(c[0].b == doctest::Approx(1.0f));
        CHECK(c[0].g == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(signedVolume(r.mesh) == doctest::Approx(1000.0));
    }
}

TEST_CASE("ply: without split attributes the mesh stays welded")
{
    Mesh m = makeCube({.width = 10.0, .height = 10.0, .depth = 10.0});
    PlyReadResult r = readPly(writePly(m, {.normals = false, .uvs = false, .colors = false}));
    REQUIRE(r.ok());
    CHECK(r.mesh.numPoints() == 8);
    CHECK(boundaryEdgeCount(r.mesh) == 0);
    CHECK(isValid(r.mesh));
}

TEST_CASE("ply: ascii point cloud with extra properties")
{
    const char* text = "ply\r\nformat ascii 1.0\r\ncomment made by hand\r\nelement vertex 3\r\nproperty float x\r\nproperty float y\r\n"
                       "property float z\r\nproperty uchar red\r\nproperty uchar green\r\nproperty uchar blue\r\nproperty float quality\r\n"
                       "end_header\r\n0 0 0 255 0 0 0.5\r\n1 0 0 0 255 0 1\r\n0 1 0 0 0 255 2\r\n";
    PlyReadResult r = readPly(text);
    REQUIRE(r.ok());
    CHECK(r.pointCloud);
    CHECK(r.mesh.numPoints() == 3);
    std::span<const glm::vec4> c = r.mesh.pointAttrs().get<glm::vec4>(attr::kColor);
    REQUIRE(c.size() == 3);
    CHECK(c[1].g == doctest::Approx(1.0f));
    std::span<const float> q = r.mesh.pointAttrs().get<float>("quality");
    REQUIRE(q.size() == 3);
    CHECK(q[2] == doctest::Approx(2.0f));

    PlyReadResult back = readPly(writePly(r.mesh));
    REQUIRE(back.ok());
    CHECK(back.pointCloud);
    CHECK(back.mesh.numPoints() == 3);
    CHECK(back.mesh.pointAttrs().get<glm::vec4>(attr::kColor)[1].g == doctest::Approx(1.0f));
}

TEST_CASE("ply: big endian, int indices, texcoord lists, face colors and skipped elements")
{
    auto be32 = [](std::string& s, uint32_t v) {
        for (int i = 3; i >= 0; --i)
            s += char((v >> (i * 8)) & 0xff);
    };
    auto bef = [&](std::string& s, float f) {
        uint32_t v;
        std::memcpy(&v, &f, 4);
        be32(s, v);
    };
    std::string s = "ply\nformat binary_big_endian 1.0\ncomment TextureFile tex.png\nelement vertex 4\nproperty float x\nproperty float y\n"
                    "property float z\nelement face 2\nproperty list uchar uint vertex_indices\nproperty list uchar float texcoord\n"
                    "property uchar red\nproperty uchar green\nproperty uchar blue\nelement material 1\nproperty int id\nend_header\n";
    const float pts[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    for (auto& p : pts)
        for (float v : p)
            bef(s, v);
    const uint32_t faces[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int f = 0; f < 2; ++f)
    {
        s += char(3);
        for (uint32_t i : faces[f])
            be32(s, i);
        s += char(6);
        for (uint32_t i : faces[f])
        {
            bef(s, pts[i][0]);
            bef(s, pts[i][1]);
        }
        s += char(f ? 0 : 255);
        s += char(0);
        s += char(f ? 255 : 0);
    }
    be32(s, 7);
    PlyReadResult r = readPly(s);
    REQUIRE(r.ok());
    CHECK(r.mesh.numFaces() == 2);
    CHECK(r.mesh.numPoints() == 4);
    REQUIRE(r.textureFiles.size() == 1);
    CHECK(r.textureFiles[0] == "tex.png");
    CHECK(std::ranges::find(r.skipped, "element material") != r.skipped.end());
    std::span<const glm::vec2> uv = r.mesh.cornerAttrs().get<glm::vec2>(attr::uvName(0));
    REQUIRE(!uv.empty());
    for (Index c = 0; c < r.mesh.cornerCapacity(); ++c)
    {
        const glm::dvec3& p = r.mesh.position(r.mesh.cornerPoint(c));
        CHECK(uv[c].x == doctest::Approx(p.x));
        CHECK(uv[c].y == doctest::Approx(p.y));
    }
    std::span<const glm::vec4> col = colors::values(r.mesh, "colorSet");
    REQUIRE(!col.empty());
    CHECK(col[r.mesh.faceCorner(0)].r == doctest::Approx(1.0f));
    CHECK(col[r.mesh.faceCorner(1)].b == doctest::Approx(1.0f));
}

TEST_CASE("ply: bad files fail cleanly")
{
    CHECK(!readPly("not a ply").ok());
    CHECK(!readPly("ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n").ok());
    CHECK(!readPly("ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nend_header\n0\n").ok());
}
