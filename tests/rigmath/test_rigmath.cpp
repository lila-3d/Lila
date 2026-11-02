#include <doctest/doctest.h>

#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <map>
#include <set>

using namespace lila;
using scene::PlugValue;
using D3 = glm::dvec3;
using M4 = glm::dmat4;
using json = nlohmann::json;

namespace
{
constexpr double kPi = 3.14159265358979323846;

scene::Graph& g() { return scene::Graph::current(); }

/** A fresh node of the type; its path. */
std::string make(const char* type)
{
    const scene::NodeType* t = scene::types::find(type);
    REQUIRE_MESSAGE(t, type);
    return g().createNode(*t)->name();
}

void set(const std::string& node, const char* attr, PlugValue v) { g().setValue(g().plug(node + "." + attr), std::move(v)); }
void connect(const std::string& src, const std::string& dst) { g().connect(g().plug(src), g().plug(dst)); }
PlugValue get(const std::string& node, const char* attr) { return g().value(g().plug(node + "." + attr)); }
double f(const std::string& n, const char* a = "result") { return std::get<double>(get(n, a)); }
int64_t i(const std::string& n, const char* a = "result") { return std::get<int64_t>(get(n, a)); }
bool b(const std::string& n, const char* a = "result") { return std::get<bool>(get(n, a)); }
D3 v(const std::string& n, const char* a = "result") { return std::get<D3>(get(n, a)); }
M4 m(const std::string& n, const char* a = "result") { return std::get<M4>(get(n, a)); }

void near(const D3& a, const D3& e, double eps = 1e-6)
{
    CHECK(a.x == doctest::Approx(e.x).epsilon(eps).scale(1.0));
    CHECK(a.y == doctest::Approx(e.y).epsilon(eps).scale(1.0));
    CHECK(a.z == doctest::Approx(e.z).epsilon(eps).scale(1.0));
}

void near(const M4& a, const M4& e, double eps = 1e-6)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(a[c][r] == doctest::Approx(e[c][r]).epsilon(eps).scale(1.0));
}

/** Calls a one-output node with the given inputs. */
std::string eval(const char* type, std::initializer_list<std::pair<const char*, PlugValue>> ins)
{
    std::string n = make(type);
    for (const auto& [a, val] : ins)
        set(n, a, val);
    return n;
}

M4 rotY(double deg) { return glm::rotate(M4(1.0), deg * kPi / 180.0, D3(0, 1, 0)); }
M4 rotX(double deg) { return glm::rotate(M4(1.0), deg * kPi / 180.0, D3(1, 0, 0)); }
} // namespace

TEST_CASE("one table makes every node type, each named once")
{
    std::set<std::string> types;
    std::set<std::string> families;
    std::map<std::string, size_t> perFamily;
    size_t overloads = 0;
    for (const rigmath::Function& fn : rigmath::functions())
    {
        CHECK_MESSAGE(!fn.doc.empty(), fn.name);
        families.insert(fn.family);
        for (const rigmath::Overload& o : fn.overloads)
        {
            ++overloads;
            ++perFamily[fn.family];
            CHECK(types.insert(o.nodeType).second);
            const scene::NodeType* t = scene::types::find(o.nodeType);
            REQUIRE_MESSAGE(t, o.nodeType);
            CHECK(t->doc == fn.doc);
            CHECK(!t->icon.empty());
            CHECK(rigmath::overloadFor(o.nodeType) == &o);
            CHECK(rigmath::functionFor(o.nodeType) == &fn);
        }
    }
    MESSAGE("functions " << rigmath::functions().size() << ", node types " << overloads << ", by family " << json(perFamily).dump());
    CHECK(overloads >= 200);
    CHECK(families == std::set<std::string>{"arithmetic", "comparison", "range", "interpolation", "easing", "trig", "exponential", "vector", "rotator",
                                            "quat", "matrix", "color", "select", "bitwise", "noise"});
    // A single overload keeps the function's name; several add the type.
    for (const char* name : {"mapRangeClamped", "lerp", "vlerp", "rlerp", "tlerp", "quatSlerp", "addFloat", "addInt", "addVector", "addColor",
                             "multiplyVectorFloat", "multiplyMatrix", "multiplyQuat", "clampVector", "selectRotator", "switchOnIntMatrix", "andInt",
                             "notBool", "makeRotFromXZ", "findLookAtRotation", "quatToSwingTwist", "aimMath", "easing", "ease"})
        CHECK_MESSAGE(scene::types::find(name), name);
    // Rotators carry a rotate order; nothing else does.
    CHECK(scene::types::find("composeRotators")->attrIndex("rotateOrder") >= 0);
    CHECK(scene::types::find("addFloat")->attrIndex("rotateOrder") < 0);
}

