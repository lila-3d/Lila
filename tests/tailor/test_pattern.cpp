#include <doctest/doctest.h>

#include "Blocks.h"
#include "Pattern.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

using namespace tailor;

namespace
{
Panel rect(const char* name, float w, float h, Vec3 origin)
{
    Panel p;
    p.name = name;
    p.outline = {{0.f, 0.f}, {w, 0.f}, {w, h}, {0.f, h}};
    p.origin = origin;
    p.particleDistance = 0.02f;
    return p;
}

float dist(const Vec2& a, const Vec2& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

bool mirroredOutlines(const Panel& a, const Panel& b)
{
    const size_t n = a.outline.size();
    if (b.outline.size() != n)
        return false;
    for (size_t j = 0; j < n; ++j)
    {
        const Vec2& p = a.outline[(n - j) % n];
        if (std::abs(b.outline[j][0] + p[0]) > 1e-6f || std::abs(b.outline[j][1] - p[1]) > 1e-6f)
            return false;
    }
    return true;
}

float capEase(const Garment& g)
{
    for (const Seam& s : g.seams)
    {
        const bool sa = g.panels[size_t(s.a.front().panel)].name.rfind("Sleeve", 0) == 0;
        const bool sb = g.panels[size_t(s.b.front().panel)].name.rfind("Sleeve", 0) == 0;
        if (sa != sb && (s.a.size() > 1 || s.b.size() > 1))
            return seamSideLength(g, sa ? s.a : s.b) / seamSideLength(g, sa ? s.b : s.a);
    }
    return 0.f;
}
} // namespace

TEST_CASE("pattern edits keep seams on the same outline and a linked twin mirrored")
{
    Garment g;
    g.panels = {rect("FrontLeft", 0.3f, 0.5f, {0.05f, 1.f, 0.1f}), rect("Back", 0.6f, 0.5f, {-0.3f, 1.f, -0.1f})};
    Seam side;
    side.a = {{0, 1, 2}};
    side.b = {{1, 3, 0}};
    side.reversed = true;
    g.seams = {side};

    const int twin = mirrorPanel(g, 0, 0.f, true);
    REQUIRE(twin == 2);
    CHECK(g.panels[2].name == "FrontRight");
    CHECK(mirroredOutlines(g.panels[0], g.panels[2]));
    // Mirrored across x = 0: the twin's point 3 lands at the mirror of the source's point 1.
    const Vec3 p0 = placePatternPoint(g.panels[0], g.panels[0].outline[1]);
    const Vec3 q0 = placePatternPoint(g.panels[2], g.panels[2].outline[3]);
    CHECK(q0[0] == doctest::Approx(-p0[0]));
    CHECK(q0[1] == doctest::Approx(p0[1]));
    CHECK(q0[2] == doctest::Approx(p0[2]));

    // A seam on the twin along the mirror of the source's sewn edge.
    Seam twinSeam;
    twinSeam.a = {{2, 2, 3}};
    twinSeam.b = {{1, 1, 2}};
    g.seams.push_back(twinSeam);

    const Vec2 sewnFrom = g.panels[0].outline[1], sewnTo = g.panels[0].outline[2];
    insertOutlinePoint(g, 0, 1, {0.15f, -0.02f});
    REQUIRE(g.panels[0].outline.size() == 5);
    CHECK(mirroredOutlines(g.panels[0], g.panels[2]));
    CHECK(dist(g.panels[0].outline[size_t(g.seams[0].a[0].from)], sewnFrom) < 1e-6f);
    CHECK(dist(g.panels[0].outline[size_t(g.seams[0].a[0].to)], sewnTo) < 1e-6f);
    // The twin's seam still sews the mirror of the same edge.
    CHECK(dist(g.panels[2].outline[size_t(g.seams[1].a[0].from)], {-sewnTo[0], sewnTo[1]}) < 1e-6f);
    CHECK(dist(g.panels[2].outline[size_t(g.seams[1].a[0].to)], {-sewnFrom[0], sewnFrom[1]}) < 1e-6f);

    // Edits from the twin flow back.
    std::vector<Vec2> moved = g.panels[2].outline;
    moved[1][1] += 0.05f;
    setOutline(g, 2, moved);
    CHECK(mirroredOutlines(g.panels[2], g.panels[0]));
    CHECK(deleteOutlinePoint(g, 2, 4) == 0);
    CHECK(g.panels[0].outline.size() == 4);
    CHECK(mirroredOutlines(g.panels[0], g.panels[2]));

    // Removing a panel unlinks its twin and renumbers the seams.
    CHECK(removePanels(g, {0}) == 1);
    CHECK(g.panels.size() == 2);
    CHECK(g.panels[1].twin == -1);
    CHECK(g.seams.size() == 1);
    CHECK(g.seams[0].a[0].panel == 1);
    CHECK(g.seams[0].b[0].panel == 0);
}

TEST_CASE("a dart takes in its edge, sews its legs and leaves the edge's seam either side")
{
    Garment g;
    g.panels = {rect("Front", 0.4f, 0.6f, {-0.2f, 0.f, 0.1f}), rect("Waistband", 0.4f, 0.06f, {-0.2f, -0.08f, 0.1f})};
    Seam waist;
    waist.a = {{0, 0, 1}};
    waist.b = {{1, 3, 2}};
    waist.reversed = true;
    g.seams = {waist};

    const int dart = addDart(g, 0, 0, 0.5f, 0.04f, 0.12f);
    REQUIRE(dart == 1);
    const Panel& f = g.panels[0];
    REQUIRE(f.outline.size() == 7);
    CHECK(f.outline[2][1] == doctest::Approx(0.12f));
    CHECK(dist(f.outline[1], f.outline[2]) == doctest::Approx(dist(f.outline[2], f.outline[3])));
    REQUIRE(g.seams[0].a.size() == 2);
    CHECK(seamSideLength(g, g.seams[0].a) == doctest::Approx(0.36f));
    CHECK(addDart(g, 1, 0, 0.5f, 0.5f, 0.02f) == -1);

    const ClothMesh m = buildClothMesh(g);
    int dartPairs = 0;
    for (size_t k = 0; k < m.seamPairs.size(); ++k)
        dartPairs += m.seamOfPair[k] == dart;
    CHECK(dartPairs >= 5);
}

TEST_CASE("notches sew notch to notch")
{
    Garment g;
    g.panels = {rect("A", 0.4f, 0.3f, {-0.4f, 1.f, 0.f}), rect("B", 0.4f, 0.3f, {0.f, 1.f, 0.f})};
    Seam s;
    s.a = {{0, 1, 2}};
    s.b = {{1, 3, 0}};
    s.reversed = true;
    g.seams = {s};
    // Without notches the points at the same arc length meet; with them A's 10 cm mark meets B's 20 cm mark.
    const int na = addNotch(g, 0, {0.4f, 0.1f});
    const int nb = addNotch(g, 1, {0.f, 0.2f});
    CHECK(addNotch(g, 0, {0.401f, 0.1f}) == na);
    CHECK(splitSeamAtNotches(g, g.seams[0]).size() == 2);

    const ClothMesh m = buildClothMesh(g);
    auto vertexAt = [&](int panel, const Vec2& at) {
        for (size_t v = 0; v < m.uv.size(); ++v)
            if (m.vertPanel[v] == panel && dist(m.uv[v], at) < 1e-4f)
                return int(v);
        return -1;
    };
    const int va = vertexAt(0, g.panels[0].outline[size_t(na)]), vb = vertexAt(1, g.panels[1].outline[size_t(nb)]);
    REQUIRE(va >= 0);
    REQUIRE(vb >= 0);
    bool sewn = false;
    for (const auto& pr : m.seamPairs)
        sewn |= (pr[0] == va && pr[1] == vb) || (pr[0] == vb && pr[1] == va);
    CHECK(sewn);
}

TEST_CASE("grading keeps the sleeve cap's ease")
{
    Garment tee;
    REQUIRE(buildBlock("tee", tee));
    const float before = capEase(tee);
    REQUIRE(before > 1.f);

    // Plain proportional scaling drifts the ease: the cap runs across, the armhole mostly down.
    Garment plain = tee;
    for (Panel& p : plain.panels)
        for (Vec2& v : p.outline)
            v = {v[0] * 1.15f, v[1] * 0.97f};
    CHECK(std::abs(capEase(plain) - before) > 0.01f);

    Garment graded = tee;
    gradePattern(graded, 1.15f, 0.97f);
    CHECK(capEase(graded) == doctest::Approx(before).epsilon(0.002));
    for (size_t k = 0; k < graded.panels.size(); ++k)
        CHECK(graded.panels[k].wrapRadius == doctest::Approx(tee.panels[k].wrapRadius * 1.15f));
}

TEST_CASE("auto-sew closes edges that face each other, not hems side by side")
{
    Garment tee;
    REQUIRE(buildBlock("tee", tee, nullptr, Body{}, {{"sleeveLength", 0.f}}));
    auto key = [](const Seam& s) { return std::tuple(s.a[0].panel, s.a[0].from, s.a[0].to, s.b[0].panel, s.b[0].from, s.b[0].to); };
    std::set<std::tuple<int, int, int, int, int, int>> built;
    for (const Seam& s : tee.seams)
        built.insert(key(s));

    // The side seams come back; the hems and armholes, side by side round the body, stay open.
    Garment bare = tee;
    bare.seams.clear();
    const std::vector<Seam> found = suggestSeams(bare, 0.08f);
    CHECK(found.size() == 2);
    for (const Seam& s : found)
    {
        CHECK(built.count(key(s)) == 1);
        CHECK(s.reversed);
    }
    CHECK(suggestSeams(tee, 0.08f).empty());

    // Two flat panels laid side by side: the facing edges, and only those.
    Garment flat;
    flat.panels = {rect("L", 0.3f, 0.4f, {-0.32f, 1.f, 0.f}), rect("R", 0.3f, 0.4f, {0.02f, 1.f, 0.f})};
    const std::vector<Seam> pair = suggestSeams(flat, 0.08f);
    REQUIRE(pair.size() == 1);
    const SeamSide& l = pair[0].a[0].panel == 0 ? pair[0].a[0] : pair[0].b[0];
    CHECK(l.from == 1);
    CHECK(l.to == 2);
}

TEST_CASE("cloth brushes: pins hold, shrink spots gather, folds hold their angle")
{
    Garment g;
    Panel sheet = rect("Sheet", 0.4f, 0.4f, {-0.2f, 1.f, 0.f});
    g.panels = {sheet};

    // A pin resolves to the vertex nearest its pattern point and holds it while the rest falls.
    g.pins = {{0, {0.2f, 0.4f}, {0.f, 1.4f, 0.f}}};
    ClothMesh m = buildClothMesh(g);
    const std::vector<Pin> pins = resolvePins(m, g);
    REQUIRE(pins.size() == 1);
    CHECK(dist(m.uv[size_t(pins[0].vertex)], {0.2f, 0.4f}) < 0.015f);
    ClothSolver solver;
    solver.reset(m, g, SolverSettings{});
    solver.setPins(pins);
    for (int f = 0; f < 30; ++f)
        solver.step();
    const Vec3& held = solver.positions()[size_t(pins[0].vertex)];
    CHECK(std::hypot(held[0], held[1] - 1.4f, held[2]) < 0.01f);

    // A shrink spot scales the rest shape most at its middle and not at all past its radius.
    Panel& p = g.panels[0];
    p.shrinkSpots = {{{0.2f, 0.2f}, 0.1f, 0.8f}};
    CHECK(shrinkAt(p, {0.2f, 0.2f}) == doctest::Approx(0.8f));
    CHECK(shrinkAt(p, {0.35f, 0.2f}) == doctest::Approx(1.f));
    CHECK(shrinkAt(p, {0.25f, 0.2f}) > 0.8f);

    // A fold line keeps its angle: the two halves of a pinned-free sheet fold up off the flat.
    Garment fold;
    fold.panels = {rect("Sheet", 0.4f, 0.4f, {-0.2f, 1.f, 0.f})};
    fold.panels[0].internalLines = {{{0.2f, 0.f}, {0.2f, 0.4f}}};
    fold.panels[0].lineAngles = {1.5707963f};
    const ClothMesh fm = buildClothMesh(fold);
    REQUIRE(fm.creaseAngles.size() == fm.creaseEdges.size());
    REQUIRE_FALSE(fm.creaseEdges.empty());
    for (float a : fm.creaseAngles)
        CHECK(a == doctest::Approx(1.5707963f));
    SolverSettings zeroG;
    zeroG.gravity = {0.f, 0.f, 0.f};
    ClothSolver fs;
    fs.reset(fm, fold, zeroG);
    for (int f = 0; f < 120; ++f)
        fs.step();
    // The free corners lift out of the sheet's plane as the crease closes towards 90 degrees.
    float off = 0.f;
    for (const Vec3& q : fs.positions())
        off = std::max(off, std::abs(q[2]));
    CHECK(off > 0.05f);
}

TEST_CASE("pins attached to the body ride it as it moves")
{
    Garment g;
    g.panels = {rect("Sheet", 0.4f, 0.4f, {-0.2f, 1.f, 0.1f})};
    // A body: one quad just behind the sheet's top edge.
    std::vector<Vec3> body{{-0.3f, 1.3f, 0.08f}, {0.3f, 1.3f, 0.08f}, {0.3f, 1.5f, 0.08f}, {-0.3f, 1.5f, 0.08f}};
    const std::vector<std::array<int, 3>> tris{{0, 1, 2}, {0, 2, 3}};
    g.pins = {{0, {0.2f, 0.4f}, {0.f, 1.4f, 0.1f}}};
    attachPins(g, body, tris);
    REQUIRE(g.pins[0].bodyTri >= 0);
    CHECK(g.pins[0].bodyTris == 2);

    const ClothMesh m = buildClothMesh(g);
    // Where it stands, the pin aims at its own target.
    const std::vector<Pin> still = resolvePins(m, g, &body, &tris);
    REQUIRE(still.size() == 1);
    CHECK(still[0].target[0] == doctest::Approx(0.f).epsilon(1e-4));
    CHECK(still[0].target[2] == doctest::Approx(0.1f));

    // The body steps 30 cm to the side and turns a quarter turn about its vertical: the pin goes with it.
    std::vector<Vec3> moved = body;
    for (Vec3& p : moved)
        p = {p[2] + 0.3f, p[1], -p[0]};
    const std::vector<Pin> carried = resolvePins(m, g, &moved, &tris);
    REQUIRE(carried.size() == 1);
    CHECK(carried[0].target[0] == doctest::Approx(0.1f + 0.3f).epsilon(0.01));
    CHECK(carried[0].target[1] == doctest::Approx(1.4f).epsilon(0.01));
    CHECK(carried[0].target[2] == doctest::Approx(0.f).epsilon(0.01));

    // A different body (another triangle count) leaves the pin where it was painted.
    const std::vector<std::array<int, 3>> one{{0, 1, 2}};
    CHECK(resolvePins(m, g, &moved, &one)[0].target[0] == doctest::Approx(0.f).epsilon(1e-4));
}

TEST_CASE("auto-sew with the body sews shoulders, not hems, necklines or armholes")
{
    const Body b;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(b, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.2f);
    Garment tee;
    REQUIRE(buildBlock("tee", tee, nullptr, b, {{"sleeveLength", 0.f}}));
    auto key = [](const Seam& s) { return std::tuple(s.a[0].panel, s.a[0].from, s.a[0].to, s.b[0].panel, s.b[0].from, s.b[0].to); };
    std::set<std::tuple<int, int, int, int, int, int>> built;
    for (const Seam& s : tee.seams)
        built.insert(key(s));
    Garment bare = tee;
    bare.seams.clear();
    const std::vector<Seam> found = suggestSeams(bare, 0.08f, &sdf);
    CHECK(found.size() == 4);
    for (const Seam& s : found)
        CHECK(built.count(key(s)) == 1);
}

TEST_CASE("auto-sew with the body only proposes seams the blocks have")
{
    const Body b;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    mannequin(b, pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.01f, 0.2f);
    auto key = [](const Seam& s) { return std::tuple(s.a[0].panel, s.a[0].from, s.a[0].to, s.b[0].panel, s.b[0].from, s.b[0].to); };
    // Not the poncho: its sides hang open facing each other, and auto-sew closes them as it would any facing pair.
    for (const char* id : {"tee", "tunic", "robe", "skirt", "cloak", "tabard", "shirt", "raglanTee"})
    {
        CAPTURE(id);
        Garment g;
        REQUIRE(buildBlock(id, g, nullptr, b));
        std::set<std::tuple<int, int, int, int, int, int>> built;
        for (const Seam& s : g.seams)
            if (s.a.size() == 1 && s.b.size() == 1)
            {
                built.insert(key(s));
                Seam r = s;
                std::swap(r.a, r.b);
                built.insert(key(r));
            }
        Garment bare = g;
        bare.seams.clear();
        int wrong = 0;
        for (const Seam& s : suggestSeams(bare, 0.08f, &sdf))
            wrong += built.count(key(s)) ? 0 : 1;
        CHECK(wrong == 0);
    }
}

