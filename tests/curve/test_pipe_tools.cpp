#include <doctest/doctest.h>

#include "curve/Carve.h"
#include "curve/Channel.h"
#include "curve/CurveNodes.h"
#include "curve/Pipe.h"
#include "curve/Strap.h"
#include "curve/Sweep.h"
#include "curve/WorldMeshes.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

scene::MeshPtr meshNamed(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(name);
    REQUIRE(n);
    return n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

json points(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

curve::Spline line(std::initializer_list<D3> list)
{
    curve::Spline s;
    s.kind = curve::Kind::Poly;
    s.points = list;
    s.normalize();
    return s;
}

/** The uv0 range over the faces whose face int attr `name` is value (or every face with an empty name). */
struct UvBox
{
    glm::dvec2 lo{1e300}, hi{-1e300};
    bool empty() const { return lo.x > hi.x; }
};

UvBox uvBox(const geo::Mesh& m, std::string_view name = {}, int value = 0)
{
    UvBox b;
    auto uv = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    auto tag = name.empty() ? std::span<const int32_t>{} : m.faceAttrs().get<int32_t>(name);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || (!name.empty() && (f >= tag.size() || tag[f] != value)))
            continue;
        m.forEachFaceCorner(f, [&](geo::Index c) {
            b.lo = glm::min(b.lo, glm::dvec2(uv[c]));
            b.hi = glm::max(b.hi, glm::dvec2(uv[c]));
        });
    }
    return b;
}

bool inside(const UvBox& b, const curve::UvRect& r, double tol = 1e-4)
{
    return b.lo.x >= r.lo.x - tol && b.lo.y >= r.lo.y - tol && b.hi.x <= r.hi.x + tol && b.hi.y <= r.hi.y + tol;
}

std::filesystem::path scratch(const char* file) { return std::filesystem::temp_directory_path() / "lila_curve_tests" / file; }

void saveAndReopen(const char* file)
{
    const std::string path = scratch(file).string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
}

double attr(const std::string& node, const char* name)
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find(node);
    REQUIRE(n);
    return n->as<double>(n->type().attrIndex(name));
}

/** A flat open grid on y = 0, size cm square with cells x cells faces, centred on centre. */
geo::Mesh ground(double size, int cells, const D3& centre = D3(0.0))
{
    geo::Mesh m = geo::makePlane({size, size, cells, cells});
    for (D3& p : m.positions())
        p += centre;
    return m;
}

int boundaryEdges(const geo::Mesh& m)
{
    int n = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            ++n;
    return n;
}

int nonManifoldEdges(const geo::Mesh& m)
{
    int n = 0;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) > 2)
            ++n;
    return n;
}

double distToSegment(const D3& p, const D3& a, const D3& b)
{
    const D3 d = b - a;
    const double t = glm::dot(d, d) > 0.0 ? std::clamp(glm::dot(p - a, d) / glm::dot(d, d), 0.0, 1.0) : 0.0;
    return glm::distance(a + d * t, p);
}
} // namespace

