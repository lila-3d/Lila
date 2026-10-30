#include <doctest/doctest.h>

#include "GarmentCodeImport.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace tailor;

namespace
{

// Front is counter-clockwise with a quadratic top edge; back is clockwise and rotated 180 degrees about Y.
const char* kTwoPanels = R"({
  "pattern": {
    "panels": {
      "front": {
        "translation": [0, 100, 10],
        "rotation": [0, 0, 0],
        "vertices": [[0, 0], [40, 0], [40, 50], [0, 50]],
        "edges": [
          {"endpoints": [0, 1]},
          {"endpoints": [1, 2]},
          {"endpoints": [2, 3], "curvature": {"type": "quadratic", "params": [[0.5, 0.2]]}},
          {"endpoints": [3, 0]}
        ]
      },
      "back": {
        "translation": [0, 100, -10],
        "rotation": [0, 180, 0],
        "vertices": [[0, 0], [0, 50], [40, 50], [40, 0]],
        "edges": [
          {"endpoints": [0, 1]},
          {"endpoints": [1, 2]},
          {"endpoints": [2, 3]},
          {"endpoints": [3, 0]}
        ]
      }
    },
    "stitches": [
      [{"panel": "front", "edge": 1}, {"panel": "back", "edge": 2}],
      [{"panel": "front", "edge": 3}, {"panel": "back", "edge": 0}, "right_wrong"]
    ]
  },
  "properties": {"curvature_coords": "relative", "units_in_meter": 100}
})";

double signedArea(const std::vector<Vec2>& o)
{
    double a = 0;
    for (size_t i = 0; i < o.size(); ++i)
    {
        const Vec2& p = o[i];
        const Vec2& q = o[(i + 1) % o.size()];
        a += double(p[0]) * q[1] - double(q[0]) * p[1];
    }
    return 0.5 * a;
}

} // namespace

TEST_CASE("GarmentCode: two panels, units, curves, placement")
{
    Garment g;
    std::string err;
    REQUIRE_MESSAGE(importGarmentCode(kTwoPanels, g, &err), err);
    REQUIRE(g.panels.size() == 2);
    CHECK(g.panels[0].name == "front");
    CHECK(g.panels[1].name == "back");

    const Panel& front = g.panels[0];
    CHECK(signedArea(front.outline) > 0);
    // 40 cm curved edge sampled at ~1 cm plus three straight-edge starts.
    CHECK(front.outline.size() >= 40);
    CHECK(front.outline[0][0] == doctest::Approx(0.f));
    CHECK(front.outline[1][0] == doctest::Approx(0.4f));
    CHECK(front.outline[2][1] == doctest::Approx(0.5f));
    CHECK(front.outline.back()[0] == doctest::Approx(0.f));
    CHECK(front.outline.back()[1] == doctest::Approx(0.5f));

    // Control point (20, 42) cm: the curve midpoint dips to y = 46 cm.
    float minTop = 1.f;
    for (size_t i = 2; i + 1 < front.outline.size(); ++i)
        minTop = std::min(minTop, front.outline[i][1]);
    CHECK(minTop == doctest::Approx(0.46f).epsilon(0.005));

    CHECK(front.origin[1] == doctest::Approx(1.f));
    CHECK(front.origin[2] == doctest::Approx(0.1f));
    CHECK(front.right[0] == doctest::Approx(1.f));

    const Panel& back = g.panels[1];
    REQUIRE(back.outline.size() == 4);
    CHECK(signedArea(back.outline) > 0);
    CHECK(back.right[0] == doctest::Approx(-1.f));
    CHECK(back.right[2] == doctest::Approx(0.f).epsilon(1e-5));
    CHECK(back.up[1] == doctest::Approx(1.f));
    CHECK(back.origin[2] == doctest::Approx(-0.1f));
}

TEST_CASE("GarmentCode: stitches map to seam runs")
{
    Garment g;
    std::string err;
    REQUIRE_MESSAGE(importGarmentCode(kTwoPanels, g, &err), err);
    REQUIRE(g.seams.size() == 2);
    const Panel& front = g.panels[0];
    const Panel& back = g.panels[1];

    // Front right side against back right side: both runs walk (40, 0) to (40, 50), so no reversal.
    const Seam& s0 = g.seams[0];
    REQUIRE(s0.a.size() == 1);
    REQUIRE(s0.b.size() == 1);
    CHECK(s0.a[0].panel == 0);
    CHECK(s0.a[0].from == 1);
    CHECK(s0.a[0].to == 2);
    CHECK(s0.b[0].panel == 1);
    CHECK(back.outline[s0.b[0].from][1] == doctest::Approx(0.f));
    CHECK(back.outline[s0.b[0].to][1] == doctest::Approx(0.5f));
    CHECK(back.outline[s0.b[0].to][0] == doctest::Approx(0.4f));
    CHECK_FALSE(s0.reversed);

    // Right-to-wrong stitch pairs start with start: front (0, 50) meets back (0, 0). Both runs walk (0, 50) to (0, 0).
    const Seam& s1 = g.seams[1];
    CHECK(s1.a[0].from == int(front.outline.size()) - 1);
    CHECK(s1.a[0].to == 0);
    CHECK(back.outline[s1.b[0].from][1] == doctest::Approx(0.5f));
    CHECK(back.outline[s1.b[0].to][1] == doctest::Approx(0.f));
    CHECK(back.outline[s1.b[0].to][0] == doctest::Approx(0.f));
    CHECK(s1.reversed);
}

TEST_CASE("GarmentCode: circle arc edge")
{
    // Semicircle of radius 20 cm on the top edge, bulging outward ("right" flag).
    const char* json = R"({"pattern": {"panels": {"p": {
        "translation": [0, 0, 0], "rotation": [0, 0, 0],
        "vertices": [[0, 0], [40, 0], [40, 50], [0, 50]],
        "edges": [{"endpoints": [0, 1]}, {"endpoints": [1, 2]},
                  {"endpoints": [2, 3], "curvature": {"type": "circle", "params": [20, 0, 1]}},
                  {"endpoints": [3, 0]}]}}, "stitches": []}})";
    Garment g;
    std::string err;
    REQUIRE_MESSAGE(importGarmentCode(json, g, &err), err);
    const auto& o = g.panels[0].outline;
    float maxY = 0.f;
    for (size_t i = 2; i + 1 < o.size(); ++i)
    {
        CHECK(std::hypot(o[i][0] - 0.2f, o[i][1] - 0.5f) == doctest::Approx(0.2f).epsilon(1e-3));
        maxY = std::max(maxY, o[i][1]);
    }
    CHECK(maxY == doctest::Approx(0.7f).epsilon(0.002));
    CHECK(signedArea(o) > 0);
}

TEST_CASE("GarmentCode: errors leave the garment untouched")
{
    Garment g;
    g.panels.resize(3);
    std::string err;
    CHECK_FALSE(importGarmentCode("{not json", g, &err));
    CHECK_FALSE(err.empty());

    const char* badStitch = R"({"pattern": {"panels": {"p": {
        "vertices": [[0, 0], [1, 0], [1, 1]],
        "edges": [{"endpoints": [0, 1]}, {"endpoints": [1, 2]}, {"endpoints": [2, 0]}]}},
        "stitches": [[{"panel": "p", "edge": 0}, {"panel": "missing", "edge": 0}]]}})";
    err.clear();
    CHECK_FALSE(importGarmentCode(badStitch, g, &err));
    CHECK(err.find("missing") != std::string::npos);
    CHECK(g.panels.size() == 3);
}
