#include <doctest/doctest.h>

#include "Blocks.h"
#include "Pattern.h"

#include <cmath>

using namespace tailor;

namespace
{
Panel detailRect(const char* name, float w, float h, Vec3 origin)
{
    Panel p;
    p.name = name;
    p.outline = {{0.f, 0.f}, {w, 0.f}, {w, h}, {0.f, h}};
    p.origin = origin;
    p.particleDistance = 0.02f;
    return p;
}

float gapBetween(const Vec3& a, const Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

int detailPanel(const Garment& g, const std::string& name)
{
    for (size_t k = 0; k < g.panels.size(); ++k)
        if (g.panels[k].name == name)
            return int(k);
    return -1;
}

/** Drapes on the mannequin and returns the largest gap left between sewn or tacked pairs. */
float detailDrapeGap(const Garment& g, int frames, ClothMesh* out = nullptr, std::vector<Vec3>* positions = nullptr)
{
    static const Sdf sdf = [] {
        std::vector<Vec3> pts;
        std::vector<std::array<int, 3>> tris;
        mannequin(Body{}, pts, tris);
        return sdfFromMesh(pts, tris, 0.01f, 0.05f);
    }();
    const ClothMesh m = buildClothMesh(g);
    ClothSolver s;
    s.reset(m, g, SolverSettings{});
    s.setColliders({&sdf});
    for (int f = 0; f < frames; ++f)
        s.step();
    float gap = 0.f;
    for (const auto& pr : m.seamPairs)
    {
        REQUIRE(std::isfinite(s.positions()[size_t(pr[0])][0]));
        gap = std::max(gap, gapBetween(s.positions()[size_t(pr[0])], s.positions()[size_t(pr[1])]));
    }
    if (out)
        *out = m;
    if (positions)
        *positions = s.positions();
    return gap;
}
} // namespace

TEST_CASE("a pocket with a flap lies tacked on its panel")
{
    Garment g;
    REQUIRE(buildBlock("tee", g));
    for (Panel& p : g.panels)
        p.particleDistance = 0.03f;
    const int front = detailPanel(g, "Front");
    REQUIRE(front >= 0);
    const size_t panels = g.panels.size();
    const int pocket = addPocket(g, front, {0.05f, 0.25f}, 0.14f, 0.16f, true);
    REQUIRE(pocket == int(panels));
    CHECK(g.panels.size() == panels + 2);
    CHECK(g.tacks.size() > 20);
    // Before draping, the pocket sits just off the front where it is tacked.
    for (const Tack& t : g.tacks)
        if (t.panelA == pocket)
            CHECK(gapBetween(placePatternPoint(g.panels[size_t(t.panelA)], t.atA), placePatternPoint(g.panels[size_t(t.panelB)], t.atB)) < 0.01f);
    CHECK(detailDrapeGap(g, 60) < 0.03f);
}

TEST_CASE("a collar goes round the neckline and closes into a ring")
{
    Garment g;
    REQUIRE(buildBlock("tee", g));
    for (Panel& p : g.panels)
        p.particleDistance = 0.03f;
    const std::vector<SeamSide> neck = findNeckline(g);
    REQUIRE(neck.size() == 2);
    float neckLength = 0.f;
    neckLength = seamSideLength(g, neck);
    const size_t seams = g.seams.size();
    const int collar = addCollar(g, neck, 0.07f, true);
    REQUIRE(collar >= 0);
    CHECK(g.seams.size() == seams + 2);
    CHECK(g.panels[size_t(collar)].outline[1][0] == doctest::Approx(neckLength));
    CHECK(detailDrapeGap(g, 80) < 0.04f);
}

TEST_CASE("a drawstring draws its channel in")
{
    Garment g;
    g.panels = {detailRect("Bag", 0.4f, 0.3f, {-0.2f, 1.f, 0.2f})};
    const int cord = addDrawstring(g, {{0, 2, 3}}, 0.1f, 0.6f, 0.01f);
    REQUIRE(cord == 1);
    CHECK(g.panels[1].outline[1][0] == doctest::Approx(0.4f * 0.6f + 0.2f));
    // Pin the bag's lower corners so it hangs; the drawn-in top comes out narrower than it was cut.
    const ClothMesh m = buildClothMesh(g);
    int c0 = -1, c1 = -1, t0 = -1, t1 = -1;
    for (size_t v = 0; v < m.uv.size(); ++v)
    {
        if (m.vertPanel[v] != 0)
            continue;
        auto near = [&](float x, float y) { return std::hypot(m.uv[v][0] - x, m.uv[v][1] - y) < 1e-4f; };
        if (near(0.f, 0.f)) c0 = int(v);
        if (near(0.4f, 0.f)) c1 = int(v);
        if (near(0.f, 0.3f)) t0 = int(v);
        if (near(0.4f, 0.3f)) t1 = int(v);
    }
    REQUIRE(c0 >= 0);
    REQUIRE(t1 >= 0);
    ClothSolver s;
    SolverSettings settings;
    settings.gravity = {0.f, 0.f, 0.f};
    s.reset(m, g, settings);
    s.setPins({{c0, m.rest3d[size_t(c0)]}, {c1, m.rest3d[size_t(c1)]}});
    for (int f = 0; f < 120; ++f)
        s.step();
    CHECK(gapBetween(s.positions()[size_t(t0)], s.positions()[size_t(t1)]) < 0.4f * 0.8f);
}

TEST_CASE("lacing zigzags through eyelets on both edges")
{
    Garment g;
    g.panels = {detailRect("Left", 0.2f, 0.3f, {-0.23f, 1.f, 0.f}), detailRect("Right", 0.2f, 0.3f, {0.03f, 1.f, 0.f})};
    std::vector<std::pair<int, Vec2>> eyelets;
    const int lace = addLacing(g, {{0, 1, 2}}, {{1, 3, 0}}, 3, 0.015f, 0.08f, 0.006f, &eyelets);
    REQUIRE(lace == 2);
    CHECK(eyelets.size() == 6);
    CHECK(g.tacks.size() == 6);
    // Alternating sides, inset from each edge.
    for (size_t k = 0; k + 1 < eyelets.size(); ++k)
        CHECK(eyelets[k].first != eyelets[k + 1].first);
    for (const auto& [panel, at] : eyelets)
        CHECK(at[0] == doctest::Approx(panel == 0 ? 0.185f : 0.015f).epsilon(0.01));
    CHECK(detailDrapeGap(g, 30) < 0.5f);
}

TEST_CASE("belt loops and an opened zip seam")
{
    Garment g;
    g.panels = {detailRect("A", 0.3f, 0.4f, {-0.3f, 1.f, 0.f}), detailRect("B", 0.3f, 0.4f, {0.f, 1.f, 0.f})};
    Seam s;
    s.a = {{0, 1, 2}};
    s.b = {{1, 3, 0}};
    s.reversed = true;
    g.seams = {s};
    const std::vector<int> loops = addBeltLoops(g, 0, 0.3f, 3, 0.012f, 0.05f);
    CHECK(loops.size() == 3);
    CHECK(g.tacks.size() >= 3 * 4);

    REQUIRE(openSeam(g, 0, 0.25f));
    REQUIRE(g.seams.size() == 1);
    CHECK(seamSideLength(g, g.seams[0].a) == doctest::Approx(0.1f).epsilon(0.01));
    CHECK(seamSideLength(g, g.seams[0].b) == doctest::Approx(0.1f).epsilon(0.01));
    // The kept stretch is the bottom quarter on both sides, so the zip stays closed from the hem up.
    const auto& a = g.panels[0].outline;
    CHECK(a[size_t(g.seams[0].a[0].from)][1] == doctest::Approx(0.f));
    const ClothMesh m = buildClothMesh(g);
    for (const auto& pr : m.seamPairs)
        if (m.vertPanel[size_t(pr[0])] != m.vertPanel[size_t(pr[1])] && m.vertPanel[size_t(pr[0])] < 2 && m.vertPanel[size_t(pr[1])] < 2)
            CHECK(m.uv[size_t(pr[0])][1] <= 0.1001f);
}

TEST_CASE("wrinkle maps fold across the squeezed direction and tile")
{
    const Fabric cotton = *findFabric("Cotton");
    const int size = 64;
    const std::vector<float> u = bakeWrinkleHeight(cotton, 0, 0.25f, size, 0.2f);
    REQUIRE(u.size() == size_t(size * size));
    // Folds made by squeezing along U run along V: height changes far more across U than along V.
    double acrossU = 0.0, alongV = 0.0, peak = 0.0;
    for (int y = 1; y < size - 1; ++y)
        for (int x = 1; x < size - 1; ++x)
        {
            acrossU += std::abs(u[size_t(y * size + x + 1)] - u[size_t(y * size + x)]);
            alongV += std::abs(u[size_t((y + 1) * size + x)] - u[size_t(y * size + x)]);
            peak = std::max(peak, double(std::abs(u[size_t(y * size + x)])));
        }
    CHECK(peak > 0.002);
    CHECK(acrossU > 1.5 * alongV);
    // The border stays flat, so the tile repeats without a seam.
    for (int k = 0; k < size; ++k)
        CHECK(std::abs(u[size_t(k)]) < 0.002f);
    const std::vector<float> n = heightToNormals(u, size, 0.2f);
    CHECK(n.size() == size_t(size * size * 4));
    for (size_t k = 0; k < n.size(); k += 4)
        CHECK(n[k + 2] >= 0.5f);
}