TEST_CASE("sweep strip uvs: layout, rectangle, texel density and caps")
{
    curve::Path path = curve::pathFrom(line({D3(0), D3(0, 0, 300)}));
    curve::SweepOptions o;
    o.uv.layout = curve::UvLayout::Along;
    o.uv.rect = {{0.0, 0.25}, {1.0, 0.375}};
    o.uv.perCm = 0.01;
    o.uv.caps = {{0.5, 0.5}, {0.75, 0.75}};
    o.startCap = o.endCap = curve::CapMode::Grid;
    curve::SweepResult r = curve::sweep(curve::circleProfile(8, 10.0), path, o);
    // Body: V around fills the strip, U runs along at a hundredth per cm (3 over 300 cm).
    auto uv = r.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    UvBox body, caps;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
    {
        if (!r.mesh.faceValid(f))
            continue;
        const double ny = std::abs(r.mesh.faceNormal(f).z);
        UvBox& b = ny > 0.9 ? caps : body;
        r.mesh.forEachFaceCorner(f, [&](geo::Index c) {
            b.lo = glm::min(b.lo, glm::dvec2(uv[c]));
            b.hi = glm::max(b.hi, glm::dvec2(uv[c]));
        });
    }
    CHECK(body.lo.y == doctest::Approx(0.25));
    CHECK(body.hi.y == doctest::Approx(0.375));
    CHECK(body.lo.x == doctest::Approx(0.0));
    CHECK(body.hi.x == doctest::Approx(3.0));
    CHECK(inside(caps, o.uv.caps));
    CHECK(caps.hi.x - caps.lo.x > 0.2);

    // Around (the default) keeps U around and V fitted per part.
    curve::SweepOptions d;
    d.uv.rect = {{0.5, 0.0}, {0.75, 1.0}};
    r = curve::sweep(curve::circleProfile(8, 10.0), path, d);
    UvBox all = uvBox(r.mesh);
    CHECK(all.lo.x == doctest::Approx(0.5));
    CHECK(all.hi.x == doctest::Approx(0.75));
    CHECK(all.lo.y == doctest::Approx(0.0));
    CHECK(all.hi.y == doctest::Approx(1.0));
}

TEST_CASE("pipe strip uvs: body, inner wall, rims and parts land in their rectangles")
{
    curve::PipeOptions o;
    o.radius = 5.0;
    o.wall = 0.5;
    o.coupling = curve::CouplingKind::Flange;
    o.fasteners = curve::FastenerKind::BoltAndNut;
    o.splitEvery = 100.0;
    o.uv.layout = curve::UvLayout::Along;
    o.uv.rect = {{0.0, 0.5}, {1.0, 0.625}};
    o.uv.perCm = 0.005;
    o.uv.caps = {{0.0, 0.0}, {0.25, 0.25}};
    o.innerUv = {{0.0, 0.75}, {1.0, 0.875}};
    curve::PipeResult r = curve::makePipe(line({D3(0), D3(0, 0, 400)}), o);
    REQUIRE(r.mesh.numFaces() > 0);
    // Couplings sit outside the body's strip only on their ends; bolts the same.
    UvBox parts = uvBox(r.mesh, "pipePart", int(curve::PipePart::Coupling));
    CHECK(!parts.empty());
    CHECK(parts.lo.y >= -1e-4);
    CHECK(parts.hi.y <= 0.625 + 1e-4);

    auto uv = r.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    auto kind = r.mesh.faceAttrs().get<int32_t>("pipePart");
    int outerFaces = 0, innerFaces = 0, rimFaces = 0;
    double longest = 0.0;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
    {
        if (!r.mesh.faceValid(f) || kind[f] != int(curve::PipePart::Body))
            continue;
        UvBox b;
        r.mesh.forEachFaceCorner(f, [&](geo::Index c) {
            b.lo = glm::min(b.lo, glm::dvec2(uv[c]));
            b.hi = glm::max(b.hi, glm::dvec2(uv[c]));
        });
        const D3 c = r.mesh.faceCentroid(f), n = r.mesh.faceNormal(f);
        const double radial = glm::length(glm::dvec2(c.x, c.y));
        if (std::abs(n.z) > 0.9)
        {
            ++rimFaces;
            CHECK(inside(b, o.uv.caps));
        }
        else if (radial > 4.6)
        {
            ++outerFaces;
            CHECK(b.lo.y >= 0.5 - 1e-4);
            CHECK(b.hi.y <= 0.625 + 1e-4);
            longest = std::max(longest, b.hi.x);
        }
        else
        {
            ++innerFaces;
            CHECK(b.lo.y >= 0.75 - 1e-4);
            CHECK(b.hi.y <= 0.875 + 1e-4);
        }
    }
    CHECK(outerFaces > 0);
    CHECK(innerFaces > 0);
    CHECK(rimFaces > 0);
    // World-scale tiling restarts per segment: 100 cm at 0.005 per cm is half a tile.
    CHECK(longest == doctest::Approx(0.5).epsilon(1e-3));
}

