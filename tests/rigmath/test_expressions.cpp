#include <doctest/doctest.h>

#include "io/SceneFile.h"
#include "ops/Command.h"
#include "rigmath/ExpressionVm.h"
#include "rigmath/Expressions.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Transform.h"

#include <cmath>
#include <fstream>
#include <iterator>

using namespace lila;
using namespace lila::scene;
namespace ex = lila::scene::expressions;

namespace expressionTests
{
json eCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
double value(const std::string& plug) { return eCmd("getAttr", {{"args", {plug}}}).get<double>(); }
} // namespace expressionTests
using namespace expressionTests;

TEST_CASE("Expressions parse Python syntax; the subset compiles, the rest is flagged for Python")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    std::string why;
    CHECK(ex::compiles("ctrl.ry * 0.5 + clamp(ctrl.tx, 0, 90)", &why));
    CHECK(ex::compiles("-ctrl.tx ** 2 if ctrl.ty > 1 and not ctrl.tz == 0 else 3 % 2", &why));
    CHECK_FALSE(ex::compiles("fabs(ctrl.tx)", &why));
    CHECK(why.find("fabs") != std::string::npos);
    CHECK(ex::compiles("noise(ctrl.tx) + random(3) + smoothstep(0, 1, time) + frame", &why));
    CHECK_FALSE(ex::compiles("random(1, 2)", &why));
    CHECK(why.find("1 or 3") != std::string::npos);
    CHECK_FALSE(ex::compiles("[x for x in range(3)][0]", &why));
    CHECK_FALSE(ex::compiles("1 < ctrl.tx < 2", &why));
    CHECK_FALSE(ex::compiles("nobody.tx + 1", &why));
    CHECK_THROWS(ex::parse("ctrl.tx +"));
    CHECK_THROWS(ex::parse("(1"));
    CHECK(ex::parse("pi")->number == doctest::Approx(3.14159265));
}

TEST_CASE("expression compiles to rig math nodes that evaluate, query and delete")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    eCmd("spaceLocator", {{"name", "foot"}});
    eCmd("spaceLocator", {{"name", "box"}});
    eCmd("addAttr", {{"args", {"foot"}}, {"longName", "roll"}, {"keyable", true}});
    json r = eCmd("expression", {{"args", {"box.ty"}}, {"string", "= ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)"}});
    CHECK(r["native"] == true);
    CHECK(r["nodes"].size() == 3);
    eCmd("setAttr", {{"args", {"ctrl.ry", 20.0}}});
    eCmd("setAttr", {{"args", {"foot.roll", 120.0}}});
    CHECK(value("box.ty") == doctest::Approx(10.0 + 90.0));
    eCmd("setAttr", {{"args", {"foot.roll", -5.0}}});
    CHECK(value("box.ty") == doctest::Approx(10.0));

    json q = eCmd("expression", {{"args", {"box.ty"}}, {"query", true}});
    CHECK(q["text"] == "ctrl.ry * 0.5 + clamp(foot.roll, 0, 90)");
    CHECK(q["native"] == true);

    // Conditionals, comparisons and logic.
    eCmd("expression", {{"args", {"box.tz"}}, {"string", "ctrl.tx * 2 if ctrl.tx > 1 and not ctrl.tz > 0 else -1"}});
    eCmd("setAttr", {{"args", {"ctrl.tx", 3.0}}});
    CHECK(value("box.tz") == doctest::Approx(6.0));
    eCmd("setAttr", {{"args", {"ctrl.tz", 1.0}}});
    CHECK(value("box.tz") == doctest::Approx(-1.0));
    eCmd("expression", {{"args", {"box.sx"}}, {"string", "2 ** 3 + 7 % 4"}});
    CHECK(value("box.sx") == doctest::Approx(11.0));

    // Setting again replaces the old nodes; save and load keep it; undo and delete clean up.
    const size_t before = Graph::current().nodes().size();
    eCmd("expression", {{"args", {"box.ty"}}, {"string", "ctrl.ry"}});
    CHECK(Graph::current().nodes().size() == before - 2); // three nodes out, one in
    CHECK(value("box.ty") == doctest::Approx(20.0));
    eCmd("expression", {{"args", {"box.ty"}}, {"delete", true}});
    CHECK(eCmd("expression", {{"args", {"box.ty"}}, {"query", true}}).is_null());
    CHECK_FALSE(Graph::current().source(Graph::current().plug("box.ty")));
    eCmd("undo");
    CHECK(eCmd("expression", {{"args", {"box.ty"}}, {"query", true}})["text"] == "ctrl.ry");
    json doc = io::serialize();
    eCmd("newScene");
    io::deserialize(doc);
    CHECK(value("box.ty") == doctest::Approx(20.0));
    CHECK(eCmd("expression", {{"args", {"box.ty"}}, {"query", true}})["text"] == "ctrl.ry");
    CHECK_THROWS(eCmd("expression", {{"args", {"box.translate"}}, {"string", "1"}}));
}

