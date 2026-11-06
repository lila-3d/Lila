// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// The core's interactive tools (stroke.h) driven from screen points on a GroomSession, scripted
// strokes, edits, undo, and guide picking in a view (guide_selection.h).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "groomlab/brush.h"
#include "groomlab/curve.h"
#include "groomlab/guide_selection.h"
#include "groomlab/random.h"
#include "groomlab/scalp_maps.h"
#include "groomlab/stroke.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

TEST(Stroke, ToolColorsAreEveryHosts) {
    // The ring, the path and the hover lines every host draws (stroke.h): Maya's colors.
    using Color = std::array<float, 3>;
    BrushToolSettings s;
    s.setMode("comb");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.8f, 0.2f}));
    EXPECT_EQ(brushHoverColor(s), (Color{1.0f, 1.0f, 1.0f}));
    s.setMode("erase");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.3f, 0.3f}));
    EXPECT_EQ(brushHoverColor(s), (Color{1.0f, 0.35f, 0.3f}));
    s.setMode("place");
    EXPECT_EQ(brushRingColor(s), (Color{0.3f, 1.0f, 0.4f}));
    s.setMode("part");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.3f, 0.3f}));
    s.setMode("flow");
    EXPECT_EQ(brushRingColor(s), (Color{1.0f, 0.55f, 0.1f}));
    EXPECT_EQ(brushPathColor(s), (Color{1.0f, 0.3f, 0.3f}));  // a part or flow stroke's path: red
}

// The unit sphere with UVs (u around, v from the bottom pole up).
std::shared_ptr<const ScalpMesh> sphereWithUVs(int rings = 24, int segments = 48) {
    auto base = test::makeSphere(rings, segments);
    auto mesh = std::make_shared<ScalpMesh>(*base);
    std::vector<std::array<float, 2>> uvs;
    for (const ScalpMesh::Triangle& t : mesh->triangles()) {
        for (const int v : {t[0], t[1], t[2]}) {
            const int r = v / (segments + 1), s = v % (segments + 1);
            uvs.push_back({static_cast<float>(s) / segments, 1.0f - static_cast<float>(r) / rings});
        }
    }
    mesh->setCornerUVs(std::move(uvs));
    return mesh;
}

GuideHierarchy groomOn(const ScalpMesh& scalp, int primaries, std::size_t children, std::uint64_t seed = 5) {
    GuideHierarchy h;
    const GroupId primary = h.addGroup("primary");
    Rng rng(seed);
    for (int i = 0; i < primaries; ++i) {
        const Vec3f n = Vec3f(rng.uniform(-1, 1), rng.uniform(0.3f, 1), rng.uniform(0.2f, 1)).normalized();
        h.addGuide(primary, test::straightCurve(n, n * 0.6f + Vec3f(0.05f, 0.1f, 0.05f), 10));
    }
    h.evaluate(&scalp);
    if (children > 0) {
        ChildGenerationParams p;
        p.count = children;
        h.generateChildren(primary, "children", p);
        h.evaluate(&scalp);
    }
    return h;
}

ToolView frontView() { return ToolView::fromMatrix(test::lookAtProjection({0, 1.2f, 4}, {0, 0.6f, 0}, 40.0, 1.0), 800, 800); }

// The view's pixels of `n` points from a to b (world).
std::vector<std::array<float, 2>> screenLine(const ToolView& view, const Vec3f& a, const Vec3f& b, int n) {
    float ax = 0, ay = 0, bx = 0, by = 0;
    view.project(a, ax, ay);
    view.project(b, bx, by);
    std::vector<std::array<float, 2>> out;
    for (int k = 0; k < n; ++k) {
        const float t = static_cast<float>(k) / static_cast<float>(n - 1);
        out.push_back({std::round(ax + (bx - ax) * t), std::round(ay + (by - ay) * t)});
    }
    return out;
}

