#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/FoldEdges.h"
#include "geo/Ops.h"
#include "scene/AutoBevel.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"

#include <filesystem>

using namespace lila;
using geo::Index;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
int faces(const std::string& m) { return run("meshInfo", {{"args", {m}}})["faces"].get<int>(); }

/** An L-shaped profile 1 cm thick with 10 cm legs, extruded 10 cm along z: a bent sheet with thickness. */
geo::Mesh lBracket()
{
    const glm::dvec2 profile[6] = {{0, 0}, {10, 0}, {10, 1}, {1, 1}, {1, 10}, {0, 10}};
    std::vector<glm::dvec3> pts;
    for (double z : {0.0, 10.0})
        for (const glm::dvec2& p : profile)
            pts.push_back({p.x, p.y, z});
    std::vector<Index> sizes, idx;
    // Caps: the back one is wound so its normal faces -z.
    sizes.push_back(6);
    for (Index i = 6; i-- > 0;)
        idx.push_back(i);
    sizes.push_back(6);
    for (Index i = 0; i < 6; ++i)
        idx.push_back(6 + i);
    for (Index i = 0; i < 6; ++i)
    {
        Index j = (i + 1) % 6;
        sizes.push_back(4);
        for (Index k : {i, j, 6 + j, 6 + i})
            idx.push_back(k);
    }
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

Index edgeAt(const geo::Mesh& m, glm::dvec3 a, glm::dvec3 b)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        glm::dvec3 p = m.position(m.edgePoint(e, 0)), q = m.position(m.edgePoint(e, 1));
        if ((glm::length(p - a) < 1e-6 && glm::length(q - b) < 1e-6) || (glm::length(p - b) < 1e-6 && glm::length(q - a) < 1e-6))
            return e;
    }
    return geo::kInvalid;
}

/** The long (z) edge of the bracket at profile point (x, y). */
Index longEdge(const geo::Mesh& m, double x, double y) { return edgeAt(m, {x, y, 0}, {x, y, 10}); }

bool has(const std::vector<Index>& v, Index e) { return std::find(v.begin(), v.end(), e) != v.end(); }
} // namespace

TEST_CASE("fold edges on a bent sheet skip its thickness")
{
    geo::Mesh m = lBracket();
    REQUIRE(m.validate().empty());
    Index outer = longEdge(m, 0, 0), inner = longEdge(m, 1, 1);
    REQUIRE(outer != geo::kInvalid);
    REQUIRE(inner != geo::kInvalid);
    CHECK(geo::edgeIsConvex(m, outer));
    CHECK_FALSE(geo::edgeIsConvex(m, inner));
    CHECK(geo::edgeFoldAngle(m, outer) == doctest::Approx(90.0));

    geo::FoldEdgeOptions o;
    o.completeCorners = false;
    // Every edge is a 90 degree fold without the thickness filter.
    CHECK(geo::foldEdges(m, o).size() == 18);

    o.thin = 1.5;
    std::vector<Index> folds = geo::foldEdges(m, o);
    CHECK(folds == std::vector<Index>{std::min(outer, inner), std::max(outer, inner)});

    o.side = geo::FoldSide::Outside;
    CHECK(geo::foldEdges(m, o) == std::vector<Index>{outer});
    o.side = geo::FoldSide::Inside;
    CHECK(geo::foldEdges(m, o) == std::vector<Index>{inner});

    // The angle range and min length filter too.
    o.side = geo::FoldSide::Both;
    o.minAngle = 95.0;
    CHECK(geo::foldEdges(m, o).empty());
    o.minAngle = 30.0;
    o.minLength = 11.0;
    CHECK(geo::foldEdges(m, o).empty());

    // A bevel of just the folds keeps the mesh closed.
    geo::bevelEdges(m, folds, {.offset = 0.3, .segments = 2});
    m.compact();
    CHECK(m.validate().empty());
}

TEST_CASE("thin width measures across the face")
{
    geo::Mesh m = lBracket();
    Index e = longEdge(m, 10, 0);
    Index c = m.edgeCorner(e);
    double w0 = geo::faceWidthAt(m, m.cornerFace(c), e), w1 = geo::faceWidthAt(m, m.cornerFace(m.cornerRadial(c)), e);
    CHECK(std::min(w0, w1) == doctest::Approx(1.0));
    CHECK(std::max(w0, w1) == doctest::Approx(10.0));
}