TEST_CASE("Expressions beyond the subset run through the Python evaluator with plugs as v0, v1")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    eCmd("spaceLocator", {{"name", "box"}});
    std::string seen;
    double seenTime = -1.0;
    ex::setPythonEvaluator([&](const std::string& code, const std::vector<double>& v, double time, double, std::string*) -> std::optional<double> {
        seen = code;
        seenTime = time;
        return v.empty() ? 0.0 : v[0] * 10.0;
    });
    eCmd("setAttr", {{"args", {"ctrl.tx", 2.0}}});
    json r = eCmd("expression", {{"args", {"box.ty"}}, {"string", "fabs(ctrl.tx)"}});
    CHECK(r["native"] == false);
    CHECK(r["reason"].get<std::string>().find("fabs") != std::string::npos);
    CHECK(value("box.ty") == doctest::Approx(20.0));
    CHECK(seen == "fabs(v0)");
    json forced = eCmd("expression", {{"args", {"box.tz"}}, {"string", "ctrl.tx + 1"}, {"python", true}});
    CHECK(forced["native"] == false);

    // time and frame reach the Python too, and follow the scene time.
    eCmd("expression", {{"args", {"box.sx"}}, {"string", "fabs(ctrl.tx) * time"}});
    Graph::current().setTime(1.5);
    value("box.sx");
    CHECK(seenTime == doctest::Approx(1.5));
    Graph::current().setTime(0.0);
    ex::setPythonEvaluator({});
}

TEST_CASE("time, frame and the Maya-style functions compile to nodes")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "ctrl"}});
    eCmd("spaceLocator", {{"name", "box"}});
    Graph& g = Graph::current();
    const double fps = anim::playback().fps;
    CHECK(eCmd("expression", {{"args", {"box.tx"}}, {"string", "sin(time) * 2 + frame"}})["native"] == true);
    g.setTime(0.75);
    CHECK(value("box.tx") == doctest::Approx(std::sin(0.75) * 2.0 + 0.75 * fps));
    g.setTime(2.0);
    CHECK(value("box.tx") == doctest::Approx(std::sin(2.0) * 2.0 + 2.0 * fps));
    g.setTime(0.0);

    eCmd("setAttr", {{"args", {"ctrl.tx", -2.5}}});
    auto at = [&](const char* text) {
        eCmd("expression", {{"args", {"box.ty"}}, {"string", text}});
        return value("box.ty");
    };
    CHECK(at("floor(ctrl.tx)") == -3.0);
    CHECK(at("ceil(ctrl.tx)") == -2.0);
    CHECK(at("round(ctrl.tx)") == -3.0);
    CHECK(at("trunc(ctrl.tx)") == -2.0);
    CHECK(at("linstep(-5, 5, ctrl.tx)") == doctest::Approx(0.25));
    CHECK(at("smoothstep(-5, 5, ctrl.tx)") == doctest::Approx(0.25 * 0.25 * (3.0 - 0.5)));
    CHECK(at("smoothstep(0, 1, 7)") == doctest::Approx(1.0));
    const double n1 = at("noise(ctrl.tx)"), n2 = at("noise(ctrl.tx, 4)");
    CHECK(n1 >= -1.0);
    CHECK(n1 <= 1.0);
    CHECK(n1 != n2);
    CHECK(std::abs(at("noise(ctrl.tx + 0.001)") - n1) < 0.01);
    const double r1 = at("random(7)");
    CHECK(r1 >= 0.0);
    CHECK(r1 < 1.0);
    CHECK(at("random(7)") == r1);
    CHECK(at("random(8)") != r1);
    CHECK(at("random(7, 5, 10)") == doctest::Approx(5.0 + 5.0 * r1));
}

