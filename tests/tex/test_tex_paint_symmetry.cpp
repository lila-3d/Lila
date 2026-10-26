#include "TexTest.h"

#include "core/MainQueue.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "render/PaintGpu.h"
#include "render/Renderer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "tex/PaintBuffer.h"
#include "tex/PaintStrokes.h"
#include "tex/PaintSymmetry.h"
#include "tex/TextureSet.h"

#include <cmath>

using namespace textest;
using namespace lila::render;
namespace gp = lila::render::paint;
namespace tp = lila::tex::paint;

namespace
{
bool gpu()
{
    Renderer& r = Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for texture painting tests");
    return false;
}

json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

scene::Graph& sg() { return scene::Graph::current(); }

/**
 * Two quads mirrored across x = 0, 100 units across, facing +Y. Each is its own UV island; flipped mirrors the right
 * island's u (mirrored UVs), otherwise both islands run u along +x (the mirrored texels sit elsewhere in uv).
 */
glm::vec2 panelUv(double x, double z, bool flipped)
{
    const float v = float(0.1 + (z + 50.0) / 100.0 * 0.8);
    if (x < 0.0)
        return {float(0.05 + (x + 50.0) / 50.0 * 0.4), v};
    return {float(flipped ? 0.95 - x / 50.0 * 0.4 : 0.55 + x / 50.0 * 0.4), v};
}

geo::Mesh panel(bool flipped)
{
    const std::vector<glm::dvec3> pts = {{-50, 0, -50}, {0, 0, -50}, {50, 0, -50}, {-50, 0, 50}, {0, 0, 50}, {50, 0, 50}};
    const std::vector<geo::Index> sizes = {4, 4};
    const std::vector<geo::Index> idx = {0, 3, 4, 1, 1, 4, 5, 2};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < 2; ++f)
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::dvec3 p = m.position(m.cornerPoint(c));
            // The shared edge's corners belong to their own face's island.
            const double x = p.x == 0.0 ? (f == 0 ? -1e-9 : 0.0) : p.x;
            uv[c] = panelUv(x, p.z, flipped);
        }
    return m;
}

scene::Node& setup(bool flipped, int res = 256)
{
    run("newScene");
    UndoStack::get().clear();
    mainq::drain();
    scene::Node* n = sg().createNode(*scene::types::find("mesh"), "PanelMesh");
    sg().setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(panel(flipped))));
    run("texCreateSet", {{"args", {"Panel"}}, {"resolution", res}});
    return *tex::findSet(sg(), "Panel");
}

const float* at(const image::Image& img, glm::vec2 uv)
{
    const int x = std::clamp(int(uv.x * img.width), 0, img.width - 1), y = std::clamp(int((1.0f - uv.y) * img.height), 0, img.height - 1);
    return &img.rgba[(size_t(y) * img.width + x) * 4];
}

json mirrorBrush()
{
    return {{"falloff", "smooth"}, {"strength", 1.0}, {"flow", 1.0}, {"spacing", 0.15}, {"mirror", true}};
}

json surfaceStroke(std::vector<glm::dvec3> points, double radius, json payload)
{
    json dabs = json::array();
    for (const glm::dvec3& p : points)
        dabs.push_back({{"position", {p.x, p.y, p.z}}, {"radius", radius}});
    return {{"space", "surface"}, {"brush", mirrorBrush()}, {"payload", std::move(payload)}, {"dabs", dabs}, {"mesh", "PanelMesh"}};
}

/** Largest difference of a channel component between texels mirrored across x = 0, over a grid. */
double mirrorDiff(const image::Image& img, int component, double x0, double x1, double z0, double z1, bool flipped, float* reached = nullptr)
{
    double worst = 0.0;
    float most = 0.0f;
    for (double x = x0; x <= x1; x += 1.0)
        for (double z = z0; z <= z1; z += 1.0)
        {
            const float a = at(img, panelUv(x, z, flipped))[component], b = at(img, panelUv(-x, z, flipped))[component];
            worst = std::max(worst, double(std::abs(a - b)));
            most = std::max(most, a);
        }
    if (reached)
        *reached = most;
    return worst;
}
} // namespace

