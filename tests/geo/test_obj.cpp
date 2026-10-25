#include "TestUtil.h"

#include "geo/Normals.h"
#include "geo/ObjIO.h"
#include "geo/Primitives.h"

#include <chrono>

using namespace lila::geo;
using namespace lila::geo::test;

namespace
{
Index hardCount(const Mesh& m)
{
    std::span<const bool> hard = m.edgeAttrs().get<bool>(attr::kHard);
    Index n = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        n += m.edgeValid(e) && !hard.empty() && hard[e];
    return n;
}
} // namespace

TEST_CASE("obj: parse basics")
{
    const char* text = R"(# test
mtllib test.mtl
o first
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 2 0 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
g top
usemtl red
s 1
f 1/1/1 2/2/1 3/3/1 4/4/1
usemtl blue
f -4/1/1 -1/2/1 -3/3/1
o second
v 5 5 5
v 6 5 5
v 6 6 5
f 6 7 8
f 6 7
)";
    ObjReadResult r = readObj(text);
    REQUIRE(r.ok());
    REQUIRE(r.objects.size() == 2);
    CHECK(r.mtllibs.size() == 1);
    const Mesh& a = r.objects[0].mesh;
    CHECK(r.objects[0].name == "first");
    CHECK(a.numFaces() == 2);
    CHECK(a.numPoints() == 5);
    CHECK(isValid(a));
    REQUIRE(a.materialNames().size() == 2);
    CHECK(a.materialNames()[1] == "blue");
    std::span<const int32_t> mat = a.faceAttrs().get<int32_t>(attr::kMaterial);
    CHECK(mat[0] == 0);
    CHECK(mat[1] == 1);
    CHECK(a.faceAttrs().has("group"));
    CHECK(r.objects[0].groupNames.at(0) == "top");
    std::span<const glm::vec2> uv = a.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(uv.size() == 7);
    CHECK(uv[2] == glm::vec2(1, 1));
    CHECK(a.faceSize(1) == 3);

    const Mesh& b = r.objects[1].mesh;
    CHECK(r.objects[1].name == "second");
    CHECK(b.numFaces() == 1);
    CHECK(b.numPoints() == 3);
    CHECK(b.position(0).x == 5.0);
}

TEST_CASE("obj: round trip keeps topology, uvs, hard edges and materials")
{
    Mesh cyl = makeCylinder({.radius = 50.0, .height = 100.0, .subdivAxis = 12, .subdivHeight = 2, .capFill = CapFill::Ngon});
    std::span<int32_t> mat = cyl.faceAttrs().getOrAdd<int32_t>(attr::kMaterial);
    cyl.materialNames() = {"side", "cap"};
    for (Index f = 0; f < cyl.faceCapacity(); ++f)
        mat[f] = cyl.faceSize(f) > 4 ? 1 : 0;

    const std::string text = writeObj(cyl);
    ObjReadResult r = readObj(text);
    REQUIRE(r.objects.size() == 1);
    const Mesh& m = r.objects[0].mesh;
    CHECK(isValid(m));
    CHECK(m.numPoints() == cyl.numPoints());
    CHECK(m.numFaces() == cyl.numFaces());
    CHECK(m.numEdges() == cyl.numEdges());
    CHECK(hardCount(m) == hardCount(cyl));
    CHECK(m.materialNames().size() == 2);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        CHECK(m.position(p) == cyl.position(p));
    std::span<const glm::vec2> uvA = std::as_const(cyl).cornerAttrs().get<glm::vec2>("uv0");
    std::span<const glm::vec2> uvB = m.cornerAttrs().get<glm::vec2>("uv0");
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        CHECK(uvA[c] == uvB[c]);
    std::span<const int32_t> matB = m.faceAttrs().get<int32_t>(attr::kMaterial);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        CHECK(m.materialNames()[matB[f]] == cyl.materialNames()[mat[f]]);

    // The text itself is stable.
    CHECK(writeObj(m) == text);
}

TEST_CASE("obj: hard edges from normals without smoothing groups")
{
    Mesh cube = makeCube();
    ObjWriteOptions wo;
    wo.smoothingGroups = false;
    ObjReadResult r = readObj(writeObj(cube, wo));
    REQUIRE(r.objects.size() == 1);
    CHECK(hardCount(r.objects[0].mesh) == 12);
}

TEST_CASE("perf: obj 1M faces")
{
    Mesh m = makePlane({1000.0, 1000.0, 1000, 1000});
    auto t0 = std::chrono::steady_clock::now();
    const std::string text = writeObj(m);
    const double tWrite = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    t0 = std::chrono::steady_clock::now();
    ObjReadResult r = readObj(text);
    const double tRead = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.objects.size() == 1);
    CHECK(r.objects[0].mesh.numFaces() == 1000000);
    MESSAGE("obj 1M quads (" << text.size() / (1 << 20) << " MB): write " << tWrite << "s, read " << tRead << "s");
}
