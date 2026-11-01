#include <doctest/doctest.h>

#include "Blocks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
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
        // Sleeve caps and cuffs ease or gather on purpose; checkPattern judges those.
        bool gathered = false;
        for (const auto* sides : {&s.a, &s.b})
            for (const SeamSide& side : *sides)
                for (const char* piece : {"Sleeve", "Cuff", "Band", "Puff", "Tier", "Flounce"})
                    gathered |= g.panels[side.panel].name.rfind(piece, 0) == 0;
        if (!gathered)
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

TEST_CASE("mannequin is a closed body the blocks fit around")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    REQUIRE(tris.size() > 1000);

    std::map<std::pair<int, int>, int> edgeUse;
    double volume = 0.0;
    float top = 0.f;
    for (const auto& t : tris)
    {
        for (int k = 0; k < 3; ++k)
            ++edgeUse[{std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3])}];
        const Vec3 &a = pts[t[0]], &b = pts[t[1]], &c = pts[t[2]];
        volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    for (const Vec3& p : pts)
        top = std::max(top, p[1]);
    int open = 0;
    int pinched = 0;
    for (const auto& [e, uses] : edgeUse)
    {
        open += uses % 2;
        pinched += uses > 2;
    }
    // Surface nets can pinch two sheets along an edge on diagonal limbs; that is still watertight.
    CHECK(open == 0);
    CHECK(pinched < 20);
    CHECK(volume > 0.04);
    CHECK(volume < 0.12);
    CHECK(top == doctest::Approx(body.height).epsilon(0.03));

    const Sdf sdf = sdfFromMesh(pts, tris, 0.02f, 0.1f);
    CHECK(sdf.sample({0.f, body.chestY(), 0.f}) < -0.05f);
    CHECK(sdf.sample({0.f, body.chestY(), body.chestRadius() + 0.1f}) > 0.f);
}

TEST_CASE("tunic drapes onto the mannequin without passing through")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);

    Garment g;
    REQUIRE(buildBlock("tunic", g));
    for (Panel& p : g.panels)
        p.particleDistance = 0.03f;
    const ClothMesh cm = buildClothMesh(g);
    ClothSolver solver;
    solver.reset(cm, g, SolverSettings{});
    solver.setColliders({&sdf});
    for (int frame = 0; frame < 90; ++frame)
        solver.step();

    int inside = 0, finite = 0;
    float lowest = 10.f;
    for (const Vec3& p : solver.positions())
    {
        finite += std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
        inside += sdf.sample(p) < -0.005f;
        lowest = std::min(lowest, p[1]);
    }
    CHECK(finite == int(solver.positions().size()));
    CHECK(inside < int(solver.positions().size()) / 100);
    // Hangs from the shoulders instead of falling off.
    CHECK(lowest > 0.4f);
    float maxGap = 0.f;
    for (const auto& pr : cm.seamPairs)
    {
        const Vec3 &a = solver.positions()[pr[0]], &b = solver.positions()[pr[1]];
        maxGap = std::max(maxGap, std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]));
    }
    CHECK(maxGap < 0.015f);
}

TEST_CASE("perf: outfit drape" * doctest::skip())
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    Garment g;
    REQUIRE(buildBlock("cloakTunicBelt", g));
    const ClothMesh cm = buildClothMesh(g);
    for (bool self : {false, true})
    {
        SolverSettings s;
        s.selfCollision = self;
        ClothSolver solver;
        solver.reset(cm, g, s);
        solver.setColliders({&sdf});
        const auto t0 = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 20; ++frame)
            solver.step();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        MESSAGE(cm.rest3d.size() << " verts, self " << self << ": " << ms / 20.0 << " ms/frame");
    }
}

