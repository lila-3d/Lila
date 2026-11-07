#include <doctest/doctest.h>

#include "TestUtil.h"
#include "scene/AnimKeyOps.h"
#include "scene/CurveModifiers.h"
#include "scene/Graph.h"

#include <cmath>

using namespace lila;
using namespace animtest;
using namespace lila::scene::anim;

namespace
{
Curve ramp(std::initializer_list<std::pair<double, double>> keys)
{
    Curve c;
    for (auto [t, v] : keys)
        c.insert({.time = t, .value = v});
    return c;
}

void keyOpsLocator()
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("spaceLocator", {{"name", "loc"}});
    for (auto [t, v] : {std::pair{0.0, 0.0}, {1.0, 10.0}, {2.0, 4.0}, {3.0, 8.0}})
        run("setKeyframe", {{"args", {"loc.ty"}}, {"time", t}, {"value", v}});
}

json values(const char* plug)
{
    return run("keyframe", {{"args", {plug}}, {"query", true}, {"valueChange", true}});
}
} // namespace

TEST_CASE("transformKeys scales values and times about their pivots")
{
    Curve c = ramp({{0.0, 0.0}, {1.0, 10.0}, {2.0, 4.0}});
    CHECK(transformKeys(c, {1.0, 2.0}, {.timeScale = 2.0, .timePivot = 1.0, .valueScale = 0.5, .valuePivot = 4.0}, 30.0) == 2);
    REQUIRE(c.keys.size() == 3);
    CHECK(c.keys[1].time == doctest::Approx(1.0));
    CHECK(c.keys[1].value == doctest::Approx(7.0));
    CHECK(c.keys[2].time == doctest::Approx(3.0));
    CHECK(c.keys[2].value == doctest::Approx(4.0));
    CHECK(c.keys[0].value == doctest::Approx(0.0));
}

TEST_CASE("transformKeys falloff moves neighbours by less with distance and leaves far keys alone")
{
    Curve c = ramp({{0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0}, {3.0, 0.0}});
    transformKeys(c, {1.0}, {.valueOffset = 10.0, .falloff = 1.0}, 30.0);
    CHECK(c.keys[2].value == doctest::Approx(10.0));
    CHECK(c.keys[1].value > 0.0);
    CHECK(c.keys[1].value < 10.0);
    CHECK(c.keys[0].value == doctest::Approx(0.0));
    CHECK(c.keys[3].value == doctest::Approx(0.0));
    CHECK(falloffWeight(0.0, 1.0) == doctest::Approx(1.0));
    CHECK(falloffWeight(0.5, 1.0) == doctest::Approx(0.5));
}

TEST_CASE("transformKeys with snap lands scaled keys on whole frames, and moved keys replace what was there")
{
    Curve c = ramp({{0.0, 0.0}, {1.0, 1.0}, {2.0, 2.0}});
    transformKeys(c, {2.0}, {.timeScale = 0.5, .timePivot = 0.0, .snap = true}, 24.0);
    REQUIRE(c.keys.size() == 2);
    CHECK(c.keys[1].time == doctest::Approx(1.0));
    CHECK(c.keys[1].value == doctest::Approx(2.0));
}

TEST_CASE("blendValues: neighbour, push/pull, default")
{
    std::vector<double> t = {0, 1, 2}, v = {0, 4, 10};
    std::vector<char> sel = {0, 1, 0};
    CHECK(blendValues(t, v, sel, BlendMode::Neighbour, 1.0, 0)[1] == doctest::Approx(10));
    CHECK(blendValues(t, v, sel, BlendMode::Neighbour, -0.5, 0)[1] == doctest::Approx(2));
    // Line from 0 to 10 passes 5 at t = 1: pull lands there, push doubles the 1 below it.
    CHECK(blendValues(t, v, sel, BlendMode::PushPull, -1.0, 0)[1] == doctest::Approx(5));
    CHECK(blendValues(t, v, sel, BlendMode::PushPull, 1.0, 0)[1] == doctest::Approx(3));
    CHECK(blendValues(t, v, sel, BlendMode::Default, 1.0, 7)[1] == doctest::Approx(7));
    auto ease = blendValues(t, v, sel, BlendMode::Ease, 0.0, 0);
    CHECK(ease[1] == doctest::Approx(5));
    CHECK(blendValues(t, v, sel, BlendMode::Ease, 1.0, 0)[1] > 5.0);
    CHECK(blendValues(t, v, sel, BlendMode::Ease, -1.0, 0)[1] < 5.0);
    CHECK(ease[0] == 0);
    CHECK(ease[2] == 10);
}

