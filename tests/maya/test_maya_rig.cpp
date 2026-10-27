#include <doctest/doctest.h>

#include "fbx/Fbx.h"
#include "fbx/FbxAxes.h"
#include "geo/Mesh.h"
#include "io/References.h"
#include "maya/Maya.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MathNodes.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
fs::path rigFixture(const std::string& name) { return fs::path(LILA_MAYA_TEST_DATA) / "rig" / name; }

json expected(const std::string& name)
{
    std::ifstream in(rigFixture(name + ".json"));
    REQUIRE(in.good());
    return json::parse(in);
}

maya::ImportResult importRig(const std::string& name, const std::string& upAxis = "auto")
{
    io::refs::clear();
    Graph& g = Graph::current();
    g.clear();
    maya::ImportResult r = maya::importMaya(g, rigFixture(name + ".ma"), {.upAxis = upAxis});
    if (!r.ok)
        MESSAGE(r.error);
    REQUIRE(r.ok);
    return r;
}

/** The Maya to Lila basis a file read as Z up gets (identity for Y up), as the importer builds it. */
struct Axes
{
    glm::dmat3 c3{1.0};
    fbx::AxisMap map;

    static Axes zUp()
    {
        Axes a;
        a.c3 = glm::transpose(fbx::profileBasis(fbx::AxisProfile::Ue5, math::upAxis()));
        a.map = fbx::axisMap(a.c3);
        return a;
    }
};

/** A Maya translate, rotate or scale channel as the Lila plug it lands on and the factor its value takes. */
std::pair<std::string, double> lilaChannel(const std::string& plug, const Axes& ax)
{
    static const char* kGroups[3] = {"translate", "rotate", "scale"};
    const size_t dot = plug.find('.');
    const std::string node = plug.substr(0, dot), attr = plug.substr(dot + 1);
    for (int g = 0; g < 3; ++g)
        for (int a = 0; a < 3; ++a)
            if (attr == std::string(kGroups[g]) + char('X' + a))
                return {node + "." + kGroups[g] + char('X' + ax.map.to[a]), ax.map.factor(g, a)};
    return {plug, 1.0};
}

void setDriver(const std::string& mayaPlug, double value, const Axes& ax = {})
{
    Graph& g = Graph::current();
    auto [plug, factor] = lilaChannel(mayaPlug, ax);
    value *= factor;
    PlugRef p = g.plug(plug);
    const AttrDef& def = g.attr(p);
    if (p.component >= 0 || def.type == PlugType::Float)
        g.setValue(p, value);
    else if (def.type == PlugType::Bool)
        g.setValue(p, value != 0.0);
    else
        g.setValue(p, int64_t(std::llround(value)));
}

glm::dmat4 mayaMatrix(const json& v)
{
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = v[size_t(c * 4 + r)].get<double>();
    return m;
}

double matrixError(const glm::dmat4& a, const glm::dmat4& b)
{
    double e = 0.0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            e = std::max(e, std::abs(a[c][r] - b[c][r]));
    return e;
}

std::string str(const glm::dmat4& m)
{
    std::string s;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            s += std::format("{:.4f} ", m[c][r]);
    return s;
}

struct Tolerance
{
    double matrix = 1e-4;
    double points = 1e-4;
    /** Per checked node or mesh, where Lila's math differs from Maya's on purpose. */
    std::map<std::string, double> loose;
};

