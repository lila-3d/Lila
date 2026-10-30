#include <doctest/doctest.h>

#include "TailorCore.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

using namespace tailor;

namespace
{

Panel rectPanel(float w, float h, float particle)
{
    Panel p;
    p.name = "rect";
    p.outline = {{0.f, 0.f}, {w, 0.f}, {w, h}, {0.f, h}};
    p.particleDistance = particle;
    return p;
}

double triArea(const ClothMesh& m, const std::array<int, 3>& t)
{
    const Vec2 &a = m.uv[t[0]], &b = m.uv[t[1]], &c = m.uv[t[2]];
    return 0.5 * ((double(b[0]) - a[0]) * (double(c[1]) - a[1]) - (double(b[1]) - a[1]) * (double(c[0]) - a[0]));
}

double panelArea(const ClothMesh& m, int panel)
{
    double area = 0.0;
    for (size_t i = 0; i < m.tris.size(); ++i)
        if (m.triPanel[i] == panel)
            area += triArea(m, m.tris[i]);
    return area;
}

double uvLength(const ClothMesh& m, int a, int b) { return std::hypot(m.uv[b][0] - m.uv[a][0], m.uv[b][1] - m.uv[a][1]); }

std::set<std::pair<int, int>> edgeSet(const ClothMesh& m)
{
    std::set<std::pair<int, int>> edges;
    for (const auto& t : m.tris)
        for (int k = 0; k < 3; ++k)
            edges.insert(std::minmax(t[k], t[(k + 1) % 3]));
    return edges;
}

double minAngleDeg(const ClothMesh& m)
{
    double best = 180.0;
    for (const auto& t : m.tris)
        for (int k = 0; k < 3; ++k)
        {
            const Vec2 &p = m.uv[t[k]], &q = m.uv[t[(k + 1) % 3]], &r = m.uv[t[(k + 2) % 3]];
            const double ax = q[0] - p[0], ay = q[1] - p[1], bx = r[0] - p[0], by = r[1] - p[1];
            const double c = (ax * bx + ay * by) / (std::hypot(ax, ay) * std::hypot(bx, by));
            best = std::min(best, std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / 3.14159265358979);
        }
    return best;
}

int countWhere(const ClothMesh& m, int panel, int axis, float value)
{
    int n = 0;
    for (size_t v = 0; v < m.uv.size(); ++v)
        if (m.vertPanel[v] == panel && std::abs(m.uv[v][axis] - value) < 1e-5f)
            ++n;
    return n;
}

} // namespace

TEST_CASE("tailor meshing: rectangle is isotropic at the particle distance")
{
    Garment g;
    g.panels.push_back(rectPanel(1.f, 0.5f, 0.05f));
    const ClothMesh m = buildClothMesh(g);

    REQUIRE(!m.tris.empty());
    CHECK(m.uv.size() == m.rest3d.size());
    CHECK(m.vertPanel.size() == m.uv.size());
    CHECK(m.triPanel.size() == m.tris.size());
    CHECK(panelArea(m, 0) == doctest::Approx(0.5).epsilon(1e-5));

    double sum = 0.0, lo = 1e9, hi = 0.0;
    const auto edges = edgeSet(m);
    for (const auto& [a, b] : edges)
    {
        const double l = uvLength(m, a, b);
        sum += l;
        lo = std::min(lo, l);
        hi = std::max(hi, l);
    }
    const double mean = sum / double(edges.size());
    CHECK(mean > 0.8 * 0.05);
    CHECK(mean < 1.25 * 0.05);
    CHECK(lo > 0.3 * 0.05);
    CHECK(hi < 2.0 * 0.05);

    for (const auto& t : m.tris)
        CHECK(triArea(m, t) > 1e-3 * 0.05 * 0.05);
    CHECK(minAngleDeg(m) > 10.0);

    // Every vertex is used and the corners survive resampling.
    std::vector<char> used(m.uv.size(), 0);
    for (const auto& t : m.tris)
        for (int v : t)
            used[v] = 1;
    CHECK(std::all_of(used.begin(), used.end(), [](char c) { return c != 0; }));
    CHECK(countWhere(m, 0, 0, 1.f) >= 2);
    CHECK(countWhere(m, 0, 1, 0.5f) >= 2);
}