TEST_CASE("filterKeys simplify keeps within tolerance with fewer keys")
{
    Curve c;
    for (int f = 0; f <= 60; ++f)
        c.insert({.time = f / 30.0, .value = std::sin(f / 30.0 * 3.0) * 10.0});
    Curve orig = c;
    filterKeys(c, 0.0, 2.0, {.filter = Filter::Simplify, .tolerance = 0.05}, 30.0);
    CHECK(c.keys.size() < orig.keys.size() / 3);
    for (int f = 0; f <= 60; ++f)
        CHECK(std::abs(c.evaluate(f / 30.0) - orig.evaluate(f / 30.0)) <= 0.05 + 1e-9);
}

TEST_CASE("filterKeys butterworth smooths noise and keeps the ends")
{
    Curve c;
    for (int f = 0; f <= 60; ++f)
        c.insert({.time = f / 30.0, .value = f / 6.0 + ((f % 2) ? 1.0 : -1.0)});
    const double first = c.keys.front().value, last = c.keys.back().value;
    filterKeys(c, 0.0, 2.0, {.filter = Filter::Butterworth, .cutoff = 3.0}, 30.0);
    CHECK(c.keys.front().value == doctest::Approx(first));
    CHECK(c.keys.back().value == doctest::Approx(last));
    double rough = 0.0;
    for (size_t i = 1; i + 1 < c.keys.size(); ++i)
        rough = std::max(rough, std::abs(c.keys[i + 1].value - 2.0 * c.keys[i].value + c.keys[i - 1].value));
    CHECK(rough < 0.5);
}

TEST_CASE("filterKeys resample and gaussian")
{
    Curve c = ramp({{0.0, 0.0}, {1.0, 10.0}});
    filterKeys(c, 0.0, 1.0, {.filter = Filter::Resample, .step = 5.0}, 30.0);
    CHECK(c.keys.size() == 7);
    Curve g = ramp({{0.0, 0.0}, {0.1, 10.0}, {0.2, 0.0}});
    filterKeys(g, 0.0, 0.2, {.filter = Filter::Gaussian, .width = 3.0}, 30.0);
    CHECK(g.keys.size() == 3);
    CHECK(g.keys[1].value < 10.0);
}

TEST_CASE("modifiers: stepped holds, limits clamp, noise is deterministic and centred")
{
    Curve c = ramp({{0.0, 0.0}, {1.0, 10.0}});
    std::vector<Modifier> stepped = {{.type = ModifierType::Stepped, .step = 0.25}};
    CHECK(evaluateModified(c, stepped, 0.4) == doctest::Approx(c.evaluate(0.25)));
    std::vector<Modifier> limits = {{.type = ModifierType::Limits, .max = 3.0}};
    CHECK(evaluateModified(c, limits, 0.9) == doctest::Approx(3.0));
    std::vector<Modifier> noise = {{.type = ModifierType::Noise, .amplitude = 2.0, .period = 0.1}};
    double sum = 0.0, peak = 0.0;
    for (int i = 0; i < 1000; ++i)
    {
        const double n = modifiedValue(noise, i * 0.0137, 0.0);
        sum += n;
        peak = std::max(peak, std::abs(n));
    }
    CHECK(std::abs(sum / 1000.0) < 0.3);
    CHECK(peak <= 2.0 + 1e-9);
    CHECK(peak > 0.5);
    CHECK(modifiedValue(noise, 0.3, 0.0) == modifiedValue(noise, 0.3, 0.0));
    CHECK(modifiersFromText(modifiersToText(noise)) == noise);
}

TEST_CASE("curveModifier changes the output, keeps the keys, and removes cleanly")
{
    keyOpsLocator();
    run("curveModifier", {{"args", {"loc.ty"}}, {"type", "limits"}, {"max", 5.0}});
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(5.0));
    CHECK(values("loc.ty")[1] == doctest::Approx(10.0));
    json q = run("curveModifier", {{"args", {"loc.ty"}}, {"query", true}});
    CHECK(q[0]["modifiers"][0]["type"] == "limits");
    run("curveModifier", {{"args", {"loc.ty"}}, {"remove", true}});
    at(0.0);
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(10.0));
    run("undo");
    at(0.0);
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(5.0));
}