/** Poses the drivers as each recorded pose sets them and compares world matrices and mesh points with Maya's. */
void comparePoses(const std::string& name, const Tolerance& tol, const std::set<std::string>& skipNodes = {}, const Axes& ax = {})
{
    const glm::dmat4 c4(ax.c3);
    const json want = expected(name);
    Graph& g = Graph::current();
    int index = 0;
    for (const json& pose : want["poses"])
    {
        for (const auto& [plug, v] : want["drivers"].items())
            setDriver(plug, v.get<double>(), ax);
        for (const auto& [plug, v] : pose["set"].items())
            setDriver(plug, v.get<double>(), ax);
        g.evaluate();
        for (const auto& [node, m] : pose["matrices"].items())
        {
            if (skipNodes.contains(node))
                continue;
            Node* n = g.find(node);
            INFO(name << " pose " << index << " " << std::string(node));
            REQUIRE(n);
            const glm::dmat4 got = std::get<glm::dmat4>(g.value({n->id(), Transform::kWorldMatrix, -1}));
            const glm::dmat4 exp = c4 * mayaMatrix(m) * glm::transpose(c4);
            const double t = tol.loose.contains(node) ? tol.loose.at(node) : tol.matrix;
            INFO("got  " << str(got));
            INFO("want " << str(exp));
            CHECK(matrixError(got, exp) < t);
        }
        for (const auto& [mesh, pts] : pose["points"].items())
        {
            if (skipNodes.contains(mesh))
                continue;
            Node* n = g.find(mesh);
            INFO(name << " pose " << index << " " << mesh);
            REQUIRE(n);
            MeshPtr m = std::get<MeshPtr>(g.value({n->id(), MeshNode::kOutMesh, -1}));
            REQUIRE(m);
            const glm::dmat4 w = std::get<glm::dmat4>(g.value({n->id(), Transform::kWorldMatrix, -1}));
            REQUIRE(m->pointCapacity() == pts.size());
            const double t = tol.loose.contains(mesh) ? tol.loose.at(mesh) : tol.points;
            double worst = 0.0;
            size_t worstAt = 0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const glm::dvec3 got(w * glm::dvec4(m->position(geo::Index(i)), 1.0));
                const glm::dvec3 exp = ax.c3 * glm::dvec3(pts[i][0].get<double>(), pts[i][1].get<double>(), pts[i][2].get<double>());
                if (double d = glm::length(got - exp); d > worst)
                    worst = d, worstAt = i;
            }
            INFO("worst point " << worstAt << " off by " << worst);
            CHECK(worst < t);
        }
        ++index;
    }
}

bool mentions(const std::vector<std::string>& lines, std::string_view what)
{
    for (const std::string& s : lines)
        if (s.find(what) != std::string::npos)
            return true;
    return false;
}

void dump(const maya::ImportResult& r)
{
    for (const std::string& s : r.skipped)
        MESSAGE("skipped: " << s);
    for (const std::string& s : r.warnings)
        MESSAGE("warning: " << s);
}
} // namespace

TEST_CASE("maya rig: utility nodes evaluate as Maya's")
{
    maya::ImportResult r = importRig("rig_utility");
    dump(r);
    CHECK(r.skipped.empty());
    comparePoses("rig_utility", {});
}

TEST_CASE("maya rig: matrix nodes and offset parent matrices evaluate as Maya's")
{
    maya::ImportResult r = importRig("rig_matrix");
    dump(r);
    CHECK(r.skipped.empty());
    comparePoses("rig_matrix", {});
}

TEST_CASE("maya rig: constraints keep their offsets, weights and targets")
{
    maya::ImportResult r = importRig("rig_constraints");
    dump(r);
    CHECK(r.skipped.empty());
    comparePoses("rig_constraints", {});
}

TEST_CASE("maya rig: IK handles with pole vectors and twist")
{
    maya::ImportResult r = importRig("rig_ik");
    dump(r);
    CHECK(r.skipped.empty());
    // The rotate plane solve matches Maya's; the single chain solve (shoulder to wrist) is Lila's own, close to it.
    // Rotate plane chains match Maya's solve. A single chain limb (shoulder to wrist) bends in its rest plane carried by
    // the handle, which comes close to Maya's own single chain solve.
    comparePoses("rig_ik", {.matrix = 1e-3, .loose = {{"shoulder", 0.25}, {"elbow", 0.25}, {"wrist", 0.25}}});
}

TEST_CASE("maya rig: single and stacked set driven keys")
{
    maya::ImportResult r = importRig("rig_sdk");
    dump(r);
    CHECK(r.skipped.empty());
    comparePoses("rig_sdk", {});
}