TEST_CASE("texture paint symmetry: a mirrored stroke paints the mirrored texels with non-mirrored UVs")
{
    if (!gpu())
        return;
    scene::Node& set = setup(false);
    run("texPaintStroke", {{"args", {"Panel", "P"}},
                           {"stroke", surfaceStroke({{-30, 0, 5}, {-25, 0, 8}, {-20, 0, 12}}, 8.0, {{"channels", {{"roughness", 1.0}}}})}});
    const tp::PaintPtr data = tp::readPaint(set);
    const tp::Stroke& s = *data->buffer("P")->strokes.front();
    CHECK(s.dabs.size() == 6);
    CHECK(tp::PaintSymmetry::fromJson(s.symmetry).mirror.has_value());
    const image::Image img = gp::readChannel(set.id(), "P", "roughness");
    float reached = 0.0f;
    // Texel centres sit at slightly different spots on the two islands: allow the falloff's slope over a texel.
    CHECK(mirrorDiff(img, 3, -40, -10, -5, 25, false, &reached) < 0.12);
    CHECK(reached > 0.9f);
    // Replayed from the record: the same pixels.
    const auto bytes = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == bytes);

    // Fills copy their seed: the island under (-25, 0, 0) and its mirror.
    run("texPaintStroke", {{"args", {"Panel", "F"}},
                           {"stroke", {{"mesh", "PanelMesh"}, {"brush", mirrorBrush()},
                                       {"payload", {{"kind", "fill"}, {"fillBy", "island"}, {"at", {-25.0, 0.0, 0.0}}, {"channels", {{"metallic", 1.0}}}}}}}});
    const image::Image fill = gp::readChannel(set.id(), "F", "metallic");
    CHECK(at(fill, panelUv(-25, 0, false))[3] == 1.0f);
    CHECK(at(fill, panelUv(25, 0, false))[3] == 1.0f);
}

TEST_CASE("texture paint symmetry: a projection commit projects mirrored")
{
    if (!gpu())
        return;
    scene::Node& set = setup(false, 512);
    const json view = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 800}, {"height", 600}};
    tp::ProjectionBuffer buf;
    buf.resize(800, 600);
    buf.paint({{250.0f, 300.0f}, 50.0f, 1.0f, {}}, brush::FalloffTable(brush::Falloff::fromName("smooth")), {1.0f, 0.5f, 0.0f});
    const image::Image pic = buf.resolved();
    run("texPaintStroke", {{"args", {"Panel", "P"}},
                           {"view", view},
                           {"brush", mirrorBrush()},
                           {"payload", {{"kind", "project"}, {"image", tp::PaintImage::fromRgba(pic.width, pic.height, pic.rgba)->toJson()}}}});
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    // Screen x 250 is world x = (250 - 400) / 400 * half-width at the panel's depth.
    const double half = 200.0 * std::tan(glm::radians(20.0)) * 800.0 / 600.0;
    const double x = (250.0 - 400.0) / 400.0 * half;
    float reached = 0.0f;
    CHECK(mirrorDiff(img, 3, x - 10, x + 10, -10, 10, false, &reached) < 0.06);
    CHECK(reached > 0.9f);
    CHECK(at(img, panelUv(-x, 0, false))[0] > 0.9f);
    const auto bytes = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == bytes);
}

TEST_CASE("texture paint symmetry: a mirrored direction stroke tilts the mirrored normal")
{
    if (!gpu())
        return;
    for (bool flipped : {false, true})
    {
        CAPTURE(flipped);
        scene::Node& set = setup(flipped);
        // Along +x on the left: the copy runs along -x on the right.
        run("texPaintStroke", {{"args", {"Panel", "X"}},
                               {"stroke", surfaceStroke({{-40, 0, 20}, {-35, 0, 20}, {-30, 0, 20}, {-25, 0, 20}, {-20, 0, 20}}, 5.0,
                                                        {{"kind", "direction"}, {"tilt", 0.8}})}});
        const image::Image nx = gp::readChannel(set.id(), "X", "normal");
        const float* l = at(nx, panelUv(-30, 20, flipped));
        const float* r = at(nx, panelUv(30, 20, flipped));
        REQUIRE(l[3] > 0.5f);
        REQUIRE(r[3] > 0.5f);
        // Left: along +u. Right: the stroke runs against +x, which is -u on plain UVs and +u on mirrored ones.
        CHECK(l[0] > 0.6f);
        if (flipped)
            CHECK(r[0] > 0.6f);
        else
            CHECK(r[0] < 0.4f);
        CHECK(std::abs(l[1] - 0.5f) < 0.05f);
        CHECK(std::abs(r[1] - 0.5f) < 0.05f);
        CHECK(std::abs((l[0] - 0.5f) - (flipped ? r[0] - 0.5f : 0.5f - r[0])) < 0.05f);

        // Along +z: the copy runs along +z too, tilting along +v on both islands.
        run("texPaintStroke", {{"args", {"Panel", "Z"}},
                               {"stroke", surfaceStroke({{-30, 0, -20}, {-30, 0, -15}, {-30, 0, -10}, {-30, 0, -5}, {-30, 0, 0}}, 5.0,
                                                        {{"kind", "direction"}, {"tilt", 0.8}})}});
        const image::Image nz = gp::readChannel(set.id(), "Z", "normal");
        const float* lz = at(nz, panelUv(-30, -10, flipped));
        const float* rz = at(nz, panelUv(30, -10, flipped));
        REQUIRE(lz[3] > 0.5f);
        REQUIRE(rz[3] > 0.5f);
        CHECK(lz[1] > 0.6f);
        CHECK(std::abs(lz[1] - rz[1]) < 0.05f);
        CHECK(std::abs(lz[0] - 0.5f) < 0.05f);
        CHECK(std::abs(rz[0] - 0.5f) < 0.05f);
    }
}