// The same view without its matrix: the host's projection and rays (serial weights, nothing
// culled by bounds), as a host whose matrix could not be checked.
ToolView hostOnly(const ToolView& view) {
    ToolView out;
    const ViewProjection projection = view.projection;
    out.hostProject = [projection](const Vec3f& p, float& x, float& y) { return projection.project(p, x, y); };
    out.hostRay = [view](float x, float y, Vec3f& o, Vec3f& d) { return view.ray(x, y, o, d); };
    out.hasCamera = view.hasCamera;
    out.eye = view.eye;
    out.viewDirection = view.viewDirection;
    return out;
}

TEST(ToolView, CameraAndRaysFromTheMatrix) {
    const Vec3f eye{0, 1.2f, 4}, target{0, 0.6f, 0};
    const ToolView view = frontView();
    ASSERT_TRUE(view.hasCamera);
    EXPECT_FALSE(view.orthographic);
    EXPECT_NEAR((view.eye - eye).length(), 0.0f, 1e-3f);
    EXPECT_NEAR(view.viewDirection.dot((target - eye).normalized()), 1.0f, 1e-5f);
    // The ray under a point's pixel passes through it.
    const Vec3f p{0.3f, 0.9f, 0.6f};
    float x = 0, y = 0;
    ASSERT_TRUE(view.project(p, x, y));
    Vec3f o, d;
    ASSERT_TRUE(view.ray(x, y, o, d));
    const Vec3f toP = p - o;
    EXPECT_NEAR((o + d * toP.dot(d) - p).length(), 0.0f, 1e-4f);
}

// The brush ring's world radius (what every host draws and the brush's map dabs touch): the ray
// a radius to the right of the center's pixel meets the plane through the center facing the
// camera there, so that point projects a radius away; none behind the camera.
TEST(ToolView, WorldRadiusIsTheRingsEdge) {
    const ToolView view = frontView();
    const Vec3f center{0.1f, 0.7f, 0.2f};
    float x = 0, y = 0;
    ASSERT_TRUE(view.project(center, x, y));
    for (float pixels : {1.0f, 25.0f, 60.0f, 200.0f}) {
        const float radius = worldRadiusAt(view, center, x, y, pixels);
        ASSERT_GT(radius, 0.0f);
        // A point that far from the center across the view lands a radius away on screen.
        const Vec3f across = view.viewDirection.cross(Vec3f{0, 1, 0}).normalized();
        float px = 0, py = 0, qx = 0, qy = 0;
        ASSERT_TRUE(view.project(center + across * radius, px, py));
        ASSERT_TRUE(view.project(center - across * radius, qx, qy));
        EXPECT_NEAR(0.5f * std::hypot(px - qx, py - qy), pixels, 0.02f * pixels + 0.05f) << pixels;
    }
    EXPECT_EQ(worldRadiusAt(view, Vec3f{0, 1.2f, 9}, x, y, 60.0f), 0.0f);  // behind the camera
}

// The settings every host sets, kept in range the same way (Maya's flags, Blender's settings).
TEST(Stroke, ToolSettingsStayInRange) {
    BrushToolSettings s;
    s.setScreenRadius(0.2f);
    EXPECT_EQ(s.screenRadius, 1.0f);
    s.setSegments(0);
    EXPECT_EQ(s.place.segments, 1);
    s.setPaintResolution(1);
    EXPECT_EQ(s.paintResolution, 4);
    s.setPaintResolution(100000);
    EXPECT_EQ(s.paintResolution, 16384);
    s.setHardness(1.5f);
    EXPECT_EQ(s.brush.hardness, 1.0f);
    s.setSmoothStrength(-1.0f);
    EXPECT_EQ(s.smoothStrength, 0.0f);
    s.setPaintColor({-0.5f, 0.25f, 2.0f, 1.0f});
    EXPECT_EQ(s.paintColor, (PaintColor{0.0f, 0.25f, 1.0f, 1.0f}));
}