TEST_CASE("arithmetic and comparison")
{
    g().clear();
    CHECK(f(eval("addFloat", {{"a", 2.0}, {"b", 3.5}})) == doctest::Approx(5.5));
    CHECK(i(eval("addInt", {{"a", int64_t(2)}, {"b", int64_t(3)}})) == 5);
    CHECK(f(eval("divideFloat", {{"a", 2.0}, {"b", 0.0}})) == 0.0);
    CHECK(i(eval("divideInt", {{"a", int64_t(7)}, {"b", int64_t(2)}})) == 3);
    near(v(eval("multiplyVectorFloat", {{"a", D3(1, 2, 3)}, {"b", 2.0}})), D3(2, 4, 6));
    CHECK(i(eval("percentInt", {{"a", int64_t(7)}, {"b", int64_t(3)}})) == 1);
    const std::string fm = eval("fmod", {{"dividend", 7.5}, {"divisor", 2.0}});
    CHECK(i(fm, "quotient") == 3);
    CHECK(f(fm, "remainder") == doctest::Approx(1.5));
    CHECK(b(eval("equalVector", {{"a", D3(1, 2, 3)}, {"b", D3(1, 2, 3.00001)}})));
    CHECK_FALSE(b(eval("equalVector", {{"a", D3(1, 2, 3)}, {"b", D3(1, 2, 3.1)}})));
    CHECK(b(eval("equalRotator", {{"a", D3(0, 360, 0)}, {"b", D3(0, 0, 0)}})));
    CHECK(b(eval("greaterInt", {{"a", int64_t(3)}, {"b", int64_t(2)}})));
    CHECK_FALSE(b(eval("lessEqualFloat", {{"a", 3.0}, {"b", 2.0}})));
    CHECK(b(eval("inRangeFloat", {{"value", 1.0}, {"min", 0.0}, {"max", 1.0}})));
    CHECK_FALSE(b(eval("inRangeFloat", {{"value", 1.0}, {"min", 0.0}, {"max", 1.0}, {"inclusiveMax", false}})));
}

TEST_CASE("range functions")
{
    g().clear();
    CHECK(f(eval("clampFloat", {{"value", 3.0}, {"min", 0.0}, {"max", 2.0}})) == 2.0);
    near(v(eval("clampVector", {{"value", D3(-1, 0.5, 9)}, {"min", D3(0.0)}, {"max", D3(1.0)}})), D3(0, 0.5, 1));
    CHECK(f(eval("mapRangeClamped", {{"value", 0.25}, {"outRangeA", 10.0}, {"outRangeB", 20.0}})) == doctest::Approx(12.5));
    CHECK(f(eval("mapRangeClamped", {{"value", 1.5}, {"outRangeA", 10.0}, {"outRangeB", 20.0}})) == doctest::Approx(20.0));
    CHECK(f(eval("mapRangeUnclamped", {{"value", 1.5}, {"outRangeA", 10.0}, {"outRangeB", 20.0}})) == doctest::Approx(25.0));
    CHECK(i(eval("wrapInt", {{"value", int64_t(5)}, {"min", int64_t(0)}, {"max", int64_t(3)}})) == 1);
    CHECK(i(eval("wrapInt", {{"value", int64_t(-1)}, {"min", int64_t(0)}, {"max", int64_t(3)}})) == 3);
    CHECK(f(eval("wrapFloat", {{"value", 2.5}, {"min", 0.0}, {"max", 1.0}})) == doctest::Approx(0.5));
    const std::string fl = eval("floorFloat", {{"a", -1.5}});
    CHECK(f(fl) == -2.0);
    CHECK(i(fl, "integer") == -2);
    CHECK(f(eval("roundFloat", {{"a", 2.5}})) == 3.0);
    CHECK(f(eval("normalizeAxis", {{"angle", 270.0}})) == doctest::Approx(-90.0));
    CHECK(f(eval("signFloat", {{"a", -4.0}})) == -1.0);

    const std::string mx = make("maxOfFloatArray");
    g().setElements(g().plug(mx + ".floatArray"), {1.0, 7.0, 3.0});
    CHECK(i(mx, "indexOfMaxValue") == 1);
    CHECK(f(mx, "maxValue") == 7.0);
}