TEST_CASE("transformKeys, blendKeys and filterCurve commands edit picked keys")
{
    keyOpsLocator();
    run("transformKeys", {{"keys", {{{"curve", "loc.ty"}, {"times", {1.0}}}}}, {"valueOffset", 2.0}});
    CHECK(values("loc.ty")[1] == doctest::Approx(12.0));
    run("transformKeys", {{"args", {"loc"}}, {"valueScale", 0.5}});
    CHECK(values("loc.ty") == json({0.0, 6.0, 2.0, 4.0}));
    run("blendKeys", {{"keys", {{{"curve", "loc.ty"}, {"times", {2.0}}}}}, {"mode", "neighbour"}, {"amount", 1.0}});
    CHECK(values("loc.ty")[2] == doctest::Approx(4.0));
    run("filterCurve", {{"args", {"loc"}}, {"filter", "resample"}, {"step", 15.0}});
    CHECK(run("keyframe", {{"args", {"loc.ty"}}, {"query", true}, {"keyCount", true}}) == 7);
}

TEST_CASE("rotation curves: per-axis blend and scale, time moves the whole key")
{
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("spaceLocator", {{"name", "r"}});
    at(0.0);
    run("setAttr", {{"args", {"r.rotate", 0, 0, 0}}});
    run("setKeyframe", {{"args", {"r.rotate"}}});
    at(1.0);
    run("setAttr", {{"args", {"r.rotate", 40, 20, 0}}});
    run("setKeyframe", {{"args", {"r.rotate"}}});
    run("convertRotationCurves", {{"args", {"r"}}, {"to", "smooth"}});
    CHECK(run("keyframe", {{"args", {"r.rotateY"}}, {"query", true}}) == json({0.0, 1.0}));
    run("transformKeys", {{"keys", {{{"curve", "r.rotateX"}, {"times", {1.0}}}}}, {"valueScale", 0.5}, {"timeOffset", 0.5}});
    CHECK(run("keyframe", {{"args", {"r.rotateX"}}, {"query", true}}) == json({0.0, 1.5}));
    at(1.5);
    CHECK(get("r.rx") == doctest::Approx(20.0));
    CHECK(get("r.ry") == doctest::Approx(20.0));
    run("blendKeys", {{"keys", {{{"curve", "r.rotateY"}, {"times", {1.5}}}}}, {"mode", "default"}, {"amount", 1.0}});
    at(0.0);
    at(1.5);
    CHECK(get("r.ry") == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(get("r.rx") == doctest::Approx(20.0));
}

TEST_CASE("keyTangent lock breaks and unifies handles")
{
    keyOpsLocator();
    run("keyTangent", {{"args", {"loc.ty"}}, {"time", 1.0}, {"inAngle", 30.0}});
    run("keyTangent", {{"args", {"loc.ty"}}, {"time", 1.0}, {"lock", false}});
    json q = run("keyTangent", {{"args", {"loc.ty"}}, {"time", 1.0}, {"query", true}});
    CHECK(q[0]["inAngle"].get<double>() == doctest::Approx(30.0));
    run("keyTangent", {{"args", {"loc.ty"}}, {"time", 1.0}, {"lock", true}});
    q = run("keyTangent", {{"args", {"loc.ty"}}, {"time", 1.0}, {"query", true}});
    CHECK(q[0]["inAngle"].get<double>() == doctest::Approx(q[0]["outAngle"].get<double>()));
}

TEST_CASE("currentTime update=False holds the pose, so a key set there copies it")
{
    keyOpsLocator();
    at(1.0);
    run("currentTime", {{"args", {2.0}}, {"update", false}});
    CHECK(get("loc.ty") == doctest::Approx(10.0));
    run("setKeyframe", {{"args", {"loc.ty"}}});
    CHECK(values("loc.ty")[2] == doctest::Approx(10.0));
    at(3.0);
    CHECK(get("loc.ty") == doctest::Approx(8.0));
    at(2.0);
    CHECK(get("loc.ty") == doctest::Approx(10.0));
}

TEST_CASE("a curve preview drives the output until cleared")
{
    keyOpsLocator();
    auto& g = scene::Graph::current();
    scene::NodeId id = g.find("loc_translateY")->id();
    Curve c = curveOf(*g.node(id));
    c.keys[1].value = 20.0;
    setPreview(g, id, c);
    at(1.0);
    CHECK(get("loc.ty") == doctest::Approx(20.0));
    clearPreviews(g);
    CHECK(get("loc.ty") == doctest::Approx(10.0));
}
