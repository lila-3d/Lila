#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/DeformWeights.h"
#include "scene/Deformers.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using namespace deformtest;
namespace fs = std::filesystem;

namespace
{
std::vector<float> floats(const json& j) { return j.get<std::vector<float>>(); }

fields::Layer layer(fields::Shape s, double softness = 0.0)
{
    fields::Layer l;
    l.shape = s;
    l.softness = softness;
    return l;
}
} // namespace

TEST_CASE("fields: shapes, falloff, remap and blend modes")
{
    using namespace fields;
    CHECK(falloff(0.0, 0.5) == 1.0);
    CHECK(falloff(0.5, 0.5) == 1.0);
    CHECK(falloff(0.75, 0.5) == doctest::Approx(0.5));
    CHECK(falloff(1.0, 0.5) == 0.0);
    CHECK(falloff(0.3, 1.0) == doctest::Approx(1.0 - (0.09 * (3.0 - 0.6))));

    CHECK(shapeValue(layer(Shape::Sphere), {0.5, 0, 0}) == 1.0);
    CHECK(shapeValue(layer(Shape::Sphere), {0.8, 0.8, 0}) == 0.0);
    CHECK(shapeValue(layer(Shape::Box), {0.8, 0.8, 0.8}) == 1.0);
    CHECK(shapeValue(layer(Shape::Box), {1.1, 0, 0}) == 0.0);
    Layer cap = layer(Shape::Capsule);
    cap.length = 4.0;
    CHECK(shapeValue(cap, {0.5, 1.9, 0}) == 1.0);
    CHECK(shapeValue(cap, {0.5, 3.5, 0}) == 0.0);
    CHECK(shapeValue(layer(Shape::Plane), {5, 0.5, 5}) == 1.0);
    CHECK(shapeValue(layer(Shape::Linear), {0, 0, 0}) == doctest::Approx(0.5));
    CHECK(shapeValue(layer(Shape::Linear), {0, 2, 0}) == 1.0);
    CHECK(shapeValue(layer(Shape::Radial), {0, 0, -1}) == doctest::Approx(0.25));
    Layer cone = layer(Shape::Cone);
    cone.length = 2.0;
    CHECK(shapeValue(cone, {0.1, 1.0, 0}) == 1.0);
    CHECK(shapeValue(cone, {1.5, 1.0, 0}) == 0.0);
    CHECK(shapeValue(cone, {0, -1, 0}) == 0.0);

    CHECK(blend(Blend::Add, 0.25, 0.5, 1.0) == doctest::Approx(0.75));
    CHECK(blend(Blend::Add, 0.75, 0.5, 1.0) == 1.0);
    CHECK(blend(Blend::Subtract, 0.75, 0.5, 0.5) == doctest::Approx(0.5));
    CHECK(blend(Blend::Subtract, 0.25, 0.5, 1.0) == 0.0);
    CHECK(blend(Blend::Multiply, 0.8, 0.5, 1.0) == doctest::Approx(0.4));
    CHECK(blend(Blend::Multiply, 0.8, 0.5, 0.5) == doctest::Approx(0.6));
    CHECK(blend(Blend::Max, 0.2, 0.6, 0.5) == doctest::Approx(0.4));
    CHECK(blend(Blend::Min, 0.8, 0.2, 1.0) == doctest::Approx(0.2));
    CHECK(blend(Blend::Replace, 0.8, 0.2, 0.25) == doctest::Approx(0.65));

    Layer r;
    r.inMin = 0.2;
    r.inMax = 0.6;
    CHECK(remap(r, 0.4) == doctest::Approx(0.5));
    CHECK(remap(r, 0.9) == 1.0);
    r.invert = true;
    CHECK(remap(r, 0.4) == doctest::Approx(0.5));
    CHECK(remap(r, 0.1) == 1.0);
    r.invert = false;
    r.curve = softSelect::Curve({{0.0, 0.0, softSelect::Interp::Linear}, {1.0, 0.5, softSelect::Interp::Linear}});
    CHECK(remap(r, 0.6) == doctest::Approx(0.5));

    double a = noise({0.3, 1.7, -2.2}, 7, 3), b = noise({0.3, 1.7, -2.2}, 7, 3), c = noise({0.3, 1.7, -2.2}, 8, 3);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a >= 0.0);
    CHECK(a <= 1.0);

    Layer roundTrip = Layer::fromJson({{"type", "noise"}, {"seed", 3}, {"translate", v3(1, 2, 3)}, {"blend", "min"}});
    Layer back = Layer::fromJson(roundTrip.toJson());
    CHECK(back.kind == Kind::Noise);
    CHECK(back.seed == 3);
    CHECK(back.blend == Blend::Min);
    CHECK(back.translate == glm::dvec3(1, 2, 3));
    CHECK_THROWS(Layer::fromJson({{"shape", "torus"}}));
}

