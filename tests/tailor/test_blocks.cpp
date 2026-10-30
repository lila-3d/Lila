#include <doctest/doctest.h>

#include "Blocks.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace tailor;

namespace
{

float signedArea(const std::vector<Vec2>& o)
{
    float a = 0.f;
    for (size_t i = 0; i < o.size(); ++i)
    {
        const Vec2& p = o[i];
        const Vec2& q = o[(i + 1) % o.size()];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return 0.5f * a;
}

float orient(const Vec2& a, const Vec2& b, const Vec2& c)
{
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
}

bool segmentsCross(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d)
{
    const float d1 = orient(c, d, a), d2 = orient(c, d, b);
    const float d3 = orient(a, b, c), d4 = orient(a, b, d);
    return ((d1 > 0.f) != (d2 > 0.f)) && ((d3 > 0.f) != (d4 > 0.f)) && d1 != 0.f && d2 != 0.f && d3 != 0.f && d4 != 0.f;
}

bool selfIntersects(const std::vector<Vec2>& o)
{
    const size_t n = o.size();
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue; // adjacent through the closing edge
            if (segmentsCross(o[i], o[(i + 1) % n], o[j], o[(j + 1) % n]))
                return true;
        }
    return false;
}

float runLength(const Garment& g, const SeamSide& s)
{
    const auto& o = g.panels[s.panel].outline;
    const int n = int(o.size());
    float len = 0.f;
    for (int i = s.from; i != s.to; i = (i + 1) % n)
        len += std::hypot(o[(i + 1) % n][0] - o[i][0], o[(i + 1) % n][1] - o[i][1]);
    return len;
}

void checkGarment(const Garment& g, const std::string& name)
{
    CAPTURE(name);
    REQUIRE(!g.panels.empty());
    for (const Panel& p : g.panels)
    {
        CAPTURE(p.name);
        REQUIRE(p.outline.size() >= 3);
        CHECK(signedArea(p.outline) > 0.f);
        CHECK_FALSE(selfIntersects(p.outline));
        CHECK(p.fabric >= 0);
        CHECK(p.fabric < int(g.fabrics.size()));
        for (size_t i = 0; i < p.outline.size(); ++i)
        {
            const Vec2& a = p.outline[i];
            const Vec2& b = p.outline[(i + 1) % p.outline.size()];
            CHECK(std::hypot(b[0] - a[0], b[1] - a[1]) > 1e-4f);
        }
    }
    for (size_t si = 0; si < g.seams.size(); ++si)
    {
        CAPTURE(si);
        const Seam& s = g.seams[si];
        REQUIRE(!s.a.empty());
        REQUIRE(!s.b.empty());
        float la = 0.f, lb = 0.f;
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
            {
                REQUIRE(side.panel >= 0);
                REQUIRE(side.panel < int(g.panels.size()));
                const int n = int(g.panels[side.panel].outline.size());
                REQUIRE(side.from >= 0);
                REQUIRE(side.from < n);
                REQUIRE(side.to >= 0);
                REQUIRE(side.to < n);
                CHECK(side.from != side.to);
                (sides == &s.a ? la : lb) += runLength(g, side);
            }
        CHECK(la > 0.f);
        CHECK(std::abs(la - lb) <= 0.05f * std::max(la, lb));
    }
}

} // namespace

TEST_CASE("blocks: every block with defaults")
{
    REQUIRE(blocks().size() >= 15);
    for (const Block& b : blocks())
    {
        Garment g;
        std::string error;
        REQUIRE(buildBlock(b.id, g, &error));
        checkGarment(g, b.id);
    }
}