TEST_CASE("trig and exponential")
{
    g().clear();
    CHECK(f(eval("degSin", {{"a", 30.0}})) == doctest::Approx(0.5));
    CHECK(f(eval("atan2", {{"y", 1.0}, {"x", 1.0}})) == doctest::Approx(kPi / 4));
    CHECK(f(eval("degAcos", {{"a", 2.0}})) == doctest::Approx(0.0));
    CHECK(f(eval("sqrt", {{"a", -1.0}})) == 0.0);
    CHECK(f(eval("log", {{"a", 8.0}, {"base", 2.0}})) == doctest::Approx(3.0));
    CHECK(f(eval("pow", {{"base", 2.0}, {"exp", 10.0}})) == doctest::Approx(1024.0));
    CHECK(f(eval("pow", {{"base", -2.0}, {"exp", 0.5}})) == 0.0);
    CHECK(f(eval("loge", {{"a", 0.0}})) == 0.0);
    CHECK(f(eval("hypotenuse", {{"width", 3.0}, {"height", 4.0}})) == doctest::Approx(5.0));
}

TEST_CASE("interpolation and easing")
{
    g().clear();
    CHECK(f(eval("lerp", {{"a", 10.0}, {"b", 20.0}, {"alpha", 0.25}})) == doctest::Approx(12.5));
    CHECK(f(eval("finterpTo", {{"current", 0.0}, {"target", 10.0}, {"deltaTime", 0.1}, {"interpSpeed", 1.0}})) == doctest::Approx(1.0));
    CHECK(f(eval("finterpToConstant", {{"current", 0.0}, {"target", 10.0}, {"deltaTime", 0.5}, {"interpSpeed", 4.0}})) == doctest::Approx(2.0));
    // easeIn is index 5 of Kismet's EEasingFunc.
    CHECK(f(eval("ease", {{"alpha", 0.5}, {"easingFunc", int64_t(5)}})) == doctest::Approx(0.25));
    CHECK(f(eval("ease", {{"alpha", 0.6}, {"easingFunc", int64_t(1)}, {"steps", int64_t(3)}})) == doctest::Approx(0.5));
    CHECK(f(eval("easing", {{"value", 1.0}, {"type", int64_t(1)}, {"sourceMaximum", 2.0}, {"targetMaximum", 10.0}})) == doctest::Approx(2.5));
    for (int64_t type = 0; type <= 30; ++type)
    {
        CAPTURE(type);
        CHECK(f(eval("easing", {{"value", 0.0}, {"type", type}})) == doctest::Approx(0.0).epsilon(1e-3).scale(1.0));
        CHECK(f(eval("easing", {{"value", 1.0}, {"type", type}})) == doctest::Approx(1.0).epsilon(1e-3).scale(1.0));
    }
    near(v(eval("vlerp", {{"a", D3(0.0)}, {"b", D3(2, 4, 6)}, {"alpha", 0.5}})), D3(1, 2, 3));
    near(v(eval("rlerp", {{"a", D3(0, 0, 0)}, {"b", D3(0, 90, 0)}, {"alpha", 0.5}})), D3(0, 45, 0));
    near(v(eval("rinterpTo", {{"current", D3(0, 170, 0)}, {"target", D3(0, -170, 0)}, {"deltaTime", 0.5}, {"interpSpeed", 1.0}})), D3(0, 180, 0));
}

