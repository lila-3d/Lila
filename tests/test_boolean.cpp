#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "geo/Bvh.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "scene/Graph.h"
#include "scene/MeshBoolean.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstdlib>
#include <fstream>
#include <map>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
std::map<geo::Index, int> sideCounts(const geo::Mesh& m)
{
    std::map<geo::Index, int> sizes;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            ++sizes[m.faceSize(f)];
    return sizes;
}

std::string describe(const std::map<geo::Index, int>& sizes)
{
    std::string s;
    for (auto [n, c] : sizes)
        s += std::to_string(n) + ":" + std::to_string(c) + " ";
    return s;
}

geo::Index largestFace(const geo::Mesh& m) { return sideCounts(m).rbegin()->first; }

/** N-gons that don't lie flat: those would shade badly, flat ones are fine. */
int bentNgons(const geo::Mesh& m)
{
    int bent = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceSize(f) <= 4)
            continue;
        glm::dvec3 n = m.faceNormal(f), c = m.faceCentroid(f);
        for (geo::Index p : m.facePoints(f))
            if (std::abs(glm::dot(m.position(p) - c, n)) > 1e-3)
            {
                ++bent;
                break;
            }
    }
    return bent;
}

/** Smallest corner angle in degrees over the faces around the seam's new points: slivers show up here. */
double thinnestCorner(const geo::Mesh& m)
{
    double worst = 180.0;
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
    {
        if (!m.cornerValid(c))
            continue;
        glm::dvec3 p = m.position(m.cornerPoint(c));
        glm::dvec3 a = m.position(m.cornerPoint(m.cornerPrev(c))) - p, b = m.position(m.cornerNextPoint(c)) - p;
        worst = std::min(worst, glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0))));
    }
    return worst;
}

scene::BooleanResult cut(const geo::Mesh& base, const geo::Mesh& cutter, const glm::dmat4& cutterXf, scene::BooleanOptions opts = {})
{
    scene::BooleanOperand ops[] = {{&base, glm::dmat4(1.0)}, {&cutter, cutterXf}};
    return scene::meshBoolean(ops, glm::dmat4(1.0), opts);
}
} // namespace

TEST_CASE("boolean seam cleanup leaves only flat n-gons along the cut")
{
    geo::Mesh plate = geo::makeCube({.width = 100, .height = 100, .depth = 100});
    geo::Mesh drill = geo::makeCylinder({.radius = 20, .height = 300});

    scene::BooleanResult raw = cut(plate, drill, glm::dmat4(1.0), {.cleanup = false, .splitNgons = false});
    REQUIRE(raw.ok());
    MESSAGE("raw ", describe(sideCounts(raw.mesh)));
    CHECK(largestFace(raw.mesh) > 4);

    scene::BooleanResult r = cut(plate, drill, glm::dmat4(1.0));
    REQUIRE(r.ok());
    MESSAGE("clean ", describe(sideCounts(r.mesh)));
    CHECK(r.mesh.validate(geo::kValidateManifold).empty());
    CHECK(r.seamEdges > 0);
    CHECK(bentNgons(r.mesh) == 0);
    // Each cap is cut by spokes from its corners into four flat pieces, not fanned into slivers.
    CHECK(sideCounts(r.mesh)[3] == 0);
    CHECK(largestFace(r.mesh) < largestFace(raw.mesh) / 2);
}

TEST_CASE("boolean on a subdivided block keeps its untouched quads and splits the rest")
{
    geo::Mesh block = geo::makeCube({.width = 100, .height = 100, .depth = 100, .subdivX = 4, .subdivY = 4, .subdivZ = 4});
    geo::Mesh drill = geo::makeCylinder({.radius = 20, .height = 300});
    glm::dmat4 xf = glm::translate(glm::dmat4(1.0), glm::dvec3(7, 0, 3));

    scene::BooleanResult r = cut(block, drill, xf);
    REQUIRE(r.ok());
    auto sizes = sideCounts(r.mesh);
    MESSAGE("offset hole ", describe(sizes));
    CHECK(r.mesh.validate(geo::kValidateManifold).empty());
    CHECK(bentNgons(r.mesh) == 0);
    CHECK(thinnestCorner(r.mesh) > 9.0);
    // The block's side faces never meet the drill and come back as their original quads.
    CHECK(sizes[4] >= 4 * 16);
    CHECK(sizes[3] < sizes[4]);
}

