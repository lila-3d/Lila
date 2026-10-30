#include <doctest/doctest.h>

#include "gn/Exec.h"
#include "gn/Field.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace lila;
using json = nlohmann::json;

namespace topology_tests
{
using N = gn::GNode;
using L = gn::GLink;

gn::GeometryPtr evalNodes(std::vector<N> nodes, std::vector<L> links)
{
    gn::Group g = gn::Group::makeDefault("T");
    g.links.clear();
    g.nodes.insert(g.nodes.end(), nodes.begin(), nodes.end());
    g.links = std::move(links);
    gn::PlanPtr plan = gn::compile(g);
    REQUIRE_MESSAGE(plan->error.empty(), plan->error);
    return gn::execute(*plan, {});
}

struct Probe
{
    gn::GeometryPtr geometry;
    std::vector<double> values;

    const geo::Mesh& mesh() const { return *geometry->mesh; }
    size_t count(double v) const { return size_t(std::count_if(values.begin(), values.end(), [v](double x) { return std::abs(x - v) < 1e-6; })); }
    double max() const { return *std::max_element(values.begin(), values.end()); }
    double min() const { return *std::min_element(values.begin(), values.end()); }
};

/** Stores fieldNode.fieldSocket on the mesh coming out of meshNode.meshSocket and reads it back on the domain. */
Probe probe(std::vector<N> nodes, std::vector<L> links, const char* meshNode, const char* meshSocket, const char* fieldNode, const char* fieldSocket,
            const char* dataType, const char* domain)
{
    nodes.push_back(N{"Probe", "StoreNamedAttribute", {}, {{"name", "t"}, {"dataType", dataType}, {"domain", domain}}});
    links.push_back(L{meshNode, meshSocket, "Probe", "geometry"});
    links.push_back(L{fieldNode, fieldSocket, "Probe", "value"});
    links.push_back(L{"Probe", "geometry", "GroupOutput", "geometry"});
    Probe p;
    p.geometry = evalNodes(std::move(nodes), std::move(links));
    REQUIRE(p.geometry->mesh);
    gn::Domain d = gn::Domain::Point;
    gn::parseDomain(domain, &d);
    gn::FieldContext ctx = gn::FieldContext::forMesh(*p.geometry->mesh, d);
    gn::Domain from = d;
    std::optional<gn::Column> col = gn::readAttribute(ctx, "t", &from);
    REQUIRE(col);
    p.values = gn::expand<double>(gn::convert(*col, gn::SocketType::Float), ctx.size);
    return p;
}

/** A single field node probed on a default cube. */
Probe onCube(N field, const char* socket, const char* type, const char* domain)
{
    std::string name = field.name;
    return probe({N{"C", "MeshCube", {}, {}}, std::move(field)}, {}, "C", "mesh", name.c_str(), socket, type, domain);
}

N indexEquals(const char* name, int value) { return N{name, "Compare", {}, {{"dataType", "int"}, {"operation", "equal"}, {"b", value}}}; }

constexpr double kHalfPi = 1.57079632679489661923;

TEST_CASE("topology measures on a cube")
{
    Probe a = onCube(N{"F", "EdgeAngle", {}, {}}, "unsignedAngle", "float", "edge");
    CHECK(a.values.size() == 12);
    CHECK(a.count(kHalfPi) == 12);
    Probe s = onCube(N{"F", "EdgeAngle", {}, {}}, "signedAngle", "float", "edge");
    CHECK(s.count(kHalfPi) == 12);

    CHECK(onCube(N{"F", "FaceArea", {}, {}}, "area", "float", "face").count(10000.0) == 6);
    CHECK(onCube(N{"F", "IsFacePlanar", {}, {}}, "planar", "bool", "face").count(1.0) == 6);
    CHECK(onCube(N{"F", "VertexNeighbors", {}, {}}, "vertexCount", "int", "point").count(3.0) == 8);
    CHECK(onCube(N{"F", "VertexNeighbors", {}, {}}, "faceCount", "int", "point").count(3.0) == 8);
    CHECK(onCube(N{"F", "FaceNeighbors", {}, {}}, "vertexCount", "int", "face").count(4.0) == 6);
    CHECK(onCube(N{"F", "FaceNeighbors", {}, {}}, "faceCount", "int", "face").count(4.0) == 6);
    CHECK(onCube(N{"F", "EdgeNeighbors", {}, {}}, "faceCount", "int", "edge").count(2.0) == 12);
}

TEST_CASE("mesh islands")
{
    Probe one = onCube(N{"F", "MeshIsland", {}, {}}, "islandCount", "int", "point");
    CHECK(one.count(1.0) == 8);

    // Every face of a cube as its own piece: 18 islands, numbered from 0.
    std::vector<N> nodes = {N{"C", "MeshCube", {}, {}}, N{"D", "DuplicateElements", {}, {{"domain", "face"}, {"amount", 3}}}, N{"I", "MeshIsland", {}, {}}};
    Probe count = probe(nodes, {L{"C", "mesh", "D", "geometry"}}, "D", "geometry", "I", "islandCount", "int", "point");
    CHECK(count.count(18.0) == count.values.size());
    Probe index = probe(nodes, {L{"C", "mesh", "D", "geometry"}}, "D", "geometry", "I", "islandIndex", "int", "face");
    CHECK(index.min() == 0.0);
    CHECK(index.max() == 17.0);
    CHECK(std::set<double>(index.values.begin(), index.values.end()).size() == 18);
}

TEST_CASE("corner and edge lookups")
{
    Probe face = onCube(N{"F", "FaceOfCorner", {}, {}}, "faceIndex", "int", "corner");
    const geo::Mesh& m = face.mesh();
    for (geo::Index k = 0; k < m.cornerCapacity(); ++k)
        CHECK(face.values[k] == double(m.cornerFace(k)));

    Probe inFace = onCube(N{"F", "FaceOfCorner", {}, {}}, "indexInFace", "int", "corner");
    CHECK(inFace.count(0.0) == 6);
    CHECK(inFace.count(3.0) == 6);

    Probe vert = onCube(N{"F", "VertexOfCorner", {}, {}}, "vertexIndex", "int", "corner");
    Probe nextEdge = onCube(N{"F", "EdgesOfCorner", {}, {}}, "nextEdgeIndex", "int", "corner");
    Probe prevEdge = onCube(N{"F", "EdgesOfCorner", {}, {}}, "previousEdgeIndex", "int", "corner");
    Probe fwd = onCube(N{"F", "OffsetCornerInFace", {}, {{"offset", 1}}}, "cornerIndex", "int", "corner");
    Probe back = onCube(N{"F", "OffsetCornerInFace", {}, {{"offset", -1}}}, "cornerIndex", "int", "corner");
    Probe wrap = onCube(N{"F", "OffsetCornerInFace", {}, {{"offset", 4}}}, "cornerIndex", "int", "corner");
    for (geo::Index k = 0; k < m.cornerCapacity(); ++k)
    {
        CHECK(vert.values[k] == double(m.cornerPoint(k)));
        CHECK(nextEdge.values[k] == double(m.cornerEdge(k)));
        CHECK(prevEdge.values[k] == double(m.cornerEdge(m.cornerPrev(k))));
        CHECK(fwd.values[k] == double(m.cornerNext(k)));
        CHECK(back.values[k] == double(m.cornerPrev(k)));
        CHECK(wrap.values[k] == double(k));
    }

    Probe first = onCube(N{"F", "CornersOfFace", {}, {}}, "cornerIndex", "int", "face");
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        CHECK(first.values[f] == double(m.faceCorner(f)));
    CHECK(onCube(N{"F", "CornersOfFace", {}, {}}, "total", "int", "face").count(4.0) == 6);
    CHECK(onCube(N{"F", "CornersOfVertex", {}, {}}, "total", "int", "point").count(3.0) == 8);
    CHECK(onCube(N{"F", "EdgesOfVertex", {}, {}}, "total", "int", "point").count(3.0) == 8);

    // Weighted by corner index: sort index 0 is the lowest corner at the point, -1 wraps to the highest.
    for (int pick : {0, -1})
    {
        Probe c = probe({N{"C", "MeshCube", {}, {}}, N{"I", "Index", {}, {}}, N{"F", "CornersOfVertex", {}, {{"sortIndex", pick}}}},
                        {L{"I", "value", "F", "weights"}}, "C", "mesh", "F", "cornerIndex", "int", "point");
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        {
            std::vector<geo::Index> corners;
            m.forEachPointCorner(p, [&](geo::Index k) { corners.push_back(k); });
            geo::Index want = pick == 0 ? *std::min_element(corners.begin(), corners.end()) : *std::max_element(corners.begin(), corners.end());
            CHECK(c.values[p] == double(want));
        }
    }

    Probe ev = onCube(N{"F", "EdgeVertices", {}, {}}, "vertexIndex2", "int", "edge");
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        CHECK(ev.values[e] == double(m.edgePoint(e, 1)));
}

TEST_CASE("shortest edge paths")
{
    // A 3x3 grid; point 0 and point 8 are opposite corners four edges apart.
    std::vector<N> nodes = {N{"G", "MeshGrid", {}, {}}, N{"I", "Index", {}, {}}, indexEquals("End", 0), N{"S", "ShortestEdgePaths", {}, {}}};
    std::vector<L> links = {L{"I", "value", "End", "a"}, L{"End", "result", "S", "endVertex"}};
    Probe cost = probe(nodes, links, "G", "mesh", "S", "totalCost", "float", "point");
    CHECK(cost.values[0] == 0.0);
    CHECK(cost.values[8] == doctest::Approx(4.0));
    CHECK(cost.max() == doctest::Approx(4.0));
    Probe next = probe(nodes, links, "G", "mesh", "S", "nextVertexIndex", "int", "point");
    CHECK(next.values[0] == 0.0);
    CHECK((next.values[1] == 0.0 && next.values[3] == 0.0));

    nodes.push_back(indexEquals("Start", 8));
    nodes.push_back(N{"P", "EdgePathsToCurves", {}, {}});
    links.push_back(L{"I", "value", "Start", "a"});
    links.push_back(L{"G", "mesh", "P", "mesh"});
    links.push_back(L{"Start", "result", "P", "startVertices"});
    links.push_back(L{"S", "nextVertexIndex", "P", "nextVertexIndex"});
    std::vector<L> toCurves = links;
    toCurves.push_back(L{"P", "curves", "GroupOutput", "geometry"});
    gn::GeometryPtr curves = evalNodes(nodes, toCurves);
    REQUIRE(curves->curves);
    CHECK(curves->curves->numCurves() == 1);
    CHECK(curves->curves->numPoints() == 5);

    nodes.push_back(N{"Sel", "EdgePathsToSelection", {}, {}});
    links.push_back(L{"Start", "result", "Sel", "startVertices"});
    links.push_back(L{"S", "nextVertexIndex", "Sel", "nextVertexIndex"});
    Probe sel = probe(nodes, links, "G", "mesh", "Sel", "selection", "bool", "edge");
    CHECK(sel.count(1.0) == 4);
}

TEST_CASE("sort elements")
{
    // Sorting grid points by z puts the back row (lowest z) first; ties keep their order.
    gn::GeometryPtr g = evalNodes({N{"G", "MeshGrid", {}, {}}, N{"P", "Position", {}, {}}, N{"X", "SeparateXYZ", {}, {}}, N{"S", "SortElements", {}, {}}},
                                  {L{"G", "mesh", "S", "geometry"}, L{"P", "value", "X", "vector"}, L{"X", "z", "S", "sortWeight"},
                                   L{"S", "geometry", "GroupOutput", "geometry"}});
    REQUIRE(g->mesh);
    const geo::Mesh& m = *g->mesh;
    CHECK(m.numPoints() == 9);
    CHECK(m.numFaces() == 4);
    CHECK(m.validate().empty());
    CHECK(m.position(0).x == doctest::Approx(-50.0));
    CHECK(m.position(0).z == doctest::Approx(-50.0));
    CHECK(m.position(8).x == doctest::Approx(50.0));
    CHECK(m.position(8).z == doctest::Approx(50.0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        CHECK(m.faceNormal(f).y == doctest::Approx(1.0));
}

TEST_CASE("face groups")
{
    std::vector<N> nodes = {N{"C", "MeshCube", {}, {}}, N{"A", "EdgeAngle", {}, {}}, N{"Cmp", "Compare", {}, {{"b", 0.5}}}, N{"G", "EdgesToFaceGroups", {}, {}}};
    std::vector<L> links = {L{"A", "unsignedAngle", "Cmp", "a"}, L{"Cmp", "result", "G", "boundaryEdges"}};
    Probe groups = probe(nodes, links, "C", "mesh", "G", "faceGroupId", "int", "face");
    CHECK(std::set<double>(groups.values.begin(), groups.values.end()).size() == 6);

    Probe whole = onCube(N{"G", "EdgesToFaceGroups", {}, {{"boundaryEdges", false}}}, "faceGroupId", "int", "face");
    CHECK(whole.count(0.0) == 6);

    Probe all = probe({N{"C", "MeshCube", {}, {}}, N{"I", "Index", {}, {}}, N{"B", "FaceGroupBoundaries", {}, {}}}, {L{"I", "value", "B", "faceGroupId"}}, "C",
                      "mesh", "B", "boundaryEdges", "bool", "edge");
    CHECK(all.count(1.0) == 12);
    CHECK(onCube(N{"B", "FaceGroupBoundaries", {}, {}}, "boundaryEdges", "bool", "edge").count(0.0) == 12);
}

TEST_CASE("curvature")
{
    Probe mean = probe({N{"S", "MeshIcoSphere", {}, {{"subdivisions", 4}}}, N{"K", "Curvature", {}, {}}}, {}, "S", "mesh", "K", "mean", "float", "point");
    double sum = 0.0;
    for (double v : mean.values)
        sum += v;
    CHECK(sum / double(mean.values.size()) == doctest::Approx(1.0 / 50.0).epsilon(0.05));
    CHECK(mean.min() > 0.0);
    Probe gauss = probe({N{"S", "MeshIcoSphere", {}, {{"subdivisions", 4}}}, N{"K", "Curvature", {}, {}}}, {}, "S", "mesh", "K", "gaussian", "float", "point");
    CHECK(gauss.min() > 0.0);

    Probe flat = probe({N{"G", "MeshGrid", {}, {}}, N{"K", "Curvature", {}, {}}}, {}, "G", "mesh", "K", "mean", "float", "point");
    CHECK(flat.values[4] == doctest::Approx(0.0));
    Probe flatK = probe({N{"G", "MeshGrid", {}, {}}, N{"K", "Curvature", {}, {}}}, {}, "G", "mesh", "K", "gaussian", "float", "point");
    CHECK(flatK.values[4] == doctest::Approx(0.0));
}

TEST_CASE("cavity")
{
    // With every other corner in reach, a cube corner sits well above its surroundings.
    Probe convex = onCube(N{"K", "Cavity", {}, {{"radius", 200.0}}}, "convexity", "float", "point");
    CHECK(convex.min() > 0.5);
    CHECK(onCube(N{"K", "Cavity", {}, {{"radius", 200.0}}}, "cavity", "float", "point").max() == 0.0);

    Probe inside = probe({N{"C", "MeshCube", {}, {}}, N{"Fl", "FlipFaces", {}, {}}, N{"K", "Cavity", {}, {{"radius", 200.0}}}}, {L{"C", "mesh", "Fl", "mesh"}},
                         "Fl", "mesh", "K", "cavity", "float", "point");
    CHECK(inside.min() > 0.5);

    Probe flat = probe({N{"G", "MeshGrid", {}, {}}, N{"K", "Cavity", {}, {}}}, {}, "G", "mesh", "K", "cavity", "float", "point");
    CHECK(flat.max() == doctest::Approx(0.0));
}

TEST_CASE("thickness and occlusion")
{
    Probe t = onCube(N{"T", "Thickness", {}, {}}, "thickness", "float", "face");
    for (double v : t.values)
        CHECK(v == doctest::Approx(100.0).epsilon(1e-4));
    CHECK(onCube(N{"T", "Thickness", {}, {{"maxDistance", 50.0}}}, "thickness", "float", "face").max() == doctest::Approx(50.0));

    CHECK(onCube(N{"A", "AmbientOcclusion", {}, {}}, "ao", "float", "face").min() == doctest::Approx(1.0));
    Probe enclosed = probe({N{"C", "MeshCube", {}, {}}, N{"Fl", "FlipFaces", {}, {}}, N{"A", "AmbientOcclusion", {}, {{"distance", 1000.0}}}},
                           {L{"C", "mesh", "Fl", "mesh"}}, "Fl", "mesh", "A", "ao", "float", "face");
    CHECK(enclosed.max() == doctest::Approx(0.0));
}

TEST_CASE("geodesic distance")
{
    // From one corner of the 100 x 100 grid: the far corner is the diagonal, the near corners the side length.
    Probe d = probe({N{"G", "MeshGrid", {}, {}}, N{"I", "Index", {}, {}}, indexEquals("Src", 0), N{"D", "GeodesicDistance", {}, {}}},
                    {L{"I", "value", "Src", "a"}, L{"Src", "result", "D", "source"}}, "G", "mesh", "D", "distance", "float", "point");
    CHECK(d.values[0] == 0.0);
    CHECK(d.values[2] == doctest::Approx(100.0));
    CHECK(d.values[6] == doctest::Approx(100.0));
    CHECK(d.values[8] == doctest::Approx(100.0 * std::sqrt(2.0)));
    Probe src = probe({N{"G", "MeshGrid", {}, {}}, N{"I", "Index", {}, {}}, indexEquals("Src", 0), N{"D", "GeodesicDistance", {}, {}}},
                      {L{"I", "value", "Src", "a"}, L{"Src", "result", "D", "source"}}, "G", "mesh", "D", "sourceIndex", "int", "point");
    CHECK(src.count(0.0) == 9);
}

TEST_CASE("flood fill")
{
    auto grid = [](int steps) {
        return probe({N{"G", "MeshGrid", {}, {}}, N{"I", "Index", {}, {}}, indexEquals("Seed", 0), N{"F", "FloodFill", {}, {{"steps", steps}}}},
                     {L{"I", "value", "Seed", "a"}, L{"Seed", "result", "F", "seed"}}, "G", "mesh", "F", "selection", "bool", "point");
    };
    CHECK(grid(1).count(1.0) == 3);
    CHECK(grid(0).count(1.0) == 9);

    // Growing from the top of a cube stops at the corners when the normals turn too far.
    auto cube = [](double angle) {
        return probe({N{"C", "MeshCube", {}, {}}, N{"P", "Position", {}, {}}, N{"X", "SeparateXYZ", {}, {}}, N{"Top", "Compare", {}, {{"b", 25.0}}},
                      N{"F", "FloodFill", {}, {{"steps", 0}, {"angleLimit", angle}}}},
                     {L{"P", "value", "X", "vector"}, L{"X", "y", "Top", "a"}, L{"Top", "result", "F", "seed"}}, "C", "mesh", "F", "selection", "bool",
                     "point");
    };
    CHECK(cube(30.0).count(1.0) == 4);
    CHECK(cube(90.0).count(1.0) == 8);
}

TEST_CASE("feature edges")
{
    CHECK(onCube(N{"F", "FeatureEdges", {}, {}}, "sharp", "bool", "edge").count(1.0) == 12);
    CHECK(onCube(N{"F", "FeatureEdges", {}, {}}, "boundary", "bool", "edge").count(1.0) == 0);
    Probe grid = probe({N{"G", "MeshGrid", {}, {}}, N{"F", "FeatureEdges", {}, {}}}, {}, "G", "mesh", "F", "boundary", "bool", "edge");
    CHECK(grid.count(1.0) == 8);
    Probe gridSharp = probe({N{"G", "MeshGrid", {}, {}}, N{"F", "FeatureEdges", {}, {}}}, {}, "G", "mesh", "F", "sharp", "bool", "edge");
    CHECK(gridSharp.count(1.0) == 0);
}
} // namespace topology_tests
