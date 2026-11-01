#include <doctest/doctest.h>

#include "TailorCore.h"

#include <algorithm>
#include <cmath>
#include <map>

using namespace tailor;

namespace
{
/** Two 0.4 x 0.3 m panels facing outward (front +Z, back -Z), sewn down both sides: an open tube. */
ClothMesh sewnTube(int nu, int nv)
{
    ClothMesh m;
    const float w = 0.4f, h = 0.3f;
    for (int panel = 0; panel < 2; ++panel)
    {
        const float z = panel == 0 ? 0.1f : -0.1f;
        const int base = int(m.rest3d.size());
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
            {
                const float u = w * float(i) / float(nu - 1), v = h * float(j) / float(nv - 1);
                // The back is mirrored so both panels' normals point away from the tube's axis.
                const float x = panel == 0 ? u - 0.2f : 0.2f - u;
                m.rest3d.push_back({x, v, z});
                m.uv.push_back({u, v});
                m.vertPanel.push_back(panel);
            }
        for (int j = 0; j + 1 < nv; ++j)
            for (int i = 0; i + 1 < nu; ++i)
            {
                const int a = base + j * nu + i;
                m.tris.push_back({a, a + 1, a + nu + 1});
                m.tris.push_back({a, a + nu + 1, a + nu});
                m.triPanel.push_back(panel);
                m.triPanel.push_back(panel);
            }
    }
    const int back = nu * nv;
    for (int j = 0; j < nv; ++j)
    {
        // Front's left edge (x = -0.2) meets the back's right end (u = w), and the reverse.
        m.seamPairs.push_back({j * nu, back + j * nu + nu - 1});
        m.seamPairs.push_back({j * nu + nu - 1, back + j * nu});
        m.seamRestLength.push_back(0.f);
        m.seamRestLength.push_back(0.f);
    }
    return m;
}

std::map<std::pair<int, int>, int> edgeUses(const GarmentSurface& s)
{
    std::map<std::pair<int, int>, int> uses;
    for (const auto& t : s.tris)
        for (int k = 0; k < 3; ++k)
            ++uses[{std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3])}];
    return uses;
}

double signedVolume(const GarmentSurface& s)
{
    double v = 0.0;
    for (const auto& t : s.tris)
    {
        const Vec3 &a = s.points[t[0]], &b = s.points[t[1]], &c = s.points[t[2]];
        v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    return v;
}
} // namespace

TEST_CASE("surface: welding seams joins the panels")
{
    const int nu = 9, nv = 7;
    const ClothMesh m = sewnTube(nu, nv);
    const GarmentSurface open = garmentSurface(m, m.rest3d, {}, {.weldSeams = false});
    CHECK(open.points.size() == m.rest3d.size());
    const GarmentSurface welded = garmentSurface(m, m.rest3d, {});
    CHECK(welded.points.size() == m.rest3d.size() - m.seamPairs.size());
    CHECK(welded.tris.size() == m.tris.size());
    // Sewn edges are interior now: only the tube's top and bottom rings stay open.
    int border = 0;
    for (const auto& [e, n] : edgeUses(welded))
        border += n == 1;
    CHECK(border == 2 * 2 * (nu - 1));
    // Corners keep the pattern UVs of their own panel.
    CHECK(welded.fabricUv[0][0][0] == doctest::Approx(0.f));
    CHECK(welded.uv[0][1][0] == doctest::Approx(0.4f / float(nu - 1)));
}

TEST_CASE("surface: thickness makes a closed shell")
{
    const ClothMesh m = sewnTube(9, 7);
    const float t = 0.005f;
    const GarmentSurface s = garmentSurface(m, m.rest3d, {}, {.thickness = t});
    for (const auto& [e, n] : edgeUses(s))
        REQUIRE(n == 2);
    // Outward-wound shell: positive volume of about area x thickness (two 0.4 x 0.3 panels).
    CHECK(signedVolume(s) == doctest::Approx(2.0 * 0.4 * 0.3 * t).epsilon(0.1));
    int inner = 0, rim = 0;
    for (auto side : s.triSide)
    {
        inner += side == GarmentSurface::Inner;
        rim += side == GarmentSurface::Rim;
    }
    CHECK(inner == int(m.tris.size()));
    CHECK(rim == 2 * 2 * 2 * 8);
}