TEST_CASE("fields: a layer stack evaluates in world space and follows the field's transform")
{
    run("newScene");
    std::string f = run("field", {{"args", json::array()}, {"position", v3(10, 0, 0)}, {"radius", 2.0}});
    std::string cube = run("polyCube", {{"name", "Box"}})[0];
    auto e = fields::evaluator(graph(), graph().find(f)->id());
    REQUIRE(e);
    std::vector<glm::dvec3> pts = {{10, 0, 0}, {11.5, 0, 0}, {13, 0, 0}};
    std::vector<float> v = fields::evaluate(*e, pts, nullptr);
    CHECK(v[0] == 1.0f);
    CHECK(v[1] > 0.0f);
    CHECK(v[1] < 1.0f);
    CHECK(v[2] == 0.0f);

    run("setAttr", {{"args", {f + ".translateX", 13.0}}});
    v = fields::evaluate(*fields::evaluator(graph(), graph().find(f)->id()), pts, nullptr);
    CHECK(v[0] == 0.0f);
    CHECK(v[2] == 1.0f);

    // A box layer subtracted on top, then removed again with undo.
    CHECK(run("addFieldLayer", {{"args", {f}}, {"shape", "box"}, {"blend", "subtract"}, {"params", {{"scale", v3(0.1, 0.1, 0.1)}}}}) == 1);
    v = fields::evaluate(*fields::evaluator(graph(), graph().find(f)->id()), pts, nullptr);
    CHECK(v[2] == 0.0f);
    CHECK(run("fieldLayers", {{"args", {f}}}).size() == 2);
    run("setFieldLayer", {{"args", {f}}, {"index", 1}, {"params", {{"opacity", 0.5}}}});
    v = fields::evaluate(*fields::evaluator(graph(), graph().find(f)->id()), pts, nullptr);
    CHECK(v[2] == doctest::Approx(0.5f));
    run("moveFieldLayer", {{"args", {f}}, {"index", 1}, {"to", 0}});
    CHECK(run("fieldLayers", {{"args", {f}}})[0]["shape"] == "box");
    run("undo");
    run("undo");
    run("undo");
    CHECK(run("fieldLayers", {{"args", {f}}}).size() == 1);

    // Components and painted layers index the mesh's points.
    std::string g2 = run("field", {{"args", json::array()}, {"shape", "none"}});
    run("addFieldLayer", {{"args", {g2}}, {"type", "components"}, {"points", {0, 2}}});
    run("addFieldLayer", {{"args", {g2}}, {"type", "painted"}, {"blend", "add"}, {"params", {{"values", {0.0, 0.25}}}}});
    std::vector<float> s = floats(run("sampleField", {{"args", {g2, cube}}}));
    REQUIRE(s.size() == 8);
    CHECK(s[0] == 1.0f);
    CHECK(s[1] == doctest::Approx(0.25f));
    CHECK(s[2] == 1.0f);
    CHECK(s[3] == 0.0f);
}