TEST(ToolView, PixelsAsWorldToViewGivesThem) {
    const ViewProjection view(test::lookAtProjection({0, 1, 5}, {0, 0.5f, 0}, 40.0, 1.0), 640, 480);
    int ix = 0, iy = 0;
    float fx = 0, fy = 0;
    const Vec3f p{0.37f, 0.81f, 0.2f};
    ASSERT_TRUE(view.projectPixel(p, ix, iy));
    ASSERT_TRUE(view.project(p, fx, fy));
    EXPECT_EQ(ix, static_cast<int>(fx));  // truncated
    EXPECT_EQ(iy, static_cast<int>(fy));
    EXPECT_FALSE(view.projectPixel({50.0f, 0.5f, 0.0f}, ix, iy));  // outside the view: no pixel
    EXPECT_TRUE(view.project({50.0f, 0.5f, 0.0f}, fx, fy));        // ... though in front of the camera
    EXPECT_FALSE(view.projectPixel({0.0f, 1.0f, 9.0f}, ix, iy));   // behind it
}

TEST(GuidePicking, LassoBoundsAndClicks) {
    const auto scalp = sphereWithUVs();
    const GuideHierarchy h = groomOn(*scalp, 60, 3);
    const ViewProjection view(test::lookAtProjection({0, 1.2f, 4}, {0, 0.6f, 0}, 40.0, 1.25), 1000, 800);
    const ViewProjector project = pixelProjector(view);
    const std::vector<GuideId> all = guidesAt(h, -1);
    Rng rng(9);
    for (int k = 0; k < 20; ++k) {
        std::vector<std::array<float, 2>> lasso;
        const float cx = rng.uniform(100, 900), cy = rng.uniform(100, 700), r = rng.uniform(20, 300);
        for (int i = 0; i < 9; ++i) {
            const float a = 6.2831853f * i / 9 + rng.uniform(0, 0.3f);
            lasso.push_back({cx + r * std::cos(a) * rng.uniform(0.5f, 1), cy + r * std::sin(a)});
        }
        for (const bool roots : {false, true}) {
            EXPECT_EQ(pickGuides(h, all, project, lasso, roots, 12.0f, &view), guidesInLasso(h, all, project, lasso, roots));
        }
    }
    // A click picks the guide with the nearest point within the pixels.
    const HierarchyGuide& g = h.guides()[7];
    std::array<float, 2> at{};
    ASSERT_TRUE(project(g.points.back(), &at));
    EXPECT_EQ(pickGuides(h, all, project, {{at[0] + 1, at[1]}}, false, 12.0f), std::vector<GuideId>{g.id});
    EXPECT_TRUE(pickGuides(h, all, project, {{-500, -500}}, false, 12.0f).empty());
    // Hidden groups are not candidates.
    EXPECT_EQ(guidesAt(h, -1, "", {"children"}), guidesAt(h, 0));
}

TEST(GuidePicking, OnlyWhatTheViewShows) {
    // A guide on the front of the head and one on its back, seen from the front: through
    // visibleProjector a lasso over the whole view (or a click on the back guide's root) picks
    // only the front one; the scalp hides the other.
    const auto scalp = sphereWithUVs();
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (const Vec3f& n : {Vec3f(0.0f, 0.5f, 1.0f).normalized(), Vec3f(0.0f, 0.5f, -1.0f).normalized()}) {
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 6; ++k) pts.push_back(n * (1.0f + 0.04f * static_cast<float>(k)));
        h.addGuide(g, pts);
    }
    h.evaluate(scalp.get());
    const GuideId front = h.guides()[0].id, back = h.guides()[1].id;
    const std::array<double, 16> matrix = test::lookAtProjection({0, 1.2f, 4}, {0, 0.6f, 0}, 40.0, 1.0);
    const ViewProjection view(matrix, 800, 800);
    const ToolView tool = ToolView::fromMatrix(matrix, 800, 800);
    const std::vector<std::array<float, 2>> everything{{1, 1}, {799, 1}, {799, 799}, {1, 799}};
    const std::vector<GuideId> all = guidesAt(h, -1);
    EXPECT_EQ(pickGuides(h, all, pixelProjector(view), everything, false, 12.0f), (std::vector<GuideId>{front, back}));
    const ViewProjector visible = visibleProjector(pixelProjector(view), scalpVisibility(scalp, tool));
    EXPECT_EQ(pickGuides(h, all, visible, everything, false, 12.0f, &view), std::vector<GuideId>{front});
    std::array<float, 2> at{};
    ASSERT_TRUE(pixelProjector(view)(h.guides()[1].points.front(), &at));
    EXPECT_TRUE(pickGuides(h, {back}, visible, {at}, false, 12.0f).empty()) << "a click on the hidden guide picks nothing";
    EXPECT_EQ(visibleProjector(pixelProjector(view), nullptr)(h.guides()[1].points.front(), &at), true) << "no scalp: as before";
}

