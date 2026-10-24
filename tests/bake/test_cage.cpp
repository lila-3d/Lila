#include <doctest/doctest.h>

#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/CageEstimate.h"
#include "bake/CagePreview.h"
#include "core/MainQueue.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <chrono>
#include <filesystem>
#include <set>
#include <thread>

using namespace lila;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
scene::Graph& graph() { return scene::Graph::current(); }

void newScene()
{
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
}

std::string addMesh(const std::string& name, geo::Mesh m)
{
    scene::Graph& g = graph();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    return n->path();
}

scene::MeshPtr baseMesh(const std::string& name)
{
    scene::Graph& g = graph();
    return std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, g.find(name)->id())));
}

/** A project with one group: low and high meshes, output into a temp folder. */
std::string pairGroup(const std::string& low, const std::string& high, const fs::path& folder)
{
    const std::string p = ops::execute("createBakeProject", {{"name", "P"}}).get<std::string>();
    ops::execute("setBakeOutput", {{"project", p}, {"folder", folder.generic_string()}, {"resolution", 64}, {"supersampling", 1}});
    ops::execute("bakeGroup", {{"args", {"pair"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}});
    return "pair";
}

fs::path tempDir(const char* name)
{
    fs::path d = fs::temp_directory_path() / ("lila_cage_" + std::string(name));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}
} // namespace

TEST_CASE("UV-space dabs reach the points whose corners lie under the brush")
{
    geo::PlaneOptions o;
    o.subdivX = o.subdivZ = 4;
    auto plane = std::make_shared<const geo::Mesh>(geo::makePlane(o));
    bake::PointPaint paint(plane, *bake::paintAttr("offset"));
    std::span<const glm::vec2> uv = plane->cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());

    const glm::dvec2 center(0.5, 0.5);
    const double radius = 0.3;
    std::set<geo::Index> expected;
    for (geo::Index c = 0; c < plane->cornerCapacity(); ++c)
        if (plane->cornerValid(c) && glm::length(glm::dvec2(uv[c]) - center) <= radius)
            expected.insert(plane->cornerPoint(c));
    const auto pts = paint.uvPoints(center, radius);
    std::set<geo::Index> got;
    for (auto [p, w] : pts)
        if (w > 0.0f)
            got.insert(p);
    CHECK(got == expected);
    // Full weight at the centre point, none at the rim.
    for (auto [p, w] : pts)
        CHECK(w <= 1.0f);

    // A dab inside one face larger than the brush still reaches that face's corners (weight by distance, may be 0).
    geo::PlaneOptions big;
    auto quad = std::make_shared<const geo::Mesh>(geo::makePlane(big));
    bake::PointPaint one(quad, *bake::paintAttr("offset"));
    one.setFalloff(bake::PaintFalloff::Constant);
    CHECK(one.uvPoints({0.5, 0.5}, 0.05).size() == 4);
    CHECK(one.uvPoints({5.0, 5.0}, 0.05).empty());
}

TEST_CASE("UV dabs keep points split across islands consistent: one application per point")
{
    auto cube = std::make_shared<const geo::Mesh>(geo::makeCube());
    bake::PointPaint paint(cube, *bake::paintAttr("offset"));
    paint.setFalloff(bake::PaintFalloff::Constant);
    // Every cube point has corners in several islands; a dab over the whole layout reaches each point once.
    const auto pts = paint.uvPoints({0.5, 0.5}, 2.0);
    CHECK(pts.size() == cube->numPoints());
    std::set<geo::Index> unique;
    for (auto [p, w] : pts)
        unique.insert(p);
    CHECK(unique.size() == pts.size());
    paint.apply(pts, bake::PaintOp::Add, 1.0f, 1.0f);
    for (geo::Index p = 0; p < cube->pointCapacity(); ++p)
        if (cube->pointValid(p))
            CHECK(paint.values()[p] == doctest::Approx(2.0f));

    // A small dab on one island's corner still moves the point every island shows it at.
    std::span<const glm::vec2> uv = cube->cornerAttrs().get<glm::vec2>("uv0");
    const geo::Index c0 = cube->faceCorner(0);
    const geo::Index p0 = cube->cornerPoint(c0);
    bake::PointPaint small(cube, *bake::paintAttr("offset"));
    small.apply(small.uvPoints(glm::dvec2(uv[c0]), 0.01), bake::PaintOp::Set, 3.0f, 1.0f);
    CHECK(small.values()[p0] == doctest::Approx(3.0f));
    size_t corners = 0;
    std::set<std::pair<float, float>> places;
    cube->forEachPointCorner(p0, [&](geo::Index c) {
        ++corners;
        places.insert({uv[c].x, uv[c].y});
    });
    CHECK(places.size() > 1); // the point really is split across islands
}

