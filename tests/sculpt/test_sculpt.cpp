#include "ops/Command.h"
#include "ops/Undo.h"
#include "geo/Bvh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/Reproject.h"
#include "sculpt/Sculpt.h"
#include "sculpt/SculptSession.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

using namespace lila;
using geo::Index;

namespace
{
scene::Node* makeMeshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    if (scene::Node* old = g.find(name))
        g.deleteNode(old->id());
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

const geo::Mesh& inMesh(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    return *std::get<scene::MeshPtr>(g.storedValue({g.find(name)->id(), scene::MeshNode::kInMesh, -1}));
}

double maxDiff(const geo::Mesh& a, const geo::Mesh& b)
{
    double d = 0.0;
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (a.pointValid(p))
            d = std::max(d, glm::length(a.position(p) - b.position(p)));
    return d;
}

/** Largest gap between a point and the reflection of its partner across x = 0. */
double asymmetry(const sculpt::SculptMesh& s)
{
    const geo::SymmetryMap& map = s.symmetry();
    double worst = 0.0;
    for (Index p = 0; p < s.mesh().pointCapacity(); ++p)
    {
        Index q = map.mirrorPoint(p);
        if (q == geo::kInvalid)
            continue;
        worst = std::max(worst, glm::length(s.mesh().position(q) - map.plane().reflect(s.mesh().position(p))));
    }
    return worst;
}

geo::Mesh sphere(int subdiv, double radius = 50.0)
{
    geo::SphereOptions o;
    o.radius = radius;
    o.subdivAxis = subdiv;
    o.subdivHeight = subdiv;
    return geo::makeSphere(o);
}

sculpt::Dab dabAt(const sculpt::SculptMesh& s, glm::dvec3 from, double radius)
{
    // Aimed at the origin from outside, like a click on the surface.
    geo::BvhHit hit;
    glm::dvec3 dir = -glm::normalize(from);
    REQUIRE(s.raycast(from, dir, hit));
    sculpt::Dab d;
    d.center = hit.point;
    d.normal = s.normalAt(hit);
    d.radius = radius;
    d.viewDir = dir;
    return d;
}
} // namespace

TEST_CASE("subdivision stencils reproduce subdivide")
{
    geo::Mesh cube = geo::makeCube({});
    // A creased edge and an open border exercise the sharp rules.
    auto crease = cube.edgeAttrs().getOrAdd<float>(geo::attr::kCrease);
    crease[0] = 1.0f;
    crease[3] = 0.5f;
    geo::deleteFaces(cube, std::vector<Index>{0});
    for (bool sharp : {true, false})
    {
        geo::SubdivideOptions o;
        o.levels = 3;
        o.sharpBoundary = sharp;
        geo::Mesh sub;
        auto stencils = geo::subdivideStencils(cube, o, &sub);
        REQUIRE(stencils.size() == 3);
        std::vector<glm::dvec3> pts(cube.positions().begin(), cube.positions().end());
        for (const auto& s : stencils)
            pts = s.apply(pts);
        geo::Mesh direct = geo::subdivide(cube, o);
        REQUIRE(pts.size() == direct.pointCapacity());
        REQUIRE(sub.pointCapacity() == direct.pointCapacity());
        double err = 0.0;
        for (Index p = 0; p < direct.pointCapacity(); ++p)
            err = std::max(err, glm::length(pts[p] - direct.position(p)));
        CHECK(err < 1e-12);
    }
}