TEST_CASE("Programs evaluate exactly as the expression's nodes")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "a"}});
    eCmd("spaceLocator", {{"name", "b"}});
    eCmd("spaceLocator", {{"name", "box"}});
    Graph& g = Graph::current();
    const char* texts[] = {
        "a.tx / b.tx + a.ty % b.ty - b.tz ** 0.5",
        "sqrt(a.tx) + log(a.ty) + log(a.tz, b.tx) + exp(b.ty * 0.1)",
        "pow(a.tx, b.tx) + hypot(a.ty, b.ty) + atan2(a.tz, b.tz)",
        "sin(a.tx) * cos(b.tx) + tan(a.ty * 0.1) + asin(b.ty) + acos(a.tz) + atan(b.tz)",
        "abs(a.tx) + sign(b.tx) + min(a.ty, b.ty) + max(a.tz, b.tz) + clamp(a.tx, b.ty, b.tz)",
        "lerp(a.tx, b.tx, a.ty) + wrap(a.tz, b.tx, b.ty) + mapRange(a.tx, b.tx, b.ty, 0, 10)",
        "degrees(a.tx) + radians(b.tx) + fmod(a.ty, b.ty) + floor(a.tz) + ceil(b.tz) + round(a.tx) + trunc(b.tx)",
        "a.tx * 2 if a.ty > b.ty and not a.tz == b.tz or b.tx <= 0 else -b.tx",
        "1 if a.tx >= b.tx else (2 if a.tx != b.tx else 3) + (a.ty < 0)",
        "noise(a.tx * 3, b.tx) + noise(a.ty) + random(b.ty * 100) + random(b.tz * 100, -5, 5)",
        "linstep(b.tx, b.ty, a.tx) + smoothstep(-1, 1, a.ty) + sin(time * 3) + frame * 0.1",
        "a.tx",
        "4.25",
    };
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        const double u = double(seed >> 8) / double(1u << 24);
        // Edge values a good share of the time: zero, negatives, whole numbers.
        if (u < 0.15)
            return 0.0;
        if (u < 0.25)
            return double(int(u * 40.0) - 5);
        return (u - 0.4) * 12.0;
    };
    for (const char* text : texts)
    {
        INFO(text);
        REQUIRE(eCmd("expression", {{"args", {"box.ty"}}, {"string", text}})["native"] == true);
        const ex::ProgramText p = ex::program(g, g.plug("box.ty"));
        REQUIRE_MESSAGE(p.why.empty(), p.why);
        lila_expr::Program prog;
        std::string why;
        REQUIRE_MESSAGE(lila_expr::parse(p.text, prog, &why), why);
        for (int trial = 0; trial < 40; ++trial)
        {
            for (const char* plug : {"a.tx", "a.ty", "a.tz", "b.tx", "b.ty", "b.tz"})
                eCmd("setAttr", {{"args", {plug, rnd()}}});
            const double t = std::abs(rnd());
            g.setTime(t);
            const double expected = value("box.ty");
            std::vector<double> inputs;
            for (const PlugRef& in : p.inputs)
                inputs.push_back(std::get<double>(g.value(in)));
            CHECK(lila_expr::evaluate(prog, inputs.data(), t) == expected);
        }
    }
    g.setTime(0.0);
}

TEST_CASE("Python expressions and missing ones have no program")
{
    eCmd("newScene");
    eCmd("spaceLocator", {{"name", "box"}});
    Graph& g = Graph::current();
    CHECK(ex::program(g, g.plug("box.ty")).why == "no expression");
    eCmd("expression", {{"args", {"box.ty"}}, {"string", "1 + 2"}, {"python", true}});
    CHECK(ex::program(g, g.plug("box.ty")).why == "runs as Python");
    eCmd("expression", {{"args", {"box.tz"}}, {"string", "1 + 2"}});
    CHECK(ex::targets(g).size() == 2);
}