TEST_CASE("vector functions")
{
    g().clear();
    near(v(eval("cross", {{"a", D3(1, 0, 0)}, {"b", D3(0, 1, 0)}})), D3(0, 0, 1));
    CHECK(f(eval("dot", {{"a", D3(1, 2, 3)}, {"b", D3(4, 5, 6)}})) == doctest::Approx(32.0));
    near(v(eval("normal", {{"a", D3(0, 3, 4)}})), D3(0, 0.6, 0.8));
    near(v(eval("normal", {{"a", D3(0.0)}})), D3(0.0));
    CHECK(f(eval("vSize", {{"a", D3(0, 3, 4)}})) == doctest::Approx(5.0));
    CHECK(f(eval("angleBetween", {{"a", D3(1, 0, 0)}, {"b", D3(0, 0, 2)}})) == doctest::Approx(90.0));
    CHECK(f(eval("signedAngleBetween", {{"a", D3(1, 0, 0)}, {"b", D3(0, 0, -1)}, {"axis", D3(0, 1, 0)}})) == doctest::Approx(90.0));
    near(v(eval("rotateAngleAxis", {{"inVect", D3(1, 0, 0)}, {"angleDeg", 90.0}, {"axis", D3(0, 1, 0)}})), D3(0, 0, -1));
    near(v(eval("projectPointOnToPlane", {{"point", D3(1, 5, 2)}, {"planeBase", D3(0, 1, 0)}})), D3(1, 1, 2));
    near(v(eval("getReflectionVector", {{"direction", D3(1, -1, 0)}})), D3(1, 1, 0));
    near(v(eval("findClosestPointOnSegment", {{"point", D3(5, 1, 0)}, {"segmentStart", D3(0.0)}, {"segmentEnd", D3(2, 0, 0)}})), D3(2, 0, 0));
    CHECK(f(eval("getPointDistanceToLine", {{"point", D3(5, 3, 0)}, {"lineOrigin", D3(0.0)}, {"lineDirection", D3(1, 0, 0)}})) == doctest::Approx(3.0));
    const std::string bv = eval("breakVector", {{"inVec", D3(1, 2, 3)}});
    CHECK(f(bv, "y") == 2.0);
    const std::string hsv = eval("rgbToHSV", {{"inColor", D3(1, 0, 0)}});
    CHECK(f(hsv, "s") == doctest::Approx(1.0));
    near(v(eval("hsvToRGB", {{"h", 120.0}})), D3(0, 1, 0), 1e-4);
}

TEST_CASE("rotators follow the node's rotate order")
{
    g().clear();
    // X 90 then Y 90 composed equals Euler (90, 90, 0) in xyz order.
    const std::string c = eval("composeRotators", {{"a", D3(90, 0, 0)}, {"b", D3(0, 90, 0)}});
    near(M4(scene::eulerToMatrix(v(c), scene::RotateOrder::XYZ)), rotY(90) * rotX(90));
    set(c, "rotateOrder", int64_t(scene::RotateOrder::ZYX));
    near(M4(scene::eulerToMatrix(v(c), scene::RotateOrder::ZYX)), rotY(90) * rotX(90));

    const std::string inv = eval("invertRotator", {{"a", D3(20, 30, 40)}});
    near(M4(scene::eulerToMatrix(v(inv), scene::RotateOrder::XYZ)) * scene::eulerToMatrix(D3(20, 30, 40), scene::RotateOrder::XYZ), M4(1.0));

    near(v(eval("rotateVector", {{"a", D3(1, 0, 0)}, {"b", D3(0, 90, 0)}})), D3(0, 0, -1));
    near(v(eval("unrotateVector", {{"a", D3(0, 0, -1)}, {"b", D3(0, 90, 0)}})), D3(1, 0, 0));

    const std::string xy = eval("makeRotFromXY", {{"x", D3(0, 0, 2)}, {"y", D3(0, 1, 0.3)}});
    const std::string axes = make("getAxes");
    connect(xy + ".result", axes + ".a");
    near(v(axes, "x"), D3(0, 0, 1));
    near(v(axes, "y"), D3(0, 1, 0));

    const std::string look = eval("findLookAtRotation", {{"start", D3(1, 1, 1)}, {"target", D3(1, 1, -4)}});
    const std::string lookAxes = make("getAxes");
    connect(look + ".result", lookAxes + ".a");
    near(v(lookAxes, "x"), D3(0, 0, -1));
    near(v(lookAxes, "y"), D3(0, 1, 0));
}