TEST_CASE("fold corners get the loops that join them")
{
    // Ground grid 7 x 4 quads with two 2 x 2 blocks raised 1 cm, one ground quad apart.
    const int W = 7, H = 4;
    std::vector<glm::dvec3> pts;
    for (int y = 0; y <= H; ++y)
        for (int x = 0; x <= W; ++x)
            pts.push_back({double(x), double(y), 0.0});
    std::vector<Index> sizes, idx;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            sizes.push_back(4);
            for (int k : {y * (W + 1) + x, y * (W + 1) + x + 1, (y + 1) * (W + 1) + x + 1, (y + 1) * (W + 1) + x})
                idx.push_back(Index(k));
        }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    std::vector<Index> blocks;
    for (int y : {1, 2})
        for (int x : {1, 2, 4, 5})
            blocks.push_back(Index(y * W + x));
    geo::extrudeFaces(m, blocks, {.offset = 1.0});
    m.compact();
    REQUIRE(m.validate().empty());

    geo::FoldEdgeOptions o;
    o.completeCorners = false;
    std::vector<Index> plain = geo::foldEdges(m, o);
    CHECK(plain.size() == 40);

    o.completeCorners = true;
    std::vector<Index> full = geo::foldEdges(m, o);
    CHECK(full.size() == 42);
    Index joinLow = edgeAt(m, {3, 1, 0}, {4, 1, 0}), joinHigh = edgeAt(m, {3, 3, 0}, {4, 3, 0});
    CHECK(has(full, joinLow));
    CHECK(has(full, joinHigh));
    CHECK_FALSE(has(plain, joinLow));
    // Loops that run off the open border are dead ends.
    CHECK_FALSE(has(full, edgeAt(m, {3, 1, 0}, {3, 0, 0})));
}

TEST_CASE("edge matcher finds edges after the mesh is rebuilt")
{
    geo::Mesh m = lBracket();
    Index outer = longEdge(m, 0, 0);
    geo::EdgeRef ref = geo::edgeRef(m, outer);

    // Same ids, moved point: matched by id while the topology is unchanged.
    geo::Mesh moved = m;
    moved.setPosition(ref.p0, moved.position(ref.p0) + glm::dvec3(-1, 0, 0));
    geo::EdgeMatch how;
    CHECK(geo::EdgeMatcher(moved).find(ref, true, &how) == outer);
    CHECK(how == geo::EdgeMatch::Id);
    CHECK(geo::EdgeMatcher(moved).find(ref, false, &how) == geo::kInvalid);

    // Rebuilt with other ids: found by its endpoints.
    geo::Mesh other = m;
    geo::bevelEdges(other, std::vector<Index>{longEdge(other, 1, 1)}, {.offset = 0.2, .segments = 3});
    other.compact();
    Index want = longEdge(other, 0, 0);
    REQUIRE(want != geo::kInvalid);
    CHECK(geo::EdgeMatcher(other).find(ref, false, &how) == want);
    CHECK(how != geo::EdgeMatch::Lost);

    // Split in two: the collinear piece nearest the old midpoint.
    geo::Mesh split = m;
    split.splitEdge(longEdge(split, 0, 0), 0.3);
    Index e = geo::EdgeMatcher(split).find(ref, false, &how);
    CHECK(how == geo::EdgeMatch::Overlap);
    REQUIRE(e != geo::kInvalid);
    CHECK(split.edgeLength(e) == doctest::Approx(7.0));
}

namespace
{
std::shared_ptr<const geo::Mesh> inputOf(const std::string& mod)
{
    auto set = scene::autoBevel::chosen(scene::Graph::current(), scene::Graph::current().find(mod)->id());
    REQUIRE(set);
    return set->mesh;
}

/** Edit Edges on a modifier, picking exactly the edge between a and b on its input. */
void pickEdge(const std::string& cube, const std::string& mod, glm::dvec3 a, glm::dvec3 b)
{
    run("bevelEditEdges", {{"args", {mod}}});
    Index e = edgeAt(*inputOf(mod), a, b);
    REQUIRE(e != geo::kInvalid);
    run("selectComponents", {{"args", {cube}}, {"edges", {e}}});
    run("bevelApplyEdges", {{"args", {mod}}});
}

/** The bevelled edge of a one-edge modifier, as its endpoints. */
std::pair<glm::dvec3, glm::dvec3> pickedEdge(const std::string& mod)
{
    json info = run("bevelEdgeInfo", {{"args", {mod}}});
    REQUIRE(info["count"] == 1);
    CHECK(info["lost"] == 0);
    auto in = inputOf(mod);
    Index e = info["edges"][0].get<Index>();
    return {in->position(in->edgePoint(e, 0)), in->position(in->edgePoint(e, 1))};
}

bool sameEdge(std::pair<glm::dvec3, glm::dvec3> e, glm::dvec3 a, glm::dvec3 b)
{
    auto near = [](glm::dvec3 p, glm::dvec3 q) { return glm::length(p - q) < 1e-6; };
    return (near(e.first, a) && near(e.second, b)) || (near(e.first, b) && near(e.second, a));
}
} // namespace

