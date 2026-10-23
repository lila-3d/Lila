#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/SoftSelect.h"

using namespace lila;
using namespace lila::scene::softSelect;
using json = nlohmann::json;

namespace
{
/** A row of points 1 cm apart along X, joined by edges. */
geo::Mesh row(int n)
{
    geo::Mesh m;
    for (int i = 0; i < n; ++i)
        m.addPoint({double(i), 0.0, 0.0});
    for (int i = 0; i + 1 < n; ++i)
        m.addEdge(geo::Index(i), geo::Index(i + 1));
    return m;
}

void resetPrefs()
{
    PrefStore& p = PrefStore::get();
    p.remove("general", kCurve);
    p.remove("general", kFalloffMode);
    p.remove("general", kUserPresets);
    p.set("general", kEnabled, true);
    p.set("general", kRadius, 10.0);
}
} // namespace

TEST_CASE("falloff curve: default matches smoothstep and holds its ends")
{
    Curve c;
    for (double t : {0.0, 0.1, 0.3, 0.5, 0.77, 1.0})
        CHECK(c.evaluate(t) == doctest::Approx(1.0 - t * t * (3.0 - 2.0 * t)));
    CHECK(c.evaluate(-1.0) == doctest::Approx(1.0));
    CHECK(c.evaluate(2.0) == doctest::Approx(0.0));
    CHECK(Curve(std::vector<Key>{}) == Curve());
}

TEST_CASE("falloff curve: interpolation modes")
{
    Curve lin({{0.0, 1.0, Interp::Linear}, {1.0, 0.0, Interp::Linear}});
    CHECK(lin.evaluate(0.25) == doctest::Approx(0.75));

    Curve flat({{0.0, 0.8, Interp::Flat}, {0.5, 0.2, Interp::Flat}, {1.0, 0.0, Interp::Flat}});
    CHECK(flat.evaluate(0.49) == doctest::Approx(0.8));
    CHECK(flat.evaluate(0.51) == doctest::Approx(0.2));

    Curve smooth({{0.0, 1.0, Interp::Smooth}, {0.5, 0.0, Interp::Smooth}, {1.0, 1.0, Interp::Smooth}});
    CHECK(smooth.evaluate(0.25) == doctest::Approx(0.5));
    CHECK(smooth.evaluate(0.75) == doctest::Approx(0.5));

    // Each segment uses the interpolation of the key it starts at.
    Curve mixed({{0.0, 1.0, Interp::Linear}, {0.5, 0.5, Interp::Flat}, {1.0, 0.0, Interp::Linear}});
    CHECK(mixed.evaluate(0.25) == doctest::Approx(0.75));
    CHECK(mixed.evaluate(0.75) == doctest::Approx(0.5));

    // Keys are sorted and clamped into the unit square.
    Curve messy({{1.5, -2.0, Interp::Linear}, {-1.0, 3.0, Interp::Linear}});
    REQUIRE(messy.keys().size() == 2);
    CHECK(messy.keys()[0].pos == 0.0);
    CHECK(messy.keys()[0].value == 1.0);
    CHECK(messy.keys()[1].pos == 1.0);
    CHECK(messy.keys()[1].value == 0.0);
}

TEST_CASE("falloff curve: spline is monotone between keys and passes through them")
{
    Curve c({{0.0, 1.0, Interp::Spline}, {0.1, 0.95, Interp::Spline}, {0.15, 0.2, Interp::Spline}, {0.8, 0.15, Interp::Spline},
             {1.0, 0.0, Interp::Spline}});
    for (const Key& k : c.keys())
        CHECK(c.evaluate(k.pos) == doctest::Approx(k.value));
    double prev = c.evaluate(0.0);
    for (int i = 1; i <= 1000; ++i)
    {
        double v = c.evaluate(i / 1000.0);
        CHECK(v <= prev + 1e-12);
        CHECK(v >= 0.0);
        CHECK(v <= 1.0);
        prev = v;
    }

    // A local extremum key stays the extremum: no overshoot past it.
    Curve bump({{0.0, 0.0, Interp::Spline}, {0.5, 1.0, Interp::Spline}, {1.0, 0.0, Interp::Spline}});
    for (int i = 0; i <= 100; ++i)
        CHECK(bump.evaluate(i / 100.0) <= 1.0);
}

TEST_CASE("falloff curve: editing and json")
{
    Curve c;
    int i = c.addKey({0.5, 0.25, Interp::Linear});
    CHECK(i == 1);
    CHECK(c.evaluate(0.5) == doctest::Approx(0.25));
    i = c.moveKey(1, 0.2, 0.9);
    CHECK(i == 1);
    CHECK(c.keys()[1].pos == doctest::Approx(0.2));
    c.setInterp(1, Interp::Flat);
    CHECK(c.evaluate(0.6) == doctest::Approx(0.9));

    Curve back = Curve::fromJson(c.toJson());
    CHECK(back == c);
    CHECK(Curve::fromJson(json::parse(R"([{"pos": 0, "value": 0.5, "interp": "linear"}, {"pos": 1, "value": 0.5}])")).evaluate(0.3) ==
          doctest::Approx(0.5));
    CHECK(Curve::fromJson("nonsense") == Curve());

    c.removeKey(1);
    CHECK(c.keys().size() == 2);
    c.removeKey(0);
    CHECK(c.keys().size() == 2);
}