TEST_CASE("fields: one field drives a deformer and soft selection identically")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Box"}})[0];
    std::vector<glm::dvec3> rest = points(cube);
    json sm = run("softMod", {{"args", {cube}}, {"radius", 0.0}});
    std::string handle = sm[0], mod = sm[1];
    // A sphere around the first corner, hard edged.
    std::string f = run("field", {{"args", json::array()}, {"position", json::array({rest[0].x, rest[0].y, rest[0].z})}, {"radius", 0.1}});
    run("setFieldLayer", {{"args", {f}}, {"params", {{"softness", 0.0}}}});
    run("connectField", {{"args", {f, mod}}});
    run("setAttr", {{"args", {handle + ".translateY", 5.0}}});
    std::vector<glm::dvec3> moved = points(cube);
    CHECK(near(moved[0], rest[0] + glm::dvec3(0, 5, 0)));
    for (size_t i = 1; i < rest.size(); ++i)
        CHECK(near(moved[i], rest[i]));

    std::vector<float> sampled = floats(run("sampleField", {{"args", {f, cube}}}));
    PrefStore::get().set("general", "/softSelect/enabled", true);
    PrefStore::get().set("general", "/softSelect/radius", 1000.0);
    run("softSelectField", {{"args", {f}}});
    graph().evaluate();
    MeshPtr m = graph().find(cube)->as<MeshPtr>(MeshNode::kOutMesh);
    std::vector<float> soft = softWeights(*m, glm::dmat4(1.0), {0, 1, 2, 3, 4, 5, 6, 7});
    for (size_t i = 0; i < soft.size(); ++i)
        CHECK(soft[i] == doctest::Approx(sampled[i]));
    run("softSelectField");
    PrefStore::get().set("general", "/softSelect/enabled", false);

    // Replace mode ignores the painted map; multiply keeps it.
    run("setDeformerWeights", {{"args", {mod}}, {"points", {0}}, {"value", 0.5}});
    CHECK(near(points(cube)[0], rest[0] + glm::dvec3(0, 2.5, 0)));
    run("connectField", {{"args", {f, mod}}, {"mode", "replace"}});
    CHECK(near(points(cube)[0], rest[0] + glm::dvec3(0, 5, 0)));
    run("disconnectField", {{"args", {mod}}});
    CHECK(near(points(cube)[1], rest[1] + glm::dvec3(0, 5, 0)));
}

TEST_CASE("deformer membership, timing and weight map commands")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Box"}})[0];
    std::vector<glm::dvec3> rest = points(cube);
    json sm = run("softMod", {{"args", {cube}}, {"radius", 0.0}});
    std::string handle = sm[0], mod = sm[1];
    run("setAttr", {{"args", {handle + ".translateY", 2.0}}});

    run("setDeformerMembership", {{"args", {mod}}, {"points", {1, 3}}});
    CHECK(run("deformerMembership", {{"args", {mod}}}) == json::array({1, 3}));
    std::vector<glm::dvec3> p = points(cube);
    CHECK(near(p[0], rest[0]));
    CHECK(near(p[1], rest[1] + glm::dvec3(0, 2, 0)));
    run("setDeformerMembership", {{"args", {mod}}, {"mode", "add"}, {"points", {0}}});
    CHECK(near(points(cube)[0], rest[0] + glm::dvec3(0, 2, 0)));
    run("setDeformerMembership", {{"args", {mod}}, {"mode", "remove"}, {"points", {1}}});
    CHECK(run("deformerMembership", {{"args", {mod}}}) == json::array({0, 3}));
    run("setDeformerMembership", {{"args", {mod}}, {"mode", "clear"}});
    CHECK(run("deformerMembership", {{"args", {mod}}}).is_null());
    run("undo");
    CHECK(run("deformerMembership", {{"args", {mod}}}) == json::array({0, 3}));
    run("setDeformerMembership", {{"args", {mod}}, {"mode", "clear"}});

    json timing = run("deformerTiming", {{"args", {cube}}});
    REQUIRE(timing["deformers"].size() == 1);
    CHECK(timing["deformers"][0]["deformer"] == mod);
    CHECK(timing["deformers"][0]["ms"].get<double>() >= 0.0);

    run("floodDeformerWeights", {{"args", {mod}}, {"value", 0.25}});
    std::vector<float> w = floats(run("deformerWeights", {{"args", {mod}}}));
    CHECK(w[5] == doctest::Approx(0.25f));
    run("floodDeformerWeights", {{"args", {mod}}, {"value", 1.0}, {"points", {0}}});
    run("smoothDeformerWeights", {{"args", {mod}}});
    w = floats(run("deformerWeights", {{"args", {mod}}}));
    CHECK(w[0] < 1.0f);
    CHECK(*std::max_element(w.begin() + 1, w.end()) > 0.25f);

    // Mirror across x: every -x point takes its +x partner's weight.
    std::vector<float> ramp(rest.size());
    for (size_t i = 0; i < rest.size(); ++i)
        ramp[i] = rest[i].x > 0 ? float(i) / 10.0f : 0.0f;
    run("setDeformerWeights", {{"args", {mod}}, {"weights", ramp}});
    run("mirrorDeformerWeights", {{"args", {mod}}, {"axis", "x"}});
    w = floats(run("deformerWeights", {{"args", {mod}}}));
    for (size_t i = 0; i < rest.size(); ++i)
        for (size_t j = 0; j < rest.size(); ++j)
            if (rest[i].x < 0 && near(rest[j], rest[i] * glm::dvec3(-1, 1, 1)))
                CHECK(w[i] == doctest::Approx(w[j]));

    // Copy onto another topology by closest point, and export/import by file.
    std::string sphere = run("polySphere", {{"name", "Ball"}})[0];
    std::string other = run("deformer", {{"args", {sphere}}, {"type", "softMod"}})[0];
    run("floodDeformerWeights", {{"args", {mod}}, {"value", 0.5}});
    run("copyDeformerWeights", {{"args", {mod, other}}});
    for (float x : floats(run("deformerWeights", {{"args", {other}}})))
        CHECK(x == doctest::Approx(0.5f));
    fs::path file = fs::temp_directory_path() / "lila_deform_tests" / "weights.json";
    fs::create_directories(file.parent_path());
    run("floodDeformerWeights", {{"args", {mod}}, {"value", 0.75}});
    run("exportDeformerWeights", {{"args", {mod}}, {"file", file.string()}});
    run("importDeformerWeights", {{"args", {other}}, {"file", file.string()}});
    for (float x : floats(run("deformerWeights", {{"args", {other}}})))
        CHECK(x == doctest::Approx(0.75f));
}