TEST_CASE("strap strip uvs")
{
    curve::StrapOptions o;
    o.uv.layout = curve::UvLayout::Along;
    o.uv.rect = {{0.0, 0.125}, {1.0, 0.25}};
    o.uv.perCm = 0.01;
    curve::StrapResult r = curve::makeStrap(line({D3(0), D3(200, 0, 0)}), o);
    REQUIRE(r.mesh.numFaces() > 0);
    auto uv = r.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    double along = 0.0;
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f) && std::abs(r.mesh.faceNormal(f).x) < 0.5)
            r.mesh.forEachFaceCorner(f, [&](geo::Index c) {
                CHECK(uv[c].y >= 0.125f - 1e-4f);
                CHECK(uv[c].y <= 0.25f + 1e-4f);
                along = std::max(along, double(uv[c].x));
            });
    CHECK(along == doctest::Approx(2.0).epsilon(1e-3));
}

TEST_CASE("pipe, sweep and cable uv flags: live, saved and undone")
{
    run("newScene");
    std::string path = run("curve", {{"type", "poly"}, {"points", points({D3(0), D3(0, 0, 300)})}, {"name", "Run"}});
    json r = run("pipe", {{"args", {path}}, {"uvLayout", "along"}, {"vMin", 0.5}, {"vMax", 0.625}, {"texelDensity", 0.01}});
    std::string mesh = r[0], node = r[1];
    UvBox b = uvBox(*meshNamed(mesh), "pipePart", 0);
    CHECK(b.lo.y == doctest::Approx(0.5));
    CHECK(b.hi.y == doctest::Approx(0.625));
    CHECK(b.hi.x == doctest::Approx(3.0).epsilon(1e-3));

    // A trim tool moves the pipe onto another strip by its attrs, keeping it live.
    run("setAttr", {{"args", {node + ".vMin", 0.0}}});
    run("setAttr", {{"args", {node + ".vMax", 0.125}}});
    CHECK(uvBox(*meshNamed(mesh), "pipePart", 0).hi.y == doctest::Approx(0.125));
    run("undo");
    run("undo");
    CHECK(uvBox(*meshNamed(mesh), "pipePart", 0).hi.y == doctest::Approx(0.625));

    saveAndReopen("pipe_uv.lila");
    CHECK(attr(node, "vMin") == doctest::Approx(0.5));
    CHECK(attr(node, "texelDensity") == doctest::Approx(0.01));
    CHECK(uvBox(*meshNamed(mesh), "pipePart", 0).hi.x == doctest::Approx(3.0).epsilon(1e-3));

    // curveSweep takes the same flags.
    json sw = run("extrude", {{"args", {path}}, {"profileShape", "circle"}, {"uvLayout", "along"}, {"uMin", 0.25}, {"uMax", 0.75},
                                 {"vMin", 0.0}, {"vMax", 0.25}});
    UvBox sb = uvBox(*meshNamed(sw[0].get<std::string>()));
    CHECK(sb.lo.x == doctest::Approx(0.25));
    CHECK(sb.hi.x == doctest::Approx(0.75));
    CHECK(sb.hi.y == doctest::Approx(0.25));

    // cable forwards pipe flags.
    json c = run("cable", {{"points", points({D3(0, 300, 0), D3(400, 300, 0)})}, {"pipe", true}, {"pipeOptions", {{"uvLayout", "along"}, {"vMin", 0.875}, {"vMax", 1.0}, {"startCap", "none"}, {"endCap", "none"}}}});
    UvBox cb = uvBox(*meshNamed(c[0][3].get<std::string>()), "pipePart", 0);
    CHECK(cb.lo.y == doctest::Approx(0.875));
}

