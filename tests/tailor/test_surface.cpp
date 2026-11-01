#include <doctest/doctest.h>

#include "TailorCore.h"

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