TEST_CASE("texture paint symmetry: clones, particles and stamps copy with the stroke")
{
    if (!gpu())
        return;
    scene::Node& set = setup(false);
    json base = surfaceStroke({{-30, 0, -20}}, 8.0, {{"channels", {{"baseColor", {0.0, 0.0, 1.0}}}}});
    base["brush"]["mirror"] = false;
    run("texPaintStroke", {{"args", {"Panel", "P"}}, {"stroke", base}});
    // A mirrored clone from the blue spot: the right side clones from the spot's mirror, which is empty.
    json clone = surfaceStroke({{-30, 0, 20}}, 8.0, {{"kind", "clone"}, {"cloneFrom", {-30.0, 0.0, -20.0}}, {"channels", {{"baseColor", 0.0}}}});
    run("texPaintStroke", {{"args", {"Panel", "P"}}, {"stroke", clone}});
    const tp::PaintPtr data = tp::readPaint(set);
    const tp::Payload& cp = data->buffer("P")->strokes.back()->payload;
    REQUIRE(cp.sourceOffsets.size() == 2);
    CHECK(cp.sourceOffsets[0] != cp.sourceOffsets[1]);
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, panelUv(-30, 20, false))[2] > 0.5f);
    CHECK(at(img, panelUv(30, 20, false))[3] < 0.05f);

    // Particles: every particle dab has its mirror.
    json drips = surfaceStroke({{-30, 0, 0}}, 4.0, {{"channels", {{"roughness", 1.0}}}});
    drips["particles"] = {{"kind", "drips"}, {"rate", 1.0}, {"count", 2}, {"down", {0.0, 0.0, 1.0}}, {"seed", 2}};
    run("texPaintStroke", {{"args", {"Panel", "D"}}, {"stroke", drips}});
    const tp::Stroke& ds = *tp::readPaint(set)->buffer("D")->strokes.front();
    size_t original = 0, mirrored = 0;
    for (const tp::Dab& d : ds.dabs)
        (d.copy == 0 ? original : mirrored) += 1;
    CHECK(original > 3);
    CHECK(original == mirrored);
    const image::Image di = gp::readChannel(set.id(), "D", "roughness");
    float reached = 0.0f;
    CHECK(mirrorDiff(di, 3, -40, -20, -5, 30, false, &reached) < 0.08);
    CHECK(reached > 0.5f);
}

TEST_CASE("texture paint symmetry: a mesh's radial group copies through its orbits and round-trips its record")
{
    geo::Mesh disc = geo::makeDisc({.radius = 50.0, .subdivAxis = 12, .subdivRings = 2});
    geo::SymmetrySpec spec;
    spec.radial = geo::axisRadial(1, glm::dvec3(0.0), 6, true);
    lila::brush::BrushSettings b;
    b.mirror = true;
    b.radial.count = 4;
    tp::PaintSymmetry s = tp::brushSymmetry(b, &disc, spec);
    REQUIRE(s.group.has_value());
    CHECK(s.map != nullptr);
    CHECK(s.count() == 12u);
    for (uint32_t k = 0; k < s.count(); ++k)
    {
        CHECK(s.mirrored(k) == (k >= 6));
        const glm::dvec3 p(13.0, 2.0, 5.0);
        CHECK(glm::length(glm::dvec3(s.transform(k) * glm::dvec4(p, 1.0)) - spec.radial->apply(int(k), p)) < 1e-9);
    }
    const tp::PaintSymmetry back = tp::PaintSymmetry::fromJson(s.toJson());
    REQUIRE(back.group.has_value());
    CHECK(back.group->count() == 6);
    CHECK(back.group->mirror());
    CHECK(back.count() == 12u);

    // Without the brush's mirror switch the mesh's group stays off, and the brush's own radial copies remain.
    b.mirror = false;
    tp::PaintSymmetry plain = tp::brushSymmetry(b, &disc, spec);
    CHECK_FALSE(plain.group.has_value());
    CHECK(plain.count() == 4u);
}