TEST_CASE("boolean keeps the base's hard edges and marks the seam")
{
    geo::Mesh block = geo::makeCube({.width = 100, .height = 100, .depth = 100});
    auto hard = block.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    std::fill(hard.begin(), hard.end(), true);
    geo::Mesh ball = geo::makeSphere({.radius = 40});
    glm::dmat4 xf = glm::translate(glm::dmat4(1.0), glm::dvec3(50, 50, 0));

    scene::BooleanResult r = cut(block, ball, xf);
    REQUIRE(r.ok());
    MESSAGE("corner sphere ", describe(sideCounts(r.mesh)));
    CHECK(r.mesh.validate(geo::kValidateManifold).empty());
    CHECK(bentNgons(r.mesh) == 0);
    auto seam = r.mesh.edgeAttrs().get<bool>(scene::kBooleanSeamAttr);
    REQUIRE(!seam.empty());
    CHECK(std::count(seam.begin(), seam.end(), true) == r.seamEdges);
    // The cube's far corner edges are untouched and stay hard.
    auto outHard = r.mesh.edgeAttrs().get<bool>(geo::attr::kHard);
    int hardFar = 0;
    for (geo::Index e = 0; e < r.mesh.edgeCapacity(); ++e)
    {
        glm::dvec3 a = r.mesh.position(r.mesh.edgePoint(e, 0)), b = r.mesh.position(r.mesh.edgePoint(e, 1));
        if (a.x < -49 && b.x < -49 && outHard[e])
            ++hardFar;
    }
    CHECK(hardFar == 4);
}

TEST_CASE("boolean fillet rounds the seam into a closed strip")
{
    geo::Mesh plate = geo::makeCube({.width = 100, .height = 100, .depth = 100});
    geo::Mesh drill = geo::makeCylinder({.radius = 20, .height = 300});
    scene::BooleanResult sharp = cut(plate, drill, glm::dmat4(1.0));
    scene::BooleanResult round = cut(plate, drill, glm::dmat4(1.0), {.filletWidth = 3.0, .filletSegments = 3});
    REQUIRE(round.ok());
    MESSAGE("fillet ", describe(sideCounts(round.mesh)));
    CHECK(round.mesh.validate(geo::kValidateManifold).empty());
    CHECK(round.mesh.numFaces() > sharp.mesh.numFaces() + round.seamEdges);
}

TEST_CASE("polyBoolean command cleans up and adds weighted normals")
{
    run("newScene");
    std::string box = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Block"}, {"generator", false}})[0];
    std::string drill = run("polyCylinder", {{"name", "Drill"}, {"generator", false}, {"radius", 20}, {"height", 300}})[0];
    run("polyBoolean", {{"args", {box, drill}}});
    const geo::Mesh& m = meshOf(box);
    CHECK(m.validate(geo::kValidateManifold).empty());
    CHECK(bentNgons(m) == 0);
    json stack = run("modifiers", {{"args", {box}}});
    CHECK(stack.dump().find("WeightedNormals") != std::string::npos);
    run("undo");
    CHECK(run("objExists", {{"args", {drill}}}).get<bool>());
}

TEST_CASE("zz boolean dump" * doctest::skip())
{
    // Writes the cases as OBJ for eyeballing: lila_tests -tc="zz boolean dump" -ns
    const char* dir = std::getenv("LILA_BOOLEAN_DUMP");
    if (!dir)
        return;
    auto save = [&](const std::string& name, const scene::BooleanResult& r) {
        std::ofstream(std::string(dir) + "/" + name + ".obj") << geo::writeObj(r.mesh);
    };
    geo::Mesh cube = geo::makeCube({.width = 100, .height = 100, .depth = 100});
    geo::Mesh block = geo::makeCube({.width = 100, .height = 100, .depth = 100, .subdivX = 4, .subdivY = 4, .subdivZ = 4});
    geo::Mesh drill = geo::makeCylinder({.radius = 20, .height = 300});
    geo::Mesh ball = geo::makeSphere({.radius = 40});
    save("plate_raw", cut(cube, drill, glm::dmat4(1.0), {.cleanup = false, .splitNgons = false}));
    save("plate", cut(cube, drill, glm::dmat4(1.0)));
    save("plate_fillet", cut(cube, drill, glm::dmat4(1.0), {.filletWidth = 3.0, .filletSegments = 3}));
    save("block", cut(block, drill, glm::translate(glm::dmat4(1.0), glm::dvec3(7, 0, 3))));
    save("corner", cut(cube, ball, glm::translate(glm::dmat4(1.0), glm::dvec3(50, 50, 0))));
    save("block_corner", cut(block, ball, glm::translate(glm::dmat4(1.0), glm::dvec3(50, 50, 0))));
    geo::Mesh dense = geo::makeCube({.width = 100, .height = 100, .depth = 100, .subdivX = 10, .subdivY = 10, .subdivZ = 10});
    geo::Mesh rod = geo::makeCylinder({.radius = 22, .height = 300, .subdivAxis = 24});
    save("dense_band", cut(dense, rod, glm::translate(glm::dmat4(1.0), glm::dvec3(4, 0, 2)), {.quadBand = true, .bandTimeout = 0.0}));
    save("dense_split", cut(dense, rod, glm::translate(glm::dmat4(1.0), glm::dvec3(4, 0, 2))));
    save("dense_band_fillet", cut(dense, rod, glm::translate(glm::dmat4(1.0), glm::dvec3(4, 0, 2)), {.quadBand = true, .bandTimeout = 0.0, .filletWidth = 2.0}));
}

