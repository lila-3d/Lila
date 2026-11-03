#include "TestUtil.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Bvh.h"
#include "geo/TestUtil.h"
#include "geo/ObjIO.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

using namespace lila;
using namespace retopotest;
using geo::Index;

namespace
{
geo::Mesh highResSphere(int subdiv = 128)
{
    geo::SphereOptions o;
    o.radius = 50.0;
    o.subdivAxis = subdiv;
    o.subdivHeight = subdiv;
    return geo::makeSphere(o);
}

geo::Mesh bevelledCube()
{
    geo::Mesh m = geo::makeCube();
    std::vector<Index> edges;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
            edges.push_back(e);
    geo::BevelOptions b;
    b.offset = 10.0;
    b.segments = 3;
    geo::bevelEdges(m, edges, b);
    m.compact();
    return m;
}

/** Writes the mesh next to the test binary when LILA_RETOPO_DUMP is set, for eyeballing. */
void dumpFile(const geo::Mesh& m, const char* name)
{
    if (std::getenv("LILA_RETOPO_DUMP"))
        geo::writeObjFile((std::filesystem::temp_directory_path() / name).string(), m);
}

double fractionValence4(const retopo::QualityMetrics& q)
{
    Index total = 0;
    for (Index c : q.valence)
        total += c;
    return total ? double(q.valence.size() > 4 ? q.valence[4] : 0) / double(total) : 0.0;
}

bool samePositions(const geo::Mesh& a, const geo::Mesh& b)
{
    if (a.pointCapacity() != b.pointCapacity() || a.numFaces() != b.numFaces())
        return false;
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (a.position(p) != b.position(p))
            return false;
    return true;
}
} // namespace

TEST_CASE("quad remesh: high-res sphere")
{
    geo::Mesh src = highResSphere();
    retopo::QuadRemeshOptions o;
    o.targetFaces = 2000;
    retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
    REQUIRE(r.ok());
    report("sphere", r);
    dumpFile(r.mesh, "retopo_sphere.obj");
    const retopo::QualityMetrics& q = r.metrics;
    CHECK(q.quadPercent > 95.0);
    CHECK(q.faces > 1400);
    CHECK(q.faces < 2800);
    CHECK(q.maxDistance < 0.5);
    CHECK(fractionValence4(q) > 0.9);
    CHECK(r.mesh.validate().empty());
    CHECK(geo::test::boundaryEdgeCount(r.mesh) == 0);

    retopo::QuadRemeshResult again = retopo::quadRemesh(src, o);
    CHECK(samePositions(r.mesh, again.mesh));
}

TEST_CASE("quad remesh: bevelled cube")
{
    geo::Mesh src = bevelledCube();
    dumpFile(src, "retopo_cube_src.obj");
    retopo::QuadRemeshOptions o;
    o.targetFaces = 2000;
    retopo::QuadRemeshResult r = retopo::quadRemesh(src, o);
    REQUIRE(r.ok());
    report("bevelled cube", r);
    dumpFile(r.mesh, "retopo_cube.obj");
    const retopo::QualityMetrics& q = r.metrics;
    CHECK(q.quadPercent > 95.0);
    CHECK(q.faces > 1400);
    CHECK(q.faces < 2800);
    // Face centres across the bevel's crease rows sit off the surface a little.
    CHECK(q.maxDistance < 0.02 * glm::length(src.bounds().size()));
    CHECK(fractionValence4(q) > 0.85);
    CHECK(r.mesh.validate().empty());
    CHECK(geo::test::boundaryEdgeCount(r.mesh) == 0);

    retopo::QuadRemeshResult again = retopo::quadRemesh(src, o);
    CHECK(samePositions(r.mesh, again.mesh));
}