TEST_CASE("RDP simplification")
{
    std::vector<glm::dvec2> line;
    for (int i = 0; i <= 10; ++i)
        line.push_back({i / 10.0, 1.0 - i / 10.0});
    auto s = simplify(line, 0.01);
    REQUIRE(s.size() == 2);
    CHECK(s.front() == line.front());
    CHECK(s.back() == line.back());

    std::vector<glm::dvec2> corner = {{0, 0}, {0.25, 0.25}, {0.5, 0.5}, {0.75, 0.25}, {1, 0}};
    auto k = simplify(corner, 0.01);
    REQUIRE(k.size() == 3);
    CHECK(k[1] == glm::dvec2(0.5, 0.5));
    CHECK(simplify(corner, 1.0).size() == 2);

    // A drawn stroke, backwards and short of the edges, still makes a curve over the whole range.
    std::vector<glm::dvec2> stroke;
    for (int i = 90; i >= 10; --i)
        stroke.push_back({i / 100.0, 1.0 - i / 100.0});
    Curve c = curveFromStroke(stroke, 0.01);
    CHECK(c.keys().front().pos == 0.0);
    CHECK(c.keys().back().pos == 1.0);
    CHECK(c.keys().size() <= 4);
    CHECK(c.evaluate(0.0) == doctest::Approx(0.9));
    CHECK(c.evaluate(0.5) == doctest::Approx(0.5).epsilon(0.02));
    CHECK(c.evaluate(1.0) == doctest::Approx(0.1));
}

TEST_CASE("falloff presets")
{
    struct Expect
    {
        const char* name;
        double at0, atHalf, at1;
    };
    for (Expect e : {Expect{"Smooth", 1, 0.5, 0}, {"Linear", 1, 0.5, 0}, {"Sharp", 1, 0.12, 0}, {"Dome", 1, 0.87, 0},
                     {"Bell", 1, 0.3, 0}, {"Spike", 1, 0.03, 0}, {"Plateau", 1, 1, 0}, {"Inverse", 0, 0.5, 1}, {"Ring", 1, 0.7, 0}})
    {
        CAPTURE(e.name);
        const Preset* p = findBuiltin(e.name);
        REQUIRE(p);
        CHECK(p->curve.evaluate(0.0) == doctest::Approx(e.at0));
        CHECK(p->curve.evaluate(0.5) == doctest::Approx(e.atHalf));
        CHECK(p->curve.evaluate(1.0) == doctest::Approx(e.at1));
    }
    CHECK(builtinPresets().size() == 9);
    CHECK(builtinPresets().front().curve == Curve());
}

TEST_CASE("softWeights follows the curve and falloff mode")
{
    resetPrefs();
    geo::Mesh m = row(21);
    glm::dmat4 world(1.0);
    scene::components::Ids sel{0};

    auto w = scene::softWeights(m, world, sel);
    CHECK(w[0] == 1.0f);
    CHECK(w[5] == doctest::Approx(0.5));
    CHECK(w[10] == 0.0f);

    setCurrentCurve(findBuiltin("Plateau")->curve);
    w = scene::softWeights(m, world, sel);
    CHECK(w[5] == doctest::Approx(1.0));
    CHECK(w[0] == 1.0f);

    // Bend the row back on itself: point 20 sits 1 cm from point 0 in space but 20 cm away along the edges.
    m.setPosition(20, {0.0, 1.0, 0.0});
    setCurrentCurve(findBuiltin("Linear")->curve);
    w = scene::softWeights(m, world, sel);
    CHECK(w[20] == doctest::Approx(0.9));
    PrefStore::get().set("general", kFalloffMode, "surface");
    w = scene::softWeights(m, world, sel);
    CHECK(w[20] == 0.0f);
    CHECK(w[3] == doctest::Approx(0.7));

    PrefStore::get().set("general", kEnabled, false);
    w = scene::softWeights(m, world, sel);
    CHECK(w[3] == 0.0f);
    resetPrefs();
    PrefStore::get().set("general", kEnabled, false);
}

TEST_CASE("softSelectCurve command and user presets")
{
    resetPrefs();
    json r = ops::execute("softSelectCurve", {{"preset", "Linear"}});
    CHECK(Curve::fromJson(r["keys"]) == findBuiltin("Linear")->curve);
    CHECK(r["falloffMode"] == "volume");

    ops::execute("softSelectCurve", {{"keys", {{0, 1, "flat"}, {0.5, 0.25, "linear"}, {1, 0, "linear"}}}, {"save", "Mine"}});
    CHECK(currentCurve().evaluate(0.25) == doctest::Approx(1.0));
    REQUIRE(userPresets().size() == 1);
    CHECK(userPresets()[0].name == "Mine");

    ops::execute("softSelectCurve", {{"preset", "Smooth"}});
    CHECK(currentCurve() == Curve());
    ops::execute("softSelectCurve", {{"preset", "Mine"}});
    CHECK(currentCurve().evaluate(0.75) == doctest::Approx(0.125));

    r = ops::execute("softSelectCurve", {{"rename", "Mine"}, {"to", "Ours"}});
    CHECK(r["userPresets"] == json::array({"Ours"}));
    CHECK_THROWS_AS(ops::execute("softSelectCurve", {{"preset", "Mine"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("softSelectCurve", {{"save", "Smooth"}}), CommandError);
    ops::execute("softSelectCurve", {{"remove", "Ours"}});
    CHECK(userPresets().empty());
    CHECK_THROWS_AS(ops::execute("softSelectCurve", {{"falloffMode", "sideways"}}), CommandError);
    ops::execute("softSelectCurve", {{"falloffMode", "surface"}});
    CHECK(surfaceFalloff());
    resetPrefs();
    PrefStore::get().set("general", kEnabled, false);
}