TEST_CASE("blocks: parameter variations")
{
    const std::vector<std::pair<std::string, BlockParamValues>> cases = {
        {"tee", {{"sleeveLength", 0.f}}},
        {"tee", {{"slit", 0.15f}, {"hemFlare", 0.3f}, {"ease", 0.5f}}},
        {"tunic", {{"length", 1.5f}, {"sleeveLength", 0.75f}}},
        {"robe", {{"openFront", 0.f}}},
        {"trousers", {{"length", 0.4f}}},
        {"skirt", {{"length", 0.15f}, {"flare", 0.f}}},
        {"cloak", {{"hood", 0.f}, {"closedSides", 1.f}}},
        {"cloak", {{"frontCoverage", 0.f}}},
        {"cape", {{"hood", 1.f}}},
        {"tabard", {{"sideTie", 0.f}}},
        {"poncho", {{"sideTie", 0.2f}, {"width", 0.3f}}},
        {"banner", {{"vertical", 0.f}}},
        {"banner", {{"tail", 0.f}}},
        {"tent", {{"sides", 3.f}, {"door", 0.f}}},
        {"tent", {{"sides", 16.f}}},
        {"belt", {{"fitRadius", 0.3f}, {"width", 0.1f}}},
        {"cloakTunicBelt", {{"hood", 0.f}}},
    };
    for (const auto& [id, values] : cases)
    {
        Garment g;
        REQUIRE(buildBlock(id, g, nullptr, Body{}, values));
        checkGarment(g, id);
    }
}

TEST_CASE("blocks: other bodies")
{
    Body child;
    child.height = 1.2f, child.chest = 0.64f, child.waist = 0.58f, child.hip = 0.66f, child.shoulderWidth = 0.3f;
    child.armLength = 0.42f, child.inseam = 0.52f, child.neck = 0.29f, child.head = 0.52f;
    Body large;
    large.chest = 1.25f, large.waist = 1.2f, large.hip = 1.25f, large.shoulderWidth = 0.52f;
    for (const Body& body : {child, large})
        for (const Block& b : blocks())
        {
            Garment g;
            REQUIRE(buildBlock(b.id, g, nullptr, body));
            checkGarment(g, b.id);
        }
}

TEST_CASE("blocks: registry")
{
    std::string error;
    Garment g;
    CHECK_FALSE(buildBlock("nope", g, &error));
    CHECK(!error.empty());
    const auto names = blockNames();
    CHECK(std::find(names.begin(), names.end(), "cloakTunicBelt") != names.end());
    const Block* tent = findBlock("tent");
    REQUIRE(tent);
    bool sawInt = false;
    for (const BlockParam& p : tent->params)
        sawInt |= p.name == "sides" && p.kind == BlockParam::Kind::Int && p.defaultValue == 6.f;
    CHECK(sawInt);
}

TEST_CASE("blocks: promo outfit layers and buckle")
{
    std::vector<BlockAnchor> anchors;
    const Garment g = cloakTunicBelt(Body{}, {}, &anchors);
    REQUIRE(anchors.size() == 1);
    CHECK(anchors[0].name == "buckle");
    REQUIRE(anchors[0].panel >= 0);
    CHECK(g.panels[anchors[0].panel].name == "Belt");
    CHECK(g.panels[anchors[0].panel].layer == 1);
    CHECK(anchors[0].position[2] > 0.f);
    CHECK(g.fabrics.size() == 3);
    bool hasCloak = false, hasHood = false;
    for (const Panel& p : g.panels)
    {
        hasHood |= p.name.rfind("Hood", 0) == 0 && p.layer == 2;
        hasCloak |= p.name == "Back" && p.layer == 2;
        if (p.name.rfind("Sleeve", 0) == 0)
            CHECK(p.layer == 0);
    }
    CHECK(hasCloak);
    CHECK(hasHood);
    // The belt starts outside the tunic and cinches through fabric shrink.
    const Panel& beltPanel = g.panels[anchors[0].panel];
    CHECK(g.fabrics[beltPanel.fabric].shrinkWarp < 1.f);
}

TEST_CASE("blocks: panels start outside the body")
{
    const Body body;
    for (const char* id : {"tee", "tunic", "robe", "skirt", "cloak", "poncho", "tabard"})
    {
        CAPTURE(id);
        Garment g;
        REQUIRE(buildBlock(id, g));
        for (const Panel& p : g.panels)
        {
            if (p.name.rfind("Sleeve", 0) == 0 || p.name.rfind("Hood", 0) == 0)
                continue;
            CHECK(std::hypot(p.origin[0], p.origin[2]) > body.chestRadius());
        }
    }
}