TEST_CASE("autoBevel modifier bevels folds and stays live")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string mod = run("autoBevel", {{"args", {cube}}, {"segments", 1}, {"offset", 5.0}})[0];
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["count"] == 12);
    // A one-segment bevel of every cube edge: 6 faces, 12 strips, 8 corners.
    CHECK(faces(cube) == 26);
    run("setAttr", {{"args", {mod + ".minAngle", 100.0}}});
    CHECK(faces(cube) == 6);
    run("setAttr", {{"args", {mod + ".mode", "angle"}}});
    run("setAttr", {{"args", {mod + ".minAngle", 30.0}}});
    CHECK(faces(cube) == 26);
    run("setAttr", {{"args", {mod + ".mode", "weight"}}});
    CHECK(faces(cube) == 6);
}

TEST_CASE("Edit Edges stores overrides that survive upstream changes")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    const glm::dvec3 a0(-50, 50, 50), a1(50, 50, 50), b0(-50, -50, -50), b1(50, -50, -50);
    std::string first = run("autoBevel", {{"args", {cube}}, {"mode", "manual"}, {"segments", 1}, {"offset", 5.0}})[0];
    std::string second = run("autoBevel", {{"args", {cube}}, {"mode", "manual"}, {"segments", 2}, {"offset", 5.0}})[0];
    pickEdge(cube, first, a0, a1);
    pickEdge(cube, second, b0, b1);
    CHECK(sameEdge(pickedEdge(first), a0, a1));
    CHECK(sameEdge(pickedEdge(second), b0, b1));

    // Re-bevelling upstream renumbers the second modifier's input, but it keeps its edge.
    for (int segments : {3, 1, 4})
    {
        run("setAttr", {{"args", {first + ".segments", segments}}});
        CHECK(sameEdge(pickedEdge(second), b0, b1));
    }
    run("setAttr", {{"args", {first + ".offset", 12.0}}});
    CHECK(sameEdge(pickedEdge(second), b0, b1));

    // A direct edit under the stack compacts the base; the first modifier follows its edge to the new ids.
    run("selectComponents", {{"args", {cube}}, {"faces", {2}}});
    run("polyExtrude", {{"thickness", 10}});
    auto e = pickedEdge(first);
    CHECK(glm::length(e.first - e.second) == doctest::Approx(100.0));

    // Undo walks back one step at a time.
    run("undo");
    CHECK(sameEdge(pickedEdge(first), a0, a1));
}

TEST_CASE("Edit Edges shows the input, cancels, undoes and saves")
{
    run("newScene");
    std::string cube = run("polyCube")[0];
    std::string mod = run("autoBevel", {{"args", {cube}}, {"segments", 1}})[0];
    run("addModifier", {{"args", {cube}}, {"kind", "subdivision"}});
    int full = faces(cube);

    run("bevelEditEdges", {{"args", {mod}}});
    // This modifier and the subdivision after it pass through for display.
    CHECK(faces(cube) == 6);
    CHECK(scene::components::selected(scene::Graph::current().find(cube)->id(), scene::ComponentMode::Edge).size() == 12);
    CHECK_THROWS_AS(run("polyExtrude", {{"thickness", 1}}), CommandError);

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    auto path = (dir / "autobevel_edit.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["editing"] == true);
    CHECK(faces(cube) == 6);

    run("bevelCancelEdges", {{"args", {mod}}});
    CHECK(faces(cube) == full);
    run("undo");
    CHECK(faces(cube) == 6);
    run("redo");

    // Deselect two edges and apply: plain auto becomes auto with overrides.
    run("bevelEditEdges", {{"args", {mod}}});
    run("selectComponents", {{"args", {cube}}, {"edges", {0, 1}}, {"deselect", true}});
    json r = run("bevelApplyEdges", {{"args", {mod}}});
    CHECK(r["removed"] == 2);
    CHECK(run("getAttr", {{"args", {mod + ".mode"}}}) == 1);
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["count"] == 10);
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["editing"] == false);
    run("undo");
    CHECK(run("bevelEdgeInfo", {{"args", {mod}}})["editing"] == true);
    run("bevelCancelEdges", {{"args", {mod}}});
}

TEST_CASE("selectFoldEdges and measure")
{
    run("newScene");
    std::string cube = run("polyCube", {{"subdivisionsX", 2}})[0];
    // The four edges along x are split in two; the loop around the middle is flat.
    CHECK(run("selectFoldEdges", {{"args", {cube}}}) == 16);
    std::string mod = run("autoBevel", {{"args", {cube}}})[0];
    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    double thin = run("bevelMeasureThin", {{"args", {mod}}}).get<double>();
    CHECK(thin > 0.0);
    CHECK(run("getAttr", {{"args", {mod + ".thin"}}}).get<double>() == doctest::Approx(thin));
}