TEST_CASE("surface: packed UVs replace uv but not fabricUv")
{
    const ClothMesh m = sewnTube(3, 3);
    std::vector<Vec2> packed(m.tris.size() * 3, Vec2{0.5f, 0.25f});
    const GarmentSurface s = garmentSurface(m, m.rest3d, packed);
    CHECK(s.uv[2][1][1] == doctest::Approx(0.25f));
    CHECK(s.fabricUv[2][1][1] != doctest::Approx(0.25f));
}

namespace
{
/** Two 0.2 x 0.3 m panels side by side in the XY plane facing +Z, sewn along x = 0 with the given fold. */
ClothMesh hingedPair(int nu, int nv, Garment& g, float fold)
{
    ClothMesh m;
    for (int panel = 0; panel < 2; ++panel)
    {
        const int base = int(m.rest3d.size());
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
            {
                const float u = 0.2f * float(i) / float(nu - 1), v = 0.3f * float(j) / float(nv - 1);
                m.rest3d.push_back({panel == 0 ? u - 0.2f : u, v, 0.f});
                m.uv.push_back({u, v});
                m.vertPanel.push_back(panel);
            }
        for (int j = 0; j + 1 < nv; ++j)
            for (int i = 0; i + 1 < nu; ++i)
            {
                const int a = base + j * nu + i;
                m.tris.push_back({a, a + 1, a + nu + 1});
                m.tris.push_back({a, a + nu + 1, a + nu});
                m.triPanel.push_back(panel);
                m.triPanel.push_back(panel);
            }
    }
    for (int j = 0; j < nv; ++j)
    {
        m.seamPairs.push_back({j * nu + nu - 1, nu * nv + j * nu});
        m.seamRestLength.push_back(0.f);
        m.seamOfPair.push_back(0);
    }
    g.panels.resize(2);
    g.fabrics = {*findFabric("Denim")};
    g.seams.resize(1);
    g.seams[0].foldAngle = fold;
    return m;
}

float panelAngle(const ClothMesh& m, const std::vector<Vec3>& x)
{
    Vec3 n[2] = {{0, 0, 0}, {0, 0, 0}};
    for (size_t t = 0; t < m.tris.size(); ++t)
    {
        const Vec3 &a = x[size_t(m.tris[t][0])], &b = x[size_t(m.tris[t][1])], &c = x[size_t(m.tris[t][2])];
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        Vec3& s = n[m.triPanel[t]];
        s = {s[0] + e1[1] * e2[2] - e1[2] * e2[1], s[1] + e1[2] * e2[0] - e1[0] * e2[2], s[2] + e1[0] * e2[1] - e1[1] * e2[0]};
    }
    const float d = n[0][0] * n[1][0] + n[0][1] * n[1][1] + n[0][2] * n[1][2];
    const float l = std::hypot(n[0][0], n[0][1], n[0][2]) * std::hypot(n[1][0], n[1][1], n[1][2]);
    return std::acos(std::clamp(d / l, -1.f, 1.f)) * 180.f / 3.14159265f;
}
} // namespace

TEST_CASE("surface: a folded seam holds its angle")
{
    for (float fold : {0.f, 1.5707963f})
    {
        Garment g;
        const ClothMesh m = hingedPair(7, 9, g, fold);
        CHECK(seamHinges(m, g).size() == (fold == 0.f ? 0u : 8u));
        SolverSettings s;
        s.gravity = {0.f, 0.f, 0.f};
        ClothSolver solver;
        solver.reset(m, g, s);
        // A nudge out of plane so the fold has a side to start on.
        std::vector<Vec3>& x = solver.positionsMutable();
        for (size_t i = size_t(7 * 9); i < x.size(); ++i)
            x[i][2] += 0.01f * x[i][0];
        for (int f = 0; f < 300; ++f)
            solver.step();
        CAPTURE(fold);
        CHECK(std::abs(panelAngle(m, solver.positions()) - fold * 180.f / 3.14159265f) < 3.f);
    }
}

TEST_CASE("surface: a shrunk seam gathers its edge")
{
    Garment g;
    ClothMesh m = hingedPair(7, 9, g, 0.f);
    g.seams[0].shrink = 0.7f;
    CHECK(seamShrinks(m, g).size() == 16u);
    SolverSettings s;
    s.gravity = {0.f, 0.f, 0.f};
    ClothSolver solver;
    solver.reset(m, g, s);
    for (int f = 0; f < 200; ++f)
        solver.step();
    const auto& x = solver.positions();
    const Vec3 &top = x[size_t(8 * 7 + 6)], &bottom = x[size_t(6)];
    CHECK(std::hypot(top[0] - bottom[0], top[1] - bottom[1], top[2] - bottom[2]) < 0.3f * 0.75f);
}