TEST_CASE("uv edits on a live pipe bake it first and are kept")
{
    run("newScene");
    std::string path = run("curve", {{"type", "poly"}, {"points", points({D3(0), D3(0, 0, 200)})}, {"name", "Run"}});
    json r = run("pipe", {{"args", {path}}, {"uvLayout", "along"}});
    std::string mesh = r[0], node = r[1];
    const UvBox before = uvBox(*meshNamed(mesh));
    run("selectComponents", {{"args", {mesh}}, {"faces", {0, 1, 2}}});
    run("uvTransform", {{"translate", {2.0, 0.0}}});
    CHECK(!run("objExists", {{"args", {node}}}).get<bool>());
    const UvBox after = uvBox(*meshNamed(mesh));
    CHECK(after.hi.x == doctest::Approx(before.hi.x + 2.0).epsilon(1e-4));

    saveAndReopen("pipe_uv_edit.lila");
    CHECK(uvBox(*meshNamed(mesh)).hi.x == doctest::Approx(after.hi.x).epsilon(1e-4));

    // Undo puts the live pipe back with its own uvs.
    run("newScene");
    r = run("pipe", {{"args", {run("curve", {{"type", "poly"}, {"points", points({D3(0), D3(0, 0, 200)})}})}}});
    mesh = r[0].get<std::string>();
    node = r[1].get<std::string>();
    run("selectComponents", {{"args", {mesh}}, {"faces", {0}}});
    run("uvTransform", {{"translate", {2.0, 0.0}}});
    run("undo");
    CHECK(run("objExists", {{"args", {node}}}).get<bool>());
    CHECK(uvBox(*meshNamed(mesh)).hi.x <= 1.0 + 1e-4);
}

TEST_CASE("channel sections")
{
    for (curve::ChannelShape s : {curve::ChannelShape::UChannel, curve::ChannelShape::VGroove, curve::ChannelShape::HalfRound, curve::ChannelShape::Stepped})
    {
        curve::ChannelOptions o;
        o.shape = s;
        o.width = 300.0;
        o.depth = 120.0;
        std::vector<glm::dvec2> sec = curve::channelSection(o);
        INFO(curve::channelShapeName(s));
        REQUIRE(sec.size() >= 3);
        CHECK(sec.front() == glm::dvec2(-150.0, 0.0));
        CHECK(sec.back() == glm::dvec2(150.0, 0.0));
        double low = 0.0;
        for (const glm::dvec2& p : sec)
        {
            low = std::min(low, p.y);
            CHECK(std::abs(p.x) <= 150.0 + 1e-9);
        }
        CHECK(low == doctest::Approx(-120.0));
        geo::Mesh cutter = curve::channelCutter(line({D3(0), D3(1000, 0, 0)}), o);
        CHECK(boundaryEdges(cutter) == 0);
        CHECK(nonManifoldEdges(cutter) == 0);
    }
}