TEST_CASE("boolean quad band remeshes the seam into quads that weld back")
{
    geo::Mesh dense = geo::makeCube({.width = 100, .height = 100, .depth = 100, .subdivX = 10, .subdivY = 10, .subdivZ = 10});
    geo::Mesh rod = geo::makeCylinder({.radius = 22, .height = 300, .subdivAxis = 24});
    glm::dmat4 xf = glm::translate(glm::dmat4(1.0), glm::dvec3(4, 0, 2));
    // No timeout: a loaded machine can take longer than the default, and the fallback is not what this tests.
    scene::BooleanResult r = cut(dense, rod, xf, {.quadBand = true, .bandTimeout = 0.0});
    REQUIRE(r.ok());
    MESSAGE("quad band ", r.quadBand ? "used " : "fell back ", describe(sideCounts(r.mesh)));
    CHECK(r.quadBand);
    CHECK(r.mesh.validate(geo::kValidateManifold).empty());
    auto sizes = sideCounts(r.mesh);
    CHECK(sizes[4] > 10 * (sizes[3] + 1));
    CHECK(largestFace(r.mesh) <= 5);
}

TEST_CASE("boolean quad band keeps the uvs under it")
{
    geo::Mesh dense = geo::makeCube({.width = 100, .height = 100, .depth = 100, .subdivX = 10, .subdivY = 10, .subdivZ = 10});
    geo::Mesh rod = geo::makeCylinder({.radius = 22, .height = 300, .subdivAxis = 24});
    glm::dmat4 xf = glm::translate(glm::dmat4(1.0), glm::dvec3(4, 0, 2));
    scene::BooleanResult band = cut(dense, rod, xf, {.quadBand = true, .bandTimeout = 0.0});
    scene::BooleanResult split = cut(dense, rod, xf);
    REQUIRE(band.quadBand);
    auto uvBand = band.mesh.cornerAttrs().get<glm::vec2>("uv0");
    auto uvSplit = split.mesh.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uvBand.empty());
    // Away from uv seams the band's uvs read the same surface as the untouched result does.
    geo::MeshBvh bvh(split.mesh);
    int checked = 0, off = 0;
    for (geo::Index f = 0; f < band.mesh.faceCapacity(); ++f)
    {
        if (!band.mesh.faceValid(f) || band.mesh.faceNormal(f).y < 0.99)
            continue;
        glm::dvec3 middle = band.mesh.faceCentroid(f);
        band.mesh.forEachFaceCorner(f, [&](geo::Index c) {
            glm::dvec3 p = band.mesh.position(band.mesh.cornerPoint(c));
            geo::BvhClosest hit;
            if (!bvh.closestPoint(p + 0.05 * (middle - p), hit))
                return;
            ++checked;
            off += glm::length(bvh.interpolate<glm::vec2>(uvSplit, hit.triangle, hit.bary) - uvBand[c]) > 0.01f;
        });
    }
    MESSAGE(checked, " top corners checked, ", off, " off");
    CHECK(checked > 100);
    CHECK(off == 0);
}

TEST_CASE("live boolean follows the cutter")
{
    run("newScene");
    std::string box = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Block"}, {"generator", false}, {"subdivisionsX", 6}, {"subdivisionsY", 6}, {"subdivisionsZ", 6}})[0];
    std::string drill = run("polyCylinder", {{"name", "Drill"}, {"generator", false}, {"radius", 15}, {"height", 300}})[0];
    run("polyBoolean", {{"args", {box, drill}}, {"live", true}});
    CHECK(run("objExists", {{"args", {drill}}}).get<bool>());
    json stack = run("modifiers", {{"args", {box}}});
    CHECK(stack.dump().find("BooleanModifier") != std::string::npos);
    const geo::Mesh& before = meshOf(box);
    CHECK(before.validate(geo::kValidateManifold).empty());
    const geo::Index cutFaces = before.numFaces();
    CHECK(cutFaces > 6 * 36);

    // Moving the cutter off the block leaves it whole; moving it back cuts again.
    run("move", {{"args", {drill}}, {"by", {500, 0, 0}}});
    CHECK(meshOf(box).numFaces() == 6 * 36);
    run("move", {{"args", {drill}}, {"by", {-500, 0, 0}}});
    CHECK(meshOf(box).numFaces() == cutFaces);
}