TEST_CASE("reprojection round trip leaves an untouched cage unchanged")
{
    geo::Mesh cube = geo::makeCube({});
    for (int levels : {1, 2, 3})
    {
        geo::SubdivideOptions o;
        o.levels = levels;
        geo::Mesh sub;
        auto stencils = geo::subdivideStencils(cube, o, &sub);
        std::vector<glm::dvec3> x = sculpt::fitCage(cube, stencils, sub.positions());
        double err = 0.0;
        for (Index p = 0; p < cube.pointCapacity(); ++p)
            err = std::max(err, glm::length(x[p] - cube.position(p)));
        CHECK(err < 1e-9);
    }

    // The same through the commands, with symmetry on, levels 2.
    scene::Node* n = makeMeshNode("rtCube", geo::makeCube({}));
    scene::Graph::current().setValue({n->id(), scene::MeshNode::kSymmetry, -1}, int64_t(1));
    geo::Mesh before = inMesh("rtCube");
    ops::execute("sculptEnter", {{"args", {"rtCube"}}, {"levels", 2}});
    REQUIRE(sculpt::active());
    CHECK(sculpt::active()->sculpt.mesh().numFaces() == 6 * 16);
    ops::execute("sculptExit", json::object());
    CHECK_FALSE(sculpt::active());
    CHECK(maxDiff(before, inMesh("rtCube")) < 1e-9);
}

TEST_CASE("exit fit recovers a displaced cage")
{
    // Sculpting exactly the subdivision of a moved cage must give that cage back.
    geo::Mesh cage = sphere(8);
    geo::Mesh moved = cage;
    for (Index p = 0; p < moved.pointCapacity(); ++p)
        moved.setPosition(p, moved.position(p) * glm::dvec3(1.2, 0.9, 1.0) + glm::dvec3(0.0, std::sin(double(p)) * 2.0, 0.0));
    geo::SubdivideOptions o;
    o.levels = 2;
    auto stencils = geo::subdivideStencils(cage, o);
    std::vector<glm::dvec3> target(moved.positions().begin(), moved.positions().end());
    for (const auto& s : stencils)
        target = s.apply(target);
    std::vector<glm::dvec3> x = sculpt::fitCage(cage, stencils, target);
    double err = 0.0;
    for (Index p = 0; p < cage.pointCapacity(); ++p)
        err = std::max(err, glm::length(x[p] - moved.position(p)));
    // The regularization's pull toward the starting cage is all that is left.
    CHECK(err < 1e-4);
}

TEST_CASE("relax brush keeps points on the original surface")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh proxy = geo::subdivide(sphere(10), o);
    // Uneven spacing to relax.
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
    {
        glm::dvec3 at = proxy.position(p);
        proxy.setPosition(p, glm::normalize(at + glm::dvec3(std::sin(at.y * 0.3) * 4.0, 0.0, 0.0)) * glm::length(at));
    }
    geo::MeshBvh original(proxy);
    sculpt::SculptMesh s(proxy);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Relax;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 20; ++i)
        s.dab(dabAt(s, glm::dvec3(200.0, 10.0 * std::sin(i * 0.3), 0.0), 30.0));
    sculpt::StrokeRecord r = s.endStroke();
    REQUIRE(r.points.size() > 50);
    double moved = 0.0, off = 0.0;
    for (size_t i = 0; i < r.points.size(); ++i)
    {
        moved = std::max(moved, glm::length(r.after[i] - r.before[i]));
        geo::BvhClosest c;
        REQUIRE(original.closestPoint(r.after[i], c));
        off = std::max(off, c.distance);
    }
    CHECK(moved > 0.1);
    CHECK(off < 1e-9);

    // The standalone command: relaxMesh keeps the form too.
    makeMeshNode("relaxed", proxy);
    ops::execute("relaxMesh", {{"args", {"relaxed"}}, {"iterations", 10}, {"pinSeams", true}});
    const geo::Mesh& out = inMesh("relaxed");
    double cmdOff = 0.0, cmdMoved = 0.0;
    for (Index p = 0; p < out.pointCapacity(); ++p)
    {
        geo::BvhClosest c;
        original.closestPoint(out.position(p), c);
        cmdOff = std::max(cmdOff, c.distance);
        cmdMoved = std::max(cmdMoved, glm::length(out.position(p) - proxy.position(p)));
    }
    CHECK(cmdMoved > 0.1);
    CHECK(cmdOff < 1e-6);
}