TEST_CASE("quats travel as rotation matrices")
{
    g().clear();
    const std::string q = eval("quatFromAxisAndAngle", {{"axis", D3(0, 1, 0)}, {"angle", kPi / 2}});
    near(m(q), rotY(90));
    const std::string rv = make("quatRotateVector");
    connect(q + ".result", rv + ".q");
    set(rv, "v", D3(1, 0, 0));
    near(v(rv), D3(0, 0, -1));
    const std::string bq = make("breakQuat");
    connect(q + ".result", bq + ".q");
    CHECK(f(bq, "y") == doctest::Approx(std::sin(kPi / 4)));
    CHECK(f(bq, "w") == doctest::Approx(std::cos(kPi / 4)));

    // Twist about X by 30 after a swing of 50 about Z: the split gives both back.
    const M4 twist = rotX(30), swing = glm::rotate(M4(1.0), 50 * kPi / 180.0, D3(0, 0, 1));
    const std::string st = eval("quatToSwingTwist", {{"q", swing * twist}});
    CHECK(f(st, "twistAngle") == doctest::Approx(30.0));
    near(m(st, "twist"), twist);
    near(m(st, "swing"), swing);

    near(m(eval("quatSlerp", {{"a", M4(1.0)}, {"b", rotY(90)}, {"alpha", 0.5}})), rotY(45));
    // Scale in a matrix doesn't leak into a quat read from it.
    near(m(eval("quatInversed", {{"q", rotY(30) * glm::scale(M4(1.0), D3(2.0))}})), rotY(-30));
    CHECK(f(eval("quatAngularDistance", {{"a", M4(1.0)}, {"b", rotY(60)}})) == doctest::Approx(kPi / 3));
    near(m(eval("rotatorToQuat", {{"inRot", D3(0, 90, 0)}})), rotY(90));
}

TEST_CASE("matrices compose in Unreal's order")
{
    g().clear();
    const M4 a = glm::translate(M4(1.0), D3(1, 0, 0)), r = rotY(90);
    // A first, then B: the translated point turns with B.
    near(D3(m(eval("composeTransforms", {{"a", a}, {"b", r}}))[3]), D3(0, 0, -1));
    near(m(eval("multiplyMatrix", {{"a", a}, {"b", r}})), r * a);
    const std::string rel = eval("makeRelativeTransform", {{"a", r * a}, {"relativeTo", r}});
    near(m(rel), a);

    const std::string mk = eval("makeTransform", {{"location", D3(1, 2, 3)}, {"rotation", D3(10, 20, 30)}, {"scale", D3(2, 2, 2)}});
    set(mk, "rotateOrder", int64_t(scene::RotateOrder::ZXY));
    const std::string br = make("breakTransform");
    set(br, "rotateOrder", int64_t(scene::RotateOrder::ZXY));
    connect(mk + ".result", br + ".inTransform");
    near(v(br, "location"), D3(1, 2, 3));
    near(v(br, "rotation"), D3(10, 20, 30));
    near(v(br, "scale"), D3(2, 2, 2));

    near(v(eval("transformLocation", {{"t", r * a}, {"location", D3(0.0)}})), D3(0, 0, -1));
    near(v(eval("inverseTransformLocation", {{"t", r * a}, {"location", D3(0, 0, -1)}})), D3(0.0));
    near(v(eval("transformDirection", {{"t", r * a}, {"direction", D3(1, 0, 0)}})), D3(0, 0, -1));
    near(m(eval("tlerp", {{"a", M4(1.0)}, {"b", glm::translate(M4(1.0), D3(0, 4, 0)) * rotY(90)}, {"alpha", 0.5}})),
         glm::translate(M4(1.0), D3(0, 2, 0)) * rotY(45));

    const std::string aim = eval("aimMath", {{"inputMatrix", glm::translate(M4(1.0), D3(1, 0, 0))}, {"primaryTarget", D3(1, 0, 5)}, {"secondaryTarget", D3(0, 1, 0)}});
    const M4 am = m(aim);
    near(D3(am[0]), D3(0, 0, 1));
    near(D3(am[1]), D3(0, 1, 0));
    near(D3(am[3]), D3(1, 0, 0));
}