TEST_CASE("carve a channel into an open surface")
{
    curve::ChannelOptions o;
    o.width = 200.0;
    o.depth = 80.0;
    geo::Mesh surface = ground(2000.0, 20);
    geo::Mesh cutter = curve::channelCutter(line({D3(-500, 0, 0), D3(500, 0, 300)}), o);
    curve::CarveResult r = curve::carve(surface, glm::dmat4(1.0), cutter);
    REQUIRE(r.ok());
    CHECK(r.patchFaces > 0);
    CHECK(r.unwelded == 0);
    CHECK(nonManifoldEdges(r.mesh) == 0);
    // The outer border is all that stays open: 4 sides of 20 edges.
    CHECK(boundaryEdges(r.mesh) == 80);
    CHECK(r.mesh.bounds().min.y == doctest::Approx(-80.0).epsilon(1e-6));
    CHECK(r.mesh.bounds().max.y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(r.mesh.validate().empty());
    // The channel walls carry the strip: V across in 0..1.
    auto uv = r.mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
        if (r.mesh.faceValid(f) && r.mesh.faceCentroid(f).y < -1.0)
            r.mesh.forEachFaceCorner(f, [&](geo::Index c) {
                CHECK(uv[c].y >= -1e-4f);
                CHECK(uv[c].y <= 1.0f + 1e-4f);
            });
}

TEST_CASE("carve at kilometre scale far from the origin keeps the rest of the surface exact")
{
    // 4 km of terrain 30 km from the origin, in 100 m faces, with a 3 m wide trench 1.2 km long.
    const D3 far(3.0e6, 1500.0, -2.0e6);
    geo::Mesh surface = ground(400000.0, 40, far);
    curve::ChannelOptions o;
    o.width = 300.0;
    o.depth = 150.0;
    o.shape = curve::ChannelShape::Stepped;
    geo::Mesh cutter = curve::channelCutter(line({far + D3(-60000, 0, 2500), far + D3(60000, 0, 6500)}), o);
    const auto t0 = std::chrono::steady_clock::now();
    curve::CarveResult r = curve::carve(surface, glm::dmat4(1.0), cutter);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.ok());
    CHECK(seconds < 10.0);
    CHECK(r.unwelded == 0);
    CHECK(nonManifoldEdges(r.mesh) == 0);
    CHECK(boundaryEdges(r.mesh) == 160);
    CHECK(r.mesh.bounds().min.y == doctest::Approx(far.y - 150.0).epsilon(1e-9));
    // Points away from the cut are the surface's own, bit for bit.
    int exact = 0;
    for (const D3& p : surface.positions())
        for (const D3& q : r.mesh.positions())
            if (p == q)
            {
                ++exact;
                break;
            }
    CHECK(exact == int(surface.numPoints()));

    // The same carve placed by a matrix gives the same cut.
    const glm::dmat4 m = glm::translate(glm::dmat4(1.0), far);
    curve::CarveResult moved = curve::carve(ground(400000.0, 40), m, cutter);
    REQUIRE(moved.ok());
    CHECK(moved.mesh.numFaces() == r.mesh.numFaces());
}

TEST_CASE("carveChannel command: live, saved and undone")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 3000}, {"depth", 3000}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0];
    const size_t faces0 = meshNamed(plane)->numFaces();
    json r = run("carveChannel", {{"surface", plane},
                                  {"points", points({D3(-1000, 200, 0), D3(1000, 200, 500)})},
                                  {"shape", "vGroove"},
                                  {"width", 150.0},
                                  {"depth", 60.0}});
    std::string mod = r[1], curveName = r[2];
    scene::MeshPtr m = meshNamed(plane);
    CHECK(m->numFaces() > faces0);
    // followSurface drops the path (drawn 2 m up) onto the plane.
    CHECK(m->bounds().min.y == doctest::Approx(-60.0).epsilon(1e-6));

    run("setAttr", {{"args", {mod + ".depth", 100.0}}});
    CHECK(meshNamed(plane)->bounds().min.y == doctest::Approx(-100.0).epsilon(1e-6));
    run("move", {{"args", {curveName}}, {"by", {0, 0, 400}}});
    const size_t movedFaces = meshNamed(plane)->numFaces();
    CHECK(movedFaces > faces0);

    saveAndReopen("channel.lila");
    CHECK(meshNamed(plane)->numFaces() == movedFaces);
    CHECK(attr(mod, "depth") == doctest::Approx(100.0));

    run("newScene");
    plane = run("polyPlane", {{"width", 3000}, {"depth", 3000}})[0].get<std::string>();
    run("carveChannel", {{"surface", plane}, {"points", points({D3(-500, 0, 0), D3(500, 0, 0)})}});
    CHECK(meshNamed(plane)->numFaces() > 1);
    run("undo");
    CHECK(meshNamed(plane)->numFaces() == 1);
}