TEST_CASE("symmetric strokes stay symmetric")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh proxy = geo::subdivide(geo::makeCube({}), o);
    sculpt::SculptMesh s(proxy);
    s.setSymmetry(geo::axisPlane(0), 1e-4);
    REQUIRE(s.symmetric());
    CHECK(asymmetry(s) == 0.0);
    for (sculpt::Brush brush : {sculpt::Brush::Draw, sculpt::Brush::ClayStrips, sculpt::Brush::Inflate, sculpt::Brush::Smooth,
                                sculpt::Brush::Flatten, sculpt::Brush::Pinch, sculpt::Brush::Crease, sculpt::Brush::Scrape,
                                sculpt::Brush::Relax, sculpt::Brush::Grab})
    {
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 0.8;
        s.beginStroke(b);
        // Off centre and crossing the plane.
        for (int i = 0; i < 12; ++i)
        {
            sculpt::Dab d = dabAt(s, glm::dvec3(-30.0 + i * 5.0, 20.0, 200.0), 25.0);
            d.delta = glm::dvec3(1.0, 0.5, 0.0);
            d.direction = glm::dvec3(1.0, 0.0, 0.0);
            s.dab(d);
        }
        sculpt::StrokeRecord r = s.endStroke();
        CHECK_MESSAGE(!r.empty(), sculpt::brushName(brush));
        CHECK_MESSAGE(asymmetry(s) == 0.0, sculpt::brushName(brush));
    }
}

TEST_CASE("mask and freeze are respected")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh proxy = geo::subdivide(geo::makeCube({}), o);
    sculpt::SculptMesh s(proxy);
    std::vector<float> mask(proxy.pointCapacity(), 0.0f);
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        mask[p] = proxy.position(p).x < 0.0 ? 1.0f : 0.0f;
    s.setMask(mask);
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Draw;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 10; ++i)
        s.dab(dabAt(s, glm::dvec3(-20.0 + i * 4.0, 0.0, 200.0), 40.0));
    sculpt::StrokeRecord r = s.endStroke();
    REQUIRE(!r.empty());
    bool movedUnmasked = false;
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
    {
        double d = glm::length(s.mesh().position(p) - proxy.position(p));
        if (mask[p] > 0.0f)
            CHECK(d == 0.0);
        else
            movedUnmasked |= d > 1e-6;
    }
    CHECK(movedUnmasked);

    // Freeze brush, then a stroke over it: frozen points stay.
    sculpt::BrushSettings f;
    f.brush = sculpt::Brush::Freeze;
    s.beginStroke(f);
    s.dab(dabAt(s, glm::dvec3(30.0, 0.0, 200.0), 15.0));
    s.endStroke();
    std::vector<glm::dvec3> held(s.mesh().positions().begin(), s.mesh().positions().end());
    std::vector<uint8_t> frozen(s.frozen().begin(), s.frozen().end());
    REQUIRE(std::count(frozen.begin(), frozen.end(), 1) > 0);
    b.brush = sculpt::Brush::Inflate;
    s.beginStroke(b);
    s.dab(dabAt(s, glm::dvec3(30.0, 0.0, 200.0), 40.0));
    s.endStroke();
    for (Index p = 0; p < proxy.pointCapacity(); ++p)
        if (frozen[p])
            CHECK(s.mesh().position(p) == held[p]);

    // Mask ops and undo of them.
    sculpt::StrokeRecord inv = s.maskOp(sculpt::MaskOp::Invert);
    CHECK(!inv.empty());
    CHECK(s.mask()[0] == 1.0f - mask[0]);
    s.apply(inv, true);
    CHECK(s.mask()[0] == mask[0]);
}