TEST_CASE("select, bitwise and seeded noise")
{
    g().clear();
    near(v(eval("selectVector", {{"a", D3(1.0)}, {"b", D3(2.0)}, {"pickA", false}})), D3(2.0));
    const std::string sw = make("switchOnIntFloat");
    g().setValue(g().plug(sw + ".options[0]"), 10.0);
    g().setValue(g().plug(sw + ".options[2]"), 30.0);
    set(sw, "defaultValue", -1.0);
    set(sw, "selection", int64_t(2));
    CHECK(f(sw) == 30.0);
    set(sw, "selection", int64_t(1));
    CHECK(f(sw) == 0.0);
    set(sw, "selection", int64_t(5));
    CHECK(f(sw) == -1.0);
    CHECK(f(eval("conditionFloat", {{"firstTerm", 2.0}, {"secondTerm", 1.0}, {"operation", int64_t(2)}, {"ifTrue", 7.0}})) == 7.0);
    CHECK(i(eval("andInt", {{"a", int64_t(6)}, {"b", int64_t(3)}})) == 2);
    CHECK(i(eval("xorInt", {{"a", int64_t(6)}, {"b", int64_t(3)}})) == 5);
    CHECK(i(eval("shiftLeft", {{"a", int64_t(1)}, {"shift", int64_t(4)}})) == 16);
    CHECK(b(eval("nand", {{"a", true}, {"b", false}})));

    const double r1 = f(eval("randomFloatFromStream", {{"stream", int64_t(7)}}));
    CHECK(r1 == f(eval("randomFloatFromStream", {{"stream", int64_t(7)}})));
    CHECK(r1 != f(eval("randomFloatFromStream", {{"stream", int64_t(8)}})));
    CHECK(r1 >= 0.0);
    CHECK(r1 < 1.0);
    for (int64_t s = 0; s < 50; ++s)
    {
        const int64_t r = i(eval("randomIntegerInRangeFromStream", {{"min", int64_t(3)}, {"max", int64_t(5)}, {"stream", s}}));
        CHECK(r >= 3);
        CHECK(r <= 5);
    }
    CHECK(glm::length(v(eval("randomUnitVectorFromStream", {{"stream", int64_t(3)}}))) == doctest::Approx(1.0));
    const double n1 = f(eval("perlinNoise3D", {{"value", D3(0.3, 1.7, 2.2)}, {"seed", int64_t(1)}}));
    CHECK(n1 == f(eval("perlinNoise3D", {{"value", D3(0.3, 1.7, 2.2)}, {"seed", int64_t(1)}})));
    CHECK(n1 != f(eval("perlinNoise3D", {{"value", D3(0.3, 1.7, 2.2)}, {"seed", int64_t(2)}})));
    CHECK(std::abs(n1) <= 1.0);
}

TEST_CASE("math nodes evaluate through connections")
{
    g().clear();
    const scene::NodeType* tt = scene::types::find("transform");
    const std::string src = g().createNode(*tt, "src")->name();
    const std::string dst = g().createNode(*tt, "dst")->name();
    set(src, "translate", D3(2, 0, 0));

    // src.tx * 3 + 1 drives dst.ty.
    const std::string mul = make("multiplyFloat");
    const std::string add = make("addFloat");
    connect(src + ".translateX", mul + ".a");
    set(mul, "b", 3.0);
    connect(mul + ".result", add + ".a");
    set(add, "b", 1.0);
    connect(add + ".result", dst + ".translateY");
    g().evaluate();
    CHECK(std::get<D3>(get(dst, "translate")).y == doctest::Approx(7.0));
    set(src, "translate", D3(4, 0, 0));
    g().evaluate();
    CHECK(std::get<D3>(get(dst, "translate")).y == doctest::Approx(13.0));

    // World matrix -> breakTransform -> another transform's channels reproduces it, in a non-default rotate order.
    set(src, "rotate", D3(15, 40, -25));
    set(src, "rotateOrder", int64_t(scene::RotateOrder::YZX));
    const std::string br = make("breakTransform");
    set(br, "rotateOrder", int64_t(scene::RotateOrder::YZX));
    set(dst, "rotateOrder", int64_t(scene::RotateOrder::YZX));
    g().disconnect(g().plug(dst + ".translateY"));
    connect(src + ".worldMatrix", br + ".inTransform");
    connect(br + ".location", dst + ".translate");
    connect(br + ".rotation", dst + ".rotate");
    connect(br + ".scale", dst + ".scale");
    g().evaluate();
    near(m(dst, "worldMatrix"), m(src, "worldMatrix"));

    // Int output into a float input converts.
    const std::string fl = make("floorFloat");
    const std::string sq = make("square");
    set(fl, "a", 2.7);
    connect(fl + ".integer", sq + ".a");
    CHECK(f(sq) == doctest::Approx(4.0));
}

TEST_CASE("mathNodes lists the library")
{
    json vec = ops::execute("mathNodes", {{"family", "vector"}, {"namesOnly", true}});
    CHECK(std::find(vec.begin(), vec.end(), "cross") != vec.end());
    json add = ops::execute("mathNodes", {{"name", "addVector"}});
    REQUIRE(add.size() == 1);
    CHECK(add[0]["name"] == "add");
    CHECK(add[0]["overloads"].size() == 4);
    CHECK(add[0]["overloads"][2]["inputs"][0]["kind"] == "Vector");
    CHECK_THROWS(ops::execute("mathNodes", {{"name", "noSuchThing"}}));
}