TEST_CASE("correction: tangent-space fixes ride on the deformation below")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Box"}})[0];
    std::string target = run("polyCube", {{"name", "Fix"}})[0];
    std::vector<glm::dvec3> rest = points(cube);
    json sm = run("softMod", {{"args", {cube}}, {"radius", 0.0}});
    std::string handle = sm[0];
    std::string corr = run("correction", {{"args", {cube}}})[0];
    CHECK(run("deformers", {{"args", {cube}}}).back() == corr);
    for (size_t i = 0; i < rest.size(); ++i)
        CHECK(near(points(cube)[i], rest[i]));

    // Sculpt: push point 0 out by (1, 2, 3).
    Graph& g = graph();
    PlugRef plug = modifiers::editPlug(g, g.find(target)->id());
    auto m = std::make_shared<geo::Mesh>(*std::get<MeshPtr>(g.value(plug)));
    m->setPosition(0, m->position(0) + glm::dvec3(1, 2, 3));
    g.setValue(plug, MeshPtr(m));
    CHECK(run("setCorrection", {{"args", {corr, target}}}) == 1);
    CHECK(near(points(cube)[0], rest[0] + glm::dvec3(1, 2, 3)));
    CHECK(near(points(cube)[1], rest[1]));

    // Rotate everything below: the fix turns with it.
    run("setAttr", {{"args", {handle + ".rotateY", 90.0}}});
    glm::dmat4 r = glm::rotate(glm::dmat4(1.0), glm::radians(90.0), glm::dvec3(0, 1, 0));
    glm::dvec3 pivot = glm::dvec3(g.find(handle)->as<glm::dmat4>(Transform::kWorldMatrix)[3]);
    auto turn = [&](const glm::dvec3& p) { return pivot + glm::dvec3(r * glm::dvec4(p - pivot, 0.0)); };
    CHECK(near(points(cube)[0], turn(rest[0] + glm::dvec3(1, 2, 3)), 1e-6));
    CHECK(near(points(cube)[2], turn(rest[2]), 1e-6));

    // Edit mode round trip keeps the result, reset clears it.
    std::string sculpt = run("editCorrection", {{"args", {corr}}, {"on", true}});
    CHECK(g.find(sculpt));
    run("editCorrection", {{"args", {corr}}, {"on", false}});
    CHECK_FALSE(g.find(sculpt));
    CHECK(near(points(cube)[0], turn(rest[0] + glm::dvec3(1, 2, 3)), 1e-6));
    run("resetCorrection", {{"args", {corr}}});
    CHECK(near(points(cube)[0], turn(rest[0]), 1e-6));
}