TEST_CASE("sculpt commands undo as single steps")
{
    scene::Node* n = makeMeshNode("undoCube", geo::makeCube({}));
    (void)n;
    geo::Mesh before = inMesh("undoCube");
    UndoStack& u = UndoStack::get();
    ops::execute("sculptEnter", {{"args", {"undoCube"}}, {"levels", 2}});
    auto s = sculpt::active();
    REQUIRE(s);
    std::vector<glm::dvec3> start(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end());
    json dab = {{"center", {0.0, 0.0, 50.0}}, {"normal", {0.0, 0.0, 1.0}}, {"radius", 40.0}};
    json r = ops::execute("sculptStroke", {{"brush", "draw"}, {"strength", 1.0}, {"dabs", json::array({dab, dab, dab})}});
    CHECK(r["points"].get<int>() > 0);
    std::vector<glm::dvec3> sculpted(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end());
    CHECK(sculpted != start);
    ops::execute("sculptExit", {{"keepProxy", true}});
    CHECK_FALSE(sculpt::active());
    CHECK(maxDiff(before, inMesh("undoCube")) > 0.1);
    CHECK(scene::Graph::current().find("undoCube_sculptProxy"));

    // Undo exit: back in sculpt mode with the sculpt intact, cage restored.
    REQUIRE(u.undo());
    REQUIRE(sculpt::active() == s);
    CHECK(maxDiff(before, inMesh("undoCube")) == 0.0);
    CHECK(std::vector<glm::dvec3>(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()) == sculpted);
    CHECK_FALSE(scene::Graph::current().find("undoCube_sculptProxy"));
    // Undo the stroke, then the enter.
    REQUIRE(u.undo());
    CHECK(std::vector<glm::dvec3>(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()) == start);
    REQUIRE(u.undo());
    CHECK_FALSE(sculpt::active());
    // Redo all three.
    REQUIRE(u.redo());
    CHECK(sculpt::active() == s);
    REQUIRE(u.redo());
    CHECK(std::vector<glm::dvec3>(s->sculpt.mesh().positions().begin(), s->sculpt.mesh().positions().end()) == sculpted);
    REQUIRE(u.redo());
    CHECK_FALSE(sculpt::active());
    CHECK(maxDiff(before, inMesh("undoCube")) > 0.1);

    // Re-entering resumes from the kept proxy.
    ops::execute("sculptEnter", {{"args", {"undoCube"}}});
    REQUIRE(sculpt::active());
    CHECK(sculpt::active()->mode == sculpt::ProxyMode::Subdivide);
    CHECK(sculpt::active()->proxyNode != scene::kNoNode);
    ops::execute("sculptCancel", json::object());
    CHECK_FALSE(sculpt::active());
}

TEST_CASE("transferShape and shrinkwrap move a cage onto a high poly")
{
    geo::Mesh high = sphere(40, 60.0);
    makeMeshNode("high", high);
    makeMeshNode("low", geo::makeCube({}));
    geo::MeshBvh highBvh(high);
    ops::execute("transferShape", {{"args", {"high", "low"}}, {"levels", 2}});
    // The fitted cage's subdivision lies near the sphere.
    geo::SubdivideOptions o;
    o.levels = 2;
    geo::Mesh sub = geo::subdivide(inMesh("low"), o);
    double worst = 0.0;
    for (Index p = 0; p < sub.pointCapacity(); ++p)
        worst = std::max(worst, std::abs(glm::length(sub.position(p)) - 60.0));
    CHECK(worst < 6.0);

    makeMeshNode("wrapped", geo::makeCube({}));
    ops::execute("shrinkwrap", {{"args", {"wrapped"}}, {"target", "high"}, {"method", "normal"}, {"offset", 2.0}, {"iterations", 0}});
    const geo::Mesh& w = inMesh("wrapped");
    for (Index p = 0; p < w.pointCapacity(); ++p)
        CHECK(std::abs(glm::length(w.position(p)) - 62.0) < 0.5);
}

