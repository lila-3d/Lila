#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Attribute.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/TestUtil.h"
#include "retopo/Retopo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <doctest/doctest.h>

using namespace lila;
using geo::Index;
using retopo::MeshIssue;

namespace
{
geo::Mesh soup(const std::vector<glm::dvec3>& pts, const std::vector<Index>& sizes, const std::vector<Index>& idx)
{
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

/** Only the given check enabled. */
retopo::RepairOptions only(MeshIssue issue)
{
    retopo::RepairOptions o;
    o.checks.fill(false);
    o.set(issue, true);
    o.removeLoose = false;
    return o;
}

Index found(const geo::Mesh& m, MeshIssue issue) { return retopo::findIssues(m, only(issue)).count(issue); }

/** Repairs one issue, checks it was counted and is gone. */
void repairs(geo::Mesh& m, MeshIssue issue, Index expect)
{
    retopo::RepairReport r = retopo::repairMesh(m, only(issue));
    MESSAGE(r.summary());
    CHECK(r.fixed[size_t(issue)] == expect);
    CHECK(found(m, issue) == 0);
    CHECK(geo::test::isValid(m));
}

scene::Node* makeMeshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

const geo::Mesh& outMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** A quad fan around the edge (0,0,0)-(0,1,0) with three faces on it. */
geo::Mesh fin()
{
    return soup({{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {-1, 1, 0}, {-1, 0, 0}, {0, 1, 1}, {0, 0, 1}}, {4, 4, 4},
                {0, 1, 2, 3, 1, 0, 5, 4, 0, 1, 6, 7});
}

/** Two triangles touching at one point. */
geo::Mesh bowtie()
{
    return soup({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {-1, 0, 0}, {-1, -1, 0}}, {3, 3}, {0, 1, 2, 0, 3, 4});
}
} // namespace

TEST_CASE("repair checks: a clean cube has no issues")
{
    geo::Mesh cube = geo::makeCube();
    retopo::RepairOptions all;
    all.checks.fill(true);
    retopo::MeshIssues issues = retopo::findIssues(cube, all);
    MESSAGE(issues.summary());
    CHECK(issues.total() == 0);
    retopo::RepairReport r = retopo::repairMesh(cube, all);
    CHECK(r.summary() == "nothing to repair");
    CHECK(cube.numFaces() == 6);
}

TEST_CASE("repair checks: coincident points")
{
    // Two quads side by side whose shared corners were never welded.
    geo::Mesh m = soup({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}}, {4, 4},
                       {0, 1, 2, 3, 4, 5, 6, 7});
    retopo::MeshIssues issues = retopo::findIssues(m, only(MeshIssue::Coincident));
    CHECK(issues.of(MeshIssue::Coincident) == std::vector<Index>{1, 2, 4, 7});
    repairs(m, MeshIssue::Coincident, 4);
    CHECK(m.numPoints() == 6);
    CHECK(geo::test::boundaryEdgeCount(m) == 6);

    // A looser tolerance catches near misses.
    geo::Mesh near = soup({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0.001, 0, 0}, {0, -1, 0}, {-1, 0, 0}}, {3, 3}, {0, 1, 2, 3, 4, 5});
    CHECK(found(near, MeshIssue::Coincident) == 0);
    retopo::RepairOptions loose = only(MeshIssue::Coincident);
    loose.tolerance = 0.01;
    CHECK(retopo::findIssues(near, loose).count(MeshIssue::Coincident) == 2);
}