TEST_CASE("tailor meshing: holes are left open")
{
    Garment g;
    Panel p = rectPanel(1.f, 1.f, 0.05f);
    p.holes.push_back({{0.4f, 0.4f}, {0.4f, 0.6f}, {0.6f, 0.6f}, {0.6f, 0.4f}});
    g.panels.push_back(p);
    const ClothMesh m = buildClothMesh(g);

    REQUIRE(!m.tris.empty());
    CHECK(panelArea(m, 0) == doctest::Approx(1.0 - 0.04).epsilon(1e-5));
    for (const auto& uv : m.uv)
        CHECK_FALSE((uv[0] > 0.4f + 1e-5f && uv[0] < 0.6f - 1e-5f && uv[1] > 0.4f + 1e-5f && uv[1] < 0.6f - 1e-5f));
    for (const auto& t : m.tris)
    {
        const float cx = (m.uv[t[0]][0] + m.uv[t[1]][0] + m.uv[t[2]][0]) / 3.f;
        const float cy = (m.uv[t[0]][1] + m.uv[t[1]][1] + m.uv[t[2]][1]) / 3.f;
        CHECK_FALSE((cx > 0.4f && cx < 0.6f && cy > 0.4f && cy < 0.6f));
    }
}

TEST_CASE("tailor meshing: seam resamples both sides to matching pairs")
{
    for (const bool reversed : {true, false})
    {
        CAPTURE(reversed);
        Garment g;
        g.panels.push_back(rectPanel(0.4f, 0.6f, 0.05f));
        Panel b = rectPanel(0.4f, 0.3f, 0.05f);
        b.origin = {1.f, 0.f, 0.f};
        g.panels.push_back(b);
        Seam s;
        s.a.push_back({0, 1, 2}); // right edge of A, walking up
        s.b.push_back({1, 3, 0}); // left edge of B, walking down (wraps)
        s.reversed = reversed;
        g.seams.push_back(s);
        const ClothMesh m = buildClothMesh(g);

        const int onA = countWhere(m, 0, 0, 0.4f);
        const int onB = countWhere(m, 1, 0, 0.f);
        CHECK(onA == 13); // 0.6 m at 0.05 m on the longer side
        CHECK(onB == onA);
        REQUIRE(m.seamPairs.size() == size_t(onA));
        CHECK(m.seamRestLength.size() == m.seamPairs.size());
        for (const auto& [va, vb] : m.seamPairs)
        {
            CHECK(m.vertPanel[va] == 0);
            CHECK(m.vertPanel[vb] == 1);
            CHECK(m.uv[va][0] == doctest::Approx(0.4f));
            CHECK(m.uv[vb][0] == doctest::Approx(0.f));
            const float expect = reversed ? m.uv[va][1] * 0.5f : 0.3f - m.uv[va][1] * 0.5f;
            CHECK(m.uv[vb][1] == doctest::Approx(expect).epsilon(1e-4));
        }
        CHECK(panelArea(m, 0) == doctest::Approx(0.24).epsilon(1e-5));
        CHECK(panelArea(m, 1) == doctest::Approx(0.12).epsilon(1e-5));
    }
}

TEST_CASE("tailor meshing: M:N seam sews a chain of two edges to one")
{
    Garment g;
    g.panels.push_back(rectPanel(0.3f, 0.3f, 0.05f));
    g.panels.push_back(rectPanel(0.3f, 0.3f, 0.05f));
    g.panels.push_back(rectPanel(0.6f, 0.2f, 0.05f));
    Seam s;
    s.a.push_back({0, 0, 1}); // bottom of panel 0
    s.a.push_back({1, 0, 1}); // bottom of panel 1
    s.b.push_back({2, 2, 3}); // top of panel 2, walking left
    s.reversed = true;
    g.seams.push_back(s);
    const ClothMesh m = buildClothMesh(g);

    // 0.6 m at 0.05 m: 13 samples, the middle one sews both panel ends to the same point.
    CHECK(m.seamPairs.size() == 14);
    for (const auto& [va, vb] : m.seamPairs)
    {
        const float xa = m.uv[va][0] + (m.vertPanel[va] == 1 ? 0.3f : 0.f);
        CHECK(m.vertPanel[vb] == 2);
        CHECK(m.uv[vb][0] == doctest::Approx(xa).epsilon(1e-4));
    }
}