TEST(GuidePicking, SelectionModes) {
    const std::vector<GuideId> selected{4, 2, 9}, picked{9, 7, 7, 1};
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Replace), picked);
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Add), (std::vector<GuideId>{4, 2, 9, 7, 1}));
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Deselect), (std::vector<GuideId>{4, 2}));
    // Toggle: each picked guide in turn (7 twice: in, then out again).
    EXPECT_EQ(combineSelection(selected, picked, SelectMode::Toggle), (std::vector<GuideId>{4, 2, 1}));
    EXPECT_EQ(selectModeFromName("toggle"), SelectMode::Toggle);
    EXPECT_THROW(selectModeFromName("xor"), std::invalid_argument);
}

TEST(GuidePicking, MirrorSelectionAsAScanWouldPick) {
    const auto scalp = sphereWithUVs();
    GuideHierarchy h = groomOn(*scalp, 40, 2);
    // Twins: two guides at the same root (equally near any target).
    const GroupId primary = h.findGroup("primary");
    const HierarchyGuide first = h.guides().front();
    h.addGuide(primary, first.points);
    h.addGuide(primary, first.points);
    h.evaluate(scalp.get());
    auto scan = [&](const std::vector<GuideId>& selected, int axis, float center, float tolerance) {
        std::vector<GuideId> out;
        for (const GuideId id : selected) {
            const HierarchyGuide* g = h.guide(id);
            if (!g || g->points.empty()) continue;
            const Vec3f target = mirrorPoint(g->points.front(), axis, center);
            GuideId best = kNoGuide;
            float bestDist = tolerance;
            for (const HierarchyGuide& other : h.guides()) {
                if (other.group != g->group || other.points.empty()) continue;
                const float d = (other.points.front() - target).length();
                if (d <= bestDist) {
                    bestDist = d;
                    best = other.id;
                }
            }
            if (best != kNoGuide && std::find(out.begin(), out.end(), best) == out.end()) out.push_back(best);
        }
        return out;
    };
    const std::vector<GuideId> all = guidesAt(h, -1);
    for (const int axis : {0, 1, 2}) {
        for (const float tolerance : {1e30f, 0.2f, 0.0f}) {
            EXPECT_EQ(mirrorSelection(h, all, axis, 0.0f, tolerance), scan(all, axis, 0.0f, tolerance));
        }
    }
    // Mirrored onto itself (center through the twins' root): the last twin, as the scan picks.
    const float center = first.points.front().x;
    EXPECT_EQ(mirrorSelection(h, {first.id}, 0, center, 1e30f), scan({first.id}, 0, center, 1e30f));
}

// ---- Brush alphas ------------------------------------------------------------------------------

// An alpha `size` square, each texel `f(u, v)` (u, v in -1..1 at its center, v up).
template <typename F>
BrushAlphaPtr alphaOf(int size, F f) {
    std::vector<float> values;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = (x + 0.5f) / size * 2.0f - 1.0f, v = 1.0f - (y + 0.5f) / size * 2.0f;
            values.push_back(f(u, v));
        }
    }
    return std::make_shared<const BrushAlpha>(size, size, std::move(values));
}

void alphaPaint(BrushTool& tool, const BrushAlphaPtr& alpha, float radius) {
    tool.settings.setMode("paint");
    tool.settings.paintKind = "vertices";
    tool.settings.brush.attribute = "crown";
    tool.settings.brush.value = 1.0f;
    tool.settings.brush.strength = 1.0f;
    tool.settings.setHardness(1.0f);  // full strength to the edge: the alpha alone shapes the dab
    tool.settings.screenRadius = radius;
    tool.settings.alpha = alpha;
}

}  // namespace
}  // namespace groomlab