TEST_CASE("brush benchmark on a million-triangle proxy" * doctest::skip(std::getenv("LILA_SCULPT_BENCH") == nullptr))
{
    geo::SubdivideOptions o;
    o.levels = 3;
    auto t0 = std::chrono::steady_clock::now();
    geo::Mesh proxy = geo::subdivide(sphere(90), o);
    auto t1 = std::chrono::steady_clock::now();
    sculpt::SculptMesh s(std::move(proxy));
    s.setSymmetry(geo::axisPlane(0), 1e-4);
    auto render = s.renderMesh();
    auto t2 = std::chrono::steady_clock::now();
    const size_t tris = render->indices.size() / 3;
    MESSAGE("proxy: " << s.mesh().numPoints() << " points, " << tris << " triangles; subdivide "
                      << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, setup "
                      << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms");
    CHECK(tris >= 1000000);
    {
        std::scoped_lock lock(render->dynamic->mutex);
        render->dynamic->allDirty = false;
        render->dynamic->dirty.clear();
    }

    for (auto [brush, radius] : {std::pair{sculpt::Brush::Draw, 8.0}, std::pair{sculpt::Brush::Draw, 25.0}, std::pair{sculpt::Brush::ClayStrips, 8.0},
                                 std::pair{sculpt::Brush::Smooth, 8.0}, std::pair{sculpt::Brush::Relax, 8.0}, std::pair{sculpt::Brush::Grab, 25.0}})
    {
        sculpt::BrushSettings b;
        b.brush = brush;
        b.strength = 0.5;
        s.beginStroke(b);
        const int dabs = 100;
        double worst = 0.0;
        size_t dirtyMax = 0;
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < dabs; ++i)
        {
            auto d0 = std::chrono::steady_clock::now();
            sculpt::Dab d = dabAt(s, glm::dvec3(-30.0 + i * 0.6, 10.0, 200.0), radius);
            d.delta = glm::dvec3(0.05, 0.0, 0.0);
            s.dab(d);
            worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - d0).count());
            // What the renderer would copy for this frame.
            std::scoped_lock lock(render->dynamic->mutex);
            dirtyMax = std::max(dirtyMax, render->dynamic->dirty.size());
            render->dynamic->dirty.clear();
        }
        double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        sculpt::StrokeRecord r = s.endStroke();
        MESSAGE(sculpt::brushName(brush) << " r" << radius << ": " << total / dabs << " ms per dab (worst " << worst << "), " << r.points.size()
                                         << " points changed, at most " << dirtyMax << " of " << s.mesh().numPoints()
                                         << " vertices re-uploaded per dab");
        CHECK(total / dabs < 16.0);
        CHECK(dirtyMax < s.mesh().numPoints() / 20);
    }
}


TEST_CASE("partial refit matches a rebuilt bvh")
{
    geo::SubdivideOptions o;
    o.levels = 2;
    sculpt::SculptMesh s(geo::subdivide(sphere(12), o));
    sculpt::BrushSettings b;
    b.brush = sculpt::Brush::Draw;
    b.strength = 1.0;
    s.beginStroke(b);
    for (int i = 0; i < 30; ++i)
        s.dab(dabAt(s, glm::dvec3(-20.0 + i, 5.0, 200.0), 20.0));
    s.endStroke();
    // Same triangles: a fresh triangulation of the deformed quads could pick other diagonals.
    geo::MeshBvh fresh;
    fresh.build(s.mesh(), s.bvh().triangles());
    for (int i = 0; i < 200; ++i)
    {
        glm::dvec3 from(std::sin(i * 0.37) * 80.0, std::cos(i * 0.21) * 80.0, 200.0);
        glm::dvec3 dir = glm::normalize(glm::dvec3(0.0, 0.0, 0.0) - from + glm::dvec3(std::sin(i * 1.3) * 30.0, 0.0, 0.0));
        geo::BvhHit a, c;
        bool ha = s.bvh().raycast(from, dir, a), hc = fresh.raycast(from, dir, c);
        REQUIRE(ha == hc);
        if (ha)
            CHECK(std::abs(a.t - c.t) < 1e-9);
    }
}