TEST_CASE("maya rig: cluster, lattice, nonlinear and softMod deformers")
{
    maya::ImportResult r = importRig("rig_deformers");
    dump(r);
    CHECK(r.skipped.empty());
    // Partial cluster weights blend the moved points linearly (Maya interpolates the turn); squash and wave bulge and
    // ripple by Lila's profiles.
    comparePoses("rig_deformers", {.loose = {{"clusterPlane", 0.05}, {"squashCube", 0.3}, {"wavePlane", 0.1}}});
}

TEST_CASE("maya rig: delta mush and wrap")
{
    maya::ImportResult r = importRig("rig_surface_deformers");
    dump(r);
    // Lila's delta mush and wrap binding are their own, so the shapes come close to Maya's rather than exactly.
    comparePoses("rig_surface_deformers", {.loose = {{"tube", 0.1}, {"ball", 0.05}}});
}

TEST_CASE("maya rig: follicles and uvPins")
{
    maya::ImportResult r = importRig("rig_pins");
    dump(r);
    CHECK(r.skipped.empty());
    // The proximityPin's frame turns with Lila's triangle frame, a little off Maya's smoothed one while the sheet bends.
    comparePoses("rig_pins", {.loose = {{"pinnedC", 0.06}}});
}

TEST_CASE("maya rig: a leg with IK, a reverse foot, constraints, a cluster, a lattice and stacked set driven keys")
{
    maya::ImportResult r = importRig("rig_combined");
    dump(r);
    CHECK(r.skipped.empty());
    comparePoses("rig_combined", {});
}

TEST_CASE("maya rig: networks, constraints and the leg rig hold across an axis change")
{
    // Read as Z up, every vector, matrix and channel goes through the axis change, and the rigs still pose like Maya's.
    const Axes ax = Axes::zUp();
    for (std::string name : {"rig_utility", "rig_matrix", "rig_constraints", "rig_sdk", "rig_combined"})
    {
        INFO(name);
        maya::ImportResult r = importRig(name, "z");
        dump(r);
        CHECK(r.skipped.empty());
        comparePoses(name, {}, {}, ax);
    }
}

TEST_CASE("maya rig: what stays unconverted is reported")
{
    maya::ImportResult r = importRig("rig_unsupported");
    dump(r);
    CHECK(mentions(r.skipped, "expression 'wobble'"));
    CHECK(mentions(r.skipped, "ikSplineSolver"));
    CHECK(mentions(r.skipped, "motionPath"));
    CHECK(mentions(r.skipped, "curveInfo"));
    CHECK(mentions(r.skipped, "wire 'sheetWire'"));
    CHECK(mentions(r.skipped, "ctl.label"));
    // A texture deformer with a file texture converts; Lila's sampling is its own, so only its settings are checked.
    Graph& g = Graph::current();
    Node* td = g.find("bumpDeform");
    REQUIRE(td);
    CHECK(td->type().name == "textureDeformer");
    CHECK(td->as<std::string>(td->type().attrIndex("image")).ends_with("front.png"));
    CHECK(td->as<double>(td->type().attrIndex("strength")) == doctest::Approx(0.5));
    CHECK(td->as<double>(td->type().attrIndex("midLevel")) == doctest::Approx(0.5));
    // The addDL nodes the motion path made come across as Lila math nodes, though what drives them doesn't.
    CHECK_FALSE(mentions(r.skipped, "addDL node"));
}

TEST_CASE("maya rig: decompose and compose round trip with shear")
{
    mathnodes::Decomposed d;
    d.translate = {1, 2, 3};
    d.rotation = glm::dmat3(eulerToMatrix({10, 20, 30}, RotateOrder::ZXY));
    d.scale = {1.5, 0.5, 2};
    d.shear = {0.2, -0.1, 0.3};
    const mathnodes::Decomposed back = mathnodes::decompose(mathnodes::compose(d));
    CHECK(glm::length(back.translate - d.translate) < 1e-12);
    CHECK(glm::length(back.scale - d.scale) < 1e-12);
    CHECK(glm::length(back.shear - d.shear) < 1e-12);
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(back.rotation[c] - d.rotation[c]) < 1e-12);
}