TEST_CASE("LilaLink's expression evaluator is a byte-identical copy")
{
    auto read = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    const std::string lila = read(LILA_SOURCE_DIR "/src/rigmath/ExpressionVm.h");
    const std::string ue = read(LILA_SOURCE_DIR "/ue/LilaLink/Source/LilaLinkRig/Private/LilaExpressionVm.h");
    REQUIRE_FALSE(lila.empty());
    CHECK(ue == lila);
}

TEST_CASE("Bone channels read and write as Lila composes a joint")
{
    namespace ch = lila_expr::channels;
    eCmd("newScene");
    Graph& g = Graph::current();
    Node* j = g.createNode(*types::find("joint"), "j");
    uint32_t seed = 99;
    auto rnd = [&](double lo, double hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * double(seed >> 8) / double(1u << 24);
    };
    auto toM3 = [](const glm::dmat4& m) {
        ch::M3 r;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k)
                r.m[i][k] = m[k][i];
        return r;
    };
    // Unreal axes from Lila's: a signed axis permutation, as the FBX profiles are.
    ch::Basis basis;
    basis.k = ch::M3{{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}};
    basis.unitScale = 1.0;
    for (int order = 0; order < 6; ++order)
        for (int trial = 0; trial < 25; ++trial)
        {
            const glm::dvec3 t(rnd(-50, 50), rnd(-50, 50), rnd(-50, 50)), r(rnd(-170, 170), rnd(-85, 85), rnd(-170, 170)),
                s(rnd(0.5, 2), rnd(0.5, 2), rnd(0.5, 2)), jo(rnd(-90, 90), rnd(-90, 90), rnd(-90, 90)), ra(rnd(-30, 30), rnd(-30, 30), rnd(-30, 30));
            g.setValue({j->id(), Transform::kTranslate}, t);
            g.setValue({j->id(), Transform::kRotate}, r);
            g.setValue({j->id(), Transform::kScale}, s);
            g.setValue({j->id(), Transform::kRotateOrder}, int64_t(order));
            g.setValue({j->id(), Transform::kRotateAxis}, ra);
            g.setValue({j->id(), Joint::kJointOrient}, jo);
            g.evaluate();
            const glm::dmat4 local = g.node(j->id())->as<glm::dmat4>(Transform::kMatrix);

            const double rd[3] = {r.x, r.y, r.z};
            const ch::M3 mine = ch::eulerToMatrix(rd, order), lilas = toM3(eulerToMatrix(r, RotateOrder(order)));
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    CHECK(mine.m[a][b] == doctest::Approx(lilas.m[a][b]).epsilon(1e-12));

            ch::Joint joint;
            joint.order = order;
            joint.orient = toM3(static_cast<Transform*>(g.node(j->id()))->preRotation());
            joint.axis = toM3(static_cast<Transform*>(g.node(j->id()))->rotateAxisMatrix());
            const double channelsIn[9] = {t.x, t.y, t.z, r.x, r.y, r.z, s.x, s.y, s.z};
            for (int k = 0; k < 9; ++k)
                joint.rest[k] = channelsIn[k] + (k >= 3 && k < 6 ? rnd(-20, 20) : 0.0);
            const ch::BoneTransform bone = ch::write(channelsIn, joint, basis);

            // The bone is K * local * K^T.
            const ch::M3 rq = ch::quatToMatrix(bone.rotation);
            const ch::M3 l3 = toM3(local);
            const ch::M3 expected = ch::mul(basis.k, ch::mul(l3, ch::transpose(basis.k)));
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    CHECK(rq.m[a][b] * bone.scale[b] == doctest::Approx(expected.m[a][b]).epsilon(1e-9));
            double tl[3] = {local[3][0], local[3][1], local[3][2]}, tu[3];
            ch::apply(basis.k, tl, tu);
            for (int a = 0; a < 3; ++a)
                CHECK(bone.translation[a] == doctest::Approx(tu[a]).epsilon(1e-9));

            double back[9];
            ch::read(bone, joint, basis, back);
            for (int k = 0; k < 9; ++k)
                CHECK(back[k] == doctest::Approx(channelsIn[k]).epsilon(1e-7));
        }
}