TEST_CASE("measuring a mannequin gives back its measurements")
{
    for (const Body& want : {Body{}, Body{.height = 1.62f, .chest = 1.1f, .waist = 1.0f, .hip = 1.08f}})
    {
        std::vector<Vec3> pts;
        std::vector<std::array<int, 3>> tris;
        mannequin(want, pts, tris);
        for (Vec3& p : pts)
            p = {p[0] + 2.f, p[1] + 0.5f, p[2] - 1.f};
        Vec3 offset;
        const Body got = measureBody(pts, tris, &offset);
        CAPTURE(want.height);
        CHECK(got.height == doctest::Approx(want.height).epsilon(0.03));
        CHECK(got.chest == doctest::Approx(want.chest).epsilon(0.06));
        CHECK(got.waist == doctest::Approx(want.waist).epsilon(0.06));
        CHECK(got.hip == doctest::Approx(want.hip).epsilon(0.06));
        CHECK(offset[0] == doctest::Approx(2.f).epsilon(0.01));
        CHECK(offset[1] == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(std::abs(offset[2] + 1.f) < 0.03f);
    }
}

TEST_CASE("every sleeve style drafts a cap that fits its armhole and drapes")
{
    const Body body;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(body, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.05f);
    for (int st = 0; st < int(SleeveStyle::Count); ++st)
    {
        const std::string style = sleeveStyleName(SleeveStyle(st));
        CAPTURE(style);
        Garment g;
        REQUIRE(buildBlock("tee", g, nullptr, body, {{"sleeveStyle", float(st)}, {"sleeveLength", 0.55f}}));
        for (const Panel& p : g.panels)
        {
            CHECK(signedArea(p.outline) > 0.f);
            CHECK_FALSE(selfIntersects(p.outline));
        }
        bool sleeves = false;
        for (const Panel& p : g.panels)
            sleeves |= p.name.rfind("Sleeve", 0) == 0;
        CHECK(sleeves == (SleeveStyle(st) != SleeveStyle::Batwing));
        for (Panel& p : g.panels)
            p.particleDistance = 0.035f;
        const ClothMesh cm = buildClothMesh(g);
        ClothSolver solver;
        solver.reset(cm, g, SolverSettings{});
        solver.setColliders({&sdf});
        for (int frame = 0; frame < 80; ++frame)
            solver.step();
        float maxGap = 0.f;
        int worst = -1;
        for (size_t k = 0; k < cm.seamPairs.size(); ++k)
        {
            const Vec3 &a = solver.positions()[cm.seamPairs[k][0]], &b = solver.positions()[cm.seamPairs[k][1]];
            REQUIRE(std::isfinite(a[0]));
            const float d = std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
            if (d > maxGap)
                maxGap = d, worst = cm.seamOfPair[k];
        }
        CAPTURE(worst);
        // Within two cloth particles: closed bar the corners, where the coarse mesh can't meet exactly yet.
        CHECK(maxGap < 0.07f);
    }
}

TEST_CASE("pattern check: sleeves, fit and seams")
{
    const Body body;
    auto count = [](const std::vector<PatternIssue>& issues, PatternIssue::Level level, const std::string& word) {
        return std::count_if(issues.begin(), issues.end(), [&](const PatternIssue& i) { return i.level == level && i.message.find(word) != std::string::npos; });
    };

    Garment tee;
    REQUIRE(buildBlock("tee", tee, nullptr, body));
    const auto clean = checkPattern(tee, &body);
    for (const PatternIssue& i : clean)
    {
        CAPTURE(i.message);
        CHECK(i.level == PatternIssue::Info);
    }

    // A dropped-shoulder top drafts its shoulder past the shoulder point, so its flat cap is fine.
    Garment drop;
    REQUIRE(buildBlock("tee", drop, nullptr, body, {{"sleeveStyle", float(SleeveStyle::DropShoulder)}}));
    CHECK(count(checkPattern(drop, &body), PatternIssue::Warning, "shoulder point") == 0);

    // The same flat cap on a set-in shoulder: what a pattern maker spots at once.
    Garment flat = tee;
    for (Panel& p : flat.panels)
        if (p.name.rfind("Sleeve", 0) == 0)
            for (Vec2& v : p.outline)
                if (v[1] < 0.f)
                    v[1] *= 0.15f;
    const auto flatIssues = checkPattern(flat, &body);
    CHECK(count(flatIssues, PatternIssue::Warning, "shoulder point") == 2);
    CHECK(count(flatIssues, PatternIssue::Error, "shorter than the armhole") == 2);
    for (const PatternIssue& i : flatIssues)
        CHECK_FALSE(i.tip.empty());

    // No ease round the chest on a body bigger than the pattern.
    Garment tight;
    REQUIRE(buildBlock("tee", tight, nullptr, body, {{"ease", 0.f}}));
    Body big = body;
    big.chest = 1.1f;
    CHECK(count(checkPattern(tight, &big), PatternIssue::Error, "smaller round the chest") == 1);

    // A seam whose sides were cut different lengths.
    Garment off = tee;
    off.panels[0].outline[0][1] -= 0.04f;
    CHECK(count(checkPattern(off, &body), PatternIssue::Warning, "differ by") >= 1);
}

TEST_CASE("sleeve templates build and pass the pattern check")
{
    const Body body;
    for (const char* id : {"dropShoulderTop", "shirt", "peasantBlouse", "puffBlouse", "poetShirt", "bellSleeveDress", "flutterTop",
                           "capSleeveTop", "batwingTop", "legOfMuttonBlouse", "raglanTee", "marieBlouse", "flounceSleeveTop",
                           "slitSleeveTop", "tieredSleeveBlouse", "petalSleeveTop", "capeSleeveTop"})
    {
        CAPTURE(std::string(id));
        Garment g;
        REQUIRE(buildBlock(id, g, nullptr, body));
        CHECK(buildClothMesh(g).tris.size() > 100);
        for (const PatternIssue& i : checkPattern(g, &body))
        {
            CAPTURE(i.message);
            CHECK(i.level != PatternIssue::Error);
        }
    }
}