TEST_CASE("bakeEstimateCage: one click gives a near-zero-miss cage on known pairs")
{
    struct Pair
    {
        const char* name;
        geo::Mesh high;
    };
    geo::SphereOptions so;
    so.radius = 75.0;
    so.subdivAxis = 64;
    so.subdivHeight = 32;
    geo::CubeOptions crate;
    crate.width = crate.height = crate.depth = 104.0;
    crate.subdivX = crate.subdivY = crate.subdivZ = 6;
    geo::Mesh lumpy = geo::makeCube(crate);
    // Panels pushed in and out across the faces, like a crate's planks.
    for (geo::Index p = 0; p < lumpy.pointCapacity(); ++p)
        if (lumpy.pointValid(p))
        {
            glm::dvec3 q = lumpy.position(p);
            q *= 1.0 + 0.04 * std::sin(q.x * 0.11) * std::cos(q.z * 0.13) - 0.03 * std::sin(q.y * 0.17);
            lumpy.setPosition(p, q);
        }
    std::vector<Pair> pairs;
    pairs.push_back({"sphere over cube", geo::makeSphere(so)});
    pairs.push_back({"lumpy crate", std::move(lumpy)});

    for (Pair& pr : pairs)
    {
        const std::string name = pr.name;
        CAPTURE(name);
        newScene();
        geo::CubeOptions lo;
        lo.subdivX = lo.subdivY = lo.subdivZ = 3;
        const std::string low = addMesh("box_low", geo::makeCube(lo));
        const std::string high = addMesh("box_high", std::move(pr.high));
        const std::string grp = pairGroup(low, high, tempDir("estimate"));
        // A hand-set cage of a few cm, as a group starts out before anyone tunes it.
        ops::execute("bakeGroup", {{"args", {grp}}, {"frontal", 3.0}, {"rear", 3.0}});

        const json before = ops::execute("bakeCageCoverage", {{"args", {grp}}, {"resolution", 256}});
        const json r = ops::execute("bakeEstimateCage", {{"args", {grp}}, {"resolution", 256}});
        MESSAGE(std::string(pr.name) << ": miss rate before " << r["before"]["missRate"].get<double>() * 100.0 << "%, after "
                        << r["after"]["missRate"].get<double>() * 100.0 << "% (" << r["after"]["misses"] << " of " << r["after"]["samples"]
                        << "), frontal " << r["frontal"] << " rear " << r["rear"] << ", passes " << r["iterations"]);
        CHECK(before["misses"].get<uint64_t>() == r["before"]["misses"].get<uint64_t>());
        CHECK(r["before"]["misses"].get<uint64_t>() > 0);
        CHECK(r["after"]["missRate"].get<double>() < 0.0005);

        // Applied: the scene now bakes with it, and it is one undo step.
        const json after = ops::execute("bakeCageCoverage", {{"args", {grp}}, {"resolution", 256}});
        CHECK(after["misses"] == r["after"]["misses"]);
        const bake::GroupSettings s = bake::groupSettings(*graph().find(grp));
        CHECK(s.frontal == doctest::Approx(r["frontal"].get<double>()));
        CHECK(baseMesh(low)->pointAttrs().has(bake::kOffsetAttr));
        UndoStack::get().undo();
        CHECK(!baseMesh(low)->pointAttrs().has(bake::kOffsetAttr));
        CHECK(bake::groupSettings(*graph().find(grp)).frontal == doctest::Approx(3.0));
    }
}

TEST_CASE("cage previews bake in memory off the main thread and write nothing")
{
    newScene();
    geo::SphereOptions so;
    so.radius = 70.0;
    const std::string low = addMesh("ball_low", geo::makeCube());
    const std::string high = addMesh("ball_high", geo::makeSphere(so));
    const fs::path folder = tempDir("preview");
    const std::string grp = pairGroup(low, high, folder);
    const scene::NodeId gid = graph().find(grp)->id();

    int heard = 0;
    const int id = bake::addCagePreviewListener([&](const std::shared_ptr<const bake::CagePreview>&) { ++heard; });
    bake::CagePreviewOptions o;
    o.resolution = 64;
    o.backend = "cpu";
    bake::requestCagePreview(graph(), gid, o);
    // A second request supersedes the first.
    std::vector<float> big(baseMesh(low)->pointCapacity(), 5.0f);
    o.offsets[low] = big;
    bake::requestCagePreview(graph(), gid, o);
    for (int i = 0; i < 600 && bake::cagePreviewBusy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    mainq::drain();
    bake::removeCagePreviewListener(id);
    CHECK(heard >= 1);
    const auto pv = bake::latestCagePreview(gid);
    REQUIRE(pv);
    CHECK(pv->generation >= 2);
    const bake::CagePreviewTexture* n = pv->find("ball_low", "normal");
    const bake::CagePreviewTexture* e = pv->find("ball_low", "error");
    REQUIRE(n);
    REQUIRE(e);
    CHECK(n->width == 64);
    CHECK(n->value.size() == 64 * 64);
    CHECK(pv->stats.samples > 0);
    CHECK(fs::is_empty(folder));
}