TEST_CASE("repair checks: zero-length edges")
{
    // A quad with its second corner doubled up.
    geo::Mesh m = soup({{0, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {5}, {0, 1, 2, 3, 4});
    CHECK(found(m, MeshIssue::ZeroLengthEdge) == 1);
    repairs(m, MeshIssue::ZeroLengthEdge, 1);
    m.compact();
    CHECK(m.numFaces() == 1);
    CHECK(m.faceSize(0) == 4);
}

TEST_CASE("repair checks: lamina faces")
{
    geo::Mesh m = soup({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {4, 4}, {0, 1, 2, 3, 3, 2, 1, 0});
    CHECK(retopo::findIssues(m, only(MeshIssue::Lamina)).of(MeshIssue::Lamina) == std::vector<Index>{1});
    repairs(m, MeshIssue::Lamina, 1);
    CHECK(m.numFaces() == 1);
}

TEST_CASE("repair checks: zero-area faces")
{
    // A collinear sliver under the quad's bottom edge.
    geo::Mesh m = soup({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0.5, 0, 0}}, {4, 3}, {0, 1, 2, 3, 1, 0, 4});
    CHECK(retopo::findIssues(m, only(MeshIssue::ZeroAreaFace)).of(MeshIssue::ZeroAreaFace) == std::vector<Index>{1});
    repairs(m, MeshIssue::ZeroAreaFace, 1);
    m.compact();
    // Merged into the quad, so no hole opens where the sliver was.
    CHECK(m.numFaces() == 1);
    CHECK(m.faceSize(0) == 5);
    CHECK(m.faceArea(0) == doctest::Approx(1.0));
}

TEST_CASE("repair checks: non-manifold edges and vertices")
{
    geo::Mesh f = fin();
    CHECK(found(f, MeshIssue::NonManifoldEdge) == 1);
    // The fin's end points are reported as the edge, not again as vertices.
    CHECK(found(f, MeshIssue::NonManifoldPoint) == 0);
    repairs(f, MeshIssue::NonManifoldEdge, 1);
    CHECK(f.numPoints() == 10);

    geo::Mesh b = bowtie();
    CHECK(retopo::findIssues(b, only(MeshIssue::NonManifoldPoint)).of(MeshIssue::NonManifoldPoint) == std::vector<Index>{0});
    CHECK(found(b, MeshIssue::NonManifoldEdge) == 0);
    repairs(b, MeshIssue::NonManifoldPoint, 1);
    CHECK(b.numPoints() == 6);
}

TEST_CASE("repair checks: holes")
{
    geo::Mesh m = geo::makeCube();
    std::vector<Index> one = {0};
    geo::deleteFaces(m, one);
    CHECK(found(m, MeshIssue::Hole) == 4);
    repairs(m, MeshIssue::Hole, 4);
    CHECK(m.numFaces() == 6);
    CHECK(geo::test::signedVolume(m) == doctest::Approx(1e6));
}

TEST_CASE("repair checks: triangles")
{
    geo::Mesh m = soup({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}, {3, 3}, {0, 1, 2, 0, 2, 3});
    CHECK(retopo::findIssues(m, only(MeshIssue::Triangle)).of(MeshIssue::Triangle) == std::vector<Index>{0, 1});
    repairs(m, MeshIssue::Triangle, 2);
    CHECK(m.numFaces() == 1);
}

TEST_CASE("repair checks: n-gons")
{
    std::vector<glm::dvec3> pts;
    std::vector<Index> idx;
    for (int i = 0; i < 6; ++i)
    {
        double a = i * 3.14159265358979 / 3.0;
        pts.push_back({std::cos(a), std::sin(a), 0.0});
        idx.push_back(Index(i));
    }
    geo::Mesh m = soup(pts, {6}, idx);
    const double area = m.faceArea(0);
    CHECK(found(m, MeshIssue::Ngon) == 1);
    repairs(m, MeshIssue::Ngon, 1);
    double sum = 0.0;
    Index quads = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            sum += m.faceArea(f);
            quads += m.faceSize(f) == 4;
        }
    CHECK(sum == doctest::Approx(area));
    CHECK(quads >= 1);
    CHECK(found(m, MeshIssue::Concave) == 0);
}

TEST_CASE("repair checks: concave faces")
{
    // A dart: the fourth corner bends in.
    geo::Mesh m = soup({{0, 0, 0}, {2, 1, 0}, {0, 2, 0}, {0.5, 1, 0}}, {4}, {0, 1, 2, 3});
    CHECK(found(m, MeshIssue::Concave) == 1);
    CHECK(found(geo::makeCube(), MeshIssue::Concave) == 0);
    repairs(m, MeshIssue::Concave, 1);
    CHECK(m.numFaces() == 2);
    CHECK(m.numPoints() == 4);
}

TEST_CASE("repair checks: zero-area uvs are found and left alone")
{
    geo::Mesh m = soup({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {2, 0, 0}, {2, 1, 0}}, {4, 4}, {0, 1, 2, 3, 1, 4, 5, 2});
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    m.forEachFaceCorner(0, [&](Index c) { uv[c] = glm::vec2(m.position(m.cornerPoint(c))) * 0.5f; });
    m.forEachFaceCorner(1, [&](Index c) { uv[c] = glm::vec2(0.75f, 0.25f); });
    CHECK(retopo::findIssues(m, only(MeshIssue::ZeroAreaUv)).of(MeshIssue::ZeroAreaUv) == std::vector<Index>{1});
    retopo::RepairReport r = retopo::repairMesh(m, only(MeshIssue::ZeroAreaUv));
    CHECK(r.fixed[size_t(MeshIssue::ZeroAreaUv)] == 1);
    CHECK(r.summary().find("new unwrap") != std::string::npos);
    CHECK(m.numFaces() == 2);
}

TEST_CASE("repair checks: flipped normals")
{
    geo::Mesh m = geo::makeCube();
    m.flipFace(2);
    CHECK(retopo::findIssues(m, only(MeshIssue::Flipped)).of(MeshIssue::Flipped) == std::vector<Index>{2});
    repairs(m, MeshIssue::Flipped, 1);
    CHECK(geo::test::signedVolume(m) == doctest::Approx(1e6));

    // A whole cube turned inside out faces inward: every face is flipped.
    geo::Mesh inside = geo::makeCube();
    for (Index f = 0; f < 6; ++f)
        inside.flipFace(f);
    CHECK(found(inside, MeshIssue::Flipped) == 6);
}

TEST_CASE("repair checks: summaries name each issue")
{
    retopo::RepairOptions o;
    retopo::MeshIssues issues = retopo::findIssues(bowtie(), o);
    CHECK(issues.summary() == "1 non-manifold vertices");
    CHECK(issues.elements(retopo::IssueElement::Point) == std::vector<Index>{0});
    CHECK(issues.elements(retopo::IssueElement::Face).empty());
}

TEST_CASE("commands: meshRepair select mode selects issues in their component mode")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    scene::components::clear();
    scene::components::setMode(scene::ComponentMode::Object);

    scene::NodeId finId = makeMeshNode("fin", fin())->id();
    json r = ops::execute("meshRepair", {{"args", {"fin"}}, {"mode", "select"}, {"flippedNormals", false}});
    CHECK(r["fin"]["nonManifoldEdges"] == 1);
    CHECK(r["fin"]["lamina"] == 0);
    CHECK(scene::components::mode() == scene::ComponentMode::Edge);
    CHECK(scene::components::selected(finId, scene::ComponentMode::Edge).size() == 1);
    CHECK(outMesh("fin").numFaces() == 3);

    scene::NodeId bowId = makeMeshNode("bow", bowtie())->id();
    ops::execute("meshRepair", {{"args", {"bow"}}, {"mode", "select"}});
    CHECK(scene::components::mode() == scene::ComponentMode::Vertex);
    CHECK(scene::components::selected(bowId, scene::ComponentMode::Vertex) == scene::components::Ids{0});

    // Faces for face issues: n-gons when the check is on.
    geo::Mesh pent = soup({{0, 0, 0}, {1, 0, 0}, {1.5, 1, 0}, {0.5, 2, 0}, {-0.5, 1, 0}}, {5}, {0, 1, 2, 3, 4});
    scene::NodeId pentId = makeMeshNode("pent", std::move(pent))->id();
    ops::execute("meshRepair", {{"args", {"pent"}}, {"mode", "select"}, {"ngons", true}});
    CHECK(scene::components::mode() == scene::ComponentMode::Face);
    CHECK(scene::components::selected(pentId, scene::ComponentMode::Face).size() == 1);
    // Undo goes back to the mode the previous selection was made in.
    UndoStack::get().undo();
    CHECK(scene::components::mode() == scene::ComponentMode::Vertex);
    CHECK(scene::components::selected(bowId, scene::ComponentMode::Vertex) == scene::components::Ids{0});
    UndoStack::get().redo();
    CHECK(scene::components::mode() == scene::ComponentMode::Face);

    CHECK_THROWS_AS(ops::execute("meshRepair", {{"args", {"pent"}}, {"mode", "bogus"}}), CommandError);

    ops::execute("meshRepair", {{"args", {"fin"}}});
    CHECK(outMesh("fin").numPoints() == 10);
    UndoStack::get().undo();
    CHECK(outMesh("fin").numPoints() == 8);
}

TEST_CASE("commands: voxelRemesh rebuilds and undoes")
{
    scene::Graph::current().clear();
    UndoStack::get().clear();
    geo::Mesh cube = geo::makeCube();
    std::vector<Index> one = {0};
    geo::deleteFaces(cube, one);
    cube.compact();
    makeMeshNode("box", std::move(cube));
    json r = ops::execute("voxelRemesh", {{"args", {"box"}}, {"resolution", 16}});
    CHECK(r["box"].get<int>() > 100);
    CHECK(geo::test::boundaryEdgeCount(outMesh("box")) == 0);
    UndoStack::get().undo();
    CHECK(outMesh("box").numFaces() == 5);
}