TEST_CASE("tailor meshing: internal lines become crease edges")
{
    Garment g;
    Panel p = rectPanel(1.f, 0.5f, 0.05f);
    p.internalLines.push_back({{0.5f, 0.f}, {0.5f, 0.5f}});   // edge to edge
    p.internalLines.push_back({{0.2f, 0.1f}, {0.2f, 0.4f}});  // floating
    g.panels.push_back(p);
    const ClothMesh m = buildClothMesh(g);

    REQUIRE(!m.creaseEdges.empty());
    CHECK(panelArea(m, 0) == doctest::Approx(0.5).epsilon(1e-5));
    const auto edges = edgeSet(m);
    double length = 0.0;
    for (const auto& [a, b] : m.creaseEdges)
    {
        CHECK(edges.count(std::minmax(a, b)) == 1);
        CHECK(m.uv[a][0] == doctest::Approx(m.uv[b][0]));
        const float x = m.uv[a][0];
        CHECK((std::abs(x - 0.5f) < 1e-5f || std::abs(x - 0.2f) < 1e-5f));
        length += uvLength(m, a, b);
    }
    CHECK(length == doctest::Approx(0.8).epsilon(1e-4));
}

TEST_CASE("tailor meshing: placement, wrap and deactivated panels")
{
    Garment g;
    Panel flat = rectPanel(0.4f, 0.4f, 0.05f);
    flat.origin = {1.f, 2.f, 3.f};
    flat.right = {0.f, 0.f, -1.f};
    g.panels.push_back(flat);
    Panel wrapped = rectPanel(0.6f, 0.4f, 0.05f);
    wrapped.wrapRadius = 0.3f;
    g.panels.push_back(wrapped);
    Panel off = rectPanel(0.4f, 0.4f, 0.05f);
    off.state = PanelState::Deactivated;
    g.panels.push_back(off);
    const ClothMesh m = buildClothMesh(g);

    for (size_t v = 0; v < m.uv.size(); ++v)
    {
        CHECK(m.vertPanel[v] != 2);
        const Vec3& p = m.rest3d[v];
        if (m.vertPanel[v] == 0)
        {
            CHECK(p[0] == doctest::Approx(1.f));
            CHECK(p[1] == doctest::Approx(2.f + m.uv[v][1]));
            CHECK(p[2] == doctest::Approx(3.f - m.uv[v][0]));
        }
        else
        {
            // Wrapped about the up axis through (0, y, -radius).
            CHECK(std::hypot(p[0], p[2] + 0.3f) == doctest::Approx(0.3f).epsilon(1e-4));
            CHECK(p[1] == doctest::Approx(m.uv[v][1]));
        }
    }
    for (int panel : m.triPanel)
        CHECK(panel != 2);
}

TEST_CASE("tailor meshing: deterministic")
{
    Garment g;
    Panel p = rectPanel(0.7f, 0.45f, 0.03f);
    p.outline = {{0.f, 0.f}, {0.7f, 0.f}, {0.75f, 0.3f}, {0.35f, 0.45f}, {-0.05f, 0.3f}};
    p.holes.push_back({{0.3f, 0.15f}, {0.4f, 0.15f}, {0.35f, 0.25f}});
    p.internalLines.push_back({{0.1f, 0.1f}, {0.2f, 0.3f}});
    g.panels.push_back(p);
    g.panels.push_back(rectPanel(0.5f, 0.3f, 0.03f));
    Seam s;
    s.a.push_back({0, 0, 1});
    s.b.push_back({1, 2, 3});
    s.reversed = true;
    g.seams.push_back(s);

    const ClothMesh a = buildClothMesh(g);
    const ClothMesh b = buildClothMesh(g);
    CHECK(a.uv == b.uv);
    CHECK(a.tris == b.tris);
    CHECK(a.seamPairs == b.seamPairs);
    CHECK(a.creaseEdges == b.creaseEdges);
    CHECK(minAngleDeg(a) > 5.0);
    CHECK(panelArea(a, 0) == doctest::Approx(0.285 - 0.005).epsilon(1e-4));
    CHECK(a.seamPairs.size() == 24); // 0.7 m sewn to 0.5 m at 0.03 m: lround(0.7 / 0.03) + 1
}
