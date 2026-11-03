#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RigElements.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"

#include <map>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
using json = nlohmann::json;

namespace
{
constexpr double kFps = 24.0;
constexpr int kFrames = 24;

/** A point on a damped spring chasing a target: the physics a recording bakes onto a layer. */
class Spring final : public sim::Solver
{
public:
    explicit Spring(NodeId node) : mNode(node) {}
    sim::Setup configure(Graph& g) override
    {
        const Node& n = *g.node(mNode);
        mTarget = g.find(n.as<std::string>(n.type().attrIndex("target")))->id();
        sim::Setup s;
        s.layout = {{sim::Channel::Position, 3}, {sim::Channel::Velocity, 3}};
        s.outputs = {g.find(n.as<std::string>(n.type().attrIndex("follower")))->id()};
        s.sources = {mTarget};
        return s;
    }
    void sample(Graph& g, std::vector<double>& in) override
    {
        const glm::dmat4 w = g.node(mTarget)->as<glm::dmat4>(Transform::kWorldMatrix);
        in.insert(in.end(), {w[3].x, w[3].y, w[3].z});
    }
    void reset(const std::vector<double>& in, std::vector<double>& s) override
    {
        std::copy(in.begin(), in.begin() + 3, s.begin());
        std::fill(s.begin() + 3, s.end(), 0.0);
    }
    void step(const std::vector<double>& in, std::vector<double>& s, const sim::StepInfo& at) override
    {
        for (int i = 0; i < 3; ++i)
        {
            const double x = s[size_t(i)] - in[size_t(i)];
            s[size_t(3 + i)] = (s[size_t(3 + i)] - 80.0 * x * at.dt) / (1.0 + at.dt * 4.0 + at.dt * at.dt * 80.0);
            s[size_t(i)] += s[size_t(3 + i)] * at.dt;
        }
    }
    void pose(const std::vector<double>&, const std::vector<double>& s, std::vector<glm::dmat4>& world) override
    {
        glm::dmat4 m(1.0);
        m[3] = glm::dvec4(s[0], s[1], s[2], 1.0);
        world = {m};
    }

private:
    NodeId mNode;
    NodeId mTarget = kNoNode;
};

void registerSpring()
{
    if (types::find("cleanBakeSpring"))
        return;
    const NodeType* xf = types::find("transform");
    NodeType t{.name = "cleanBakeSpring", .base = xf, .dag = true, .create = xf->create};
    const uint32_t s = AttrInput | AttrStorable;
    t.ownAttrs = {
        {.name = "target", .type = PlugType::String, .defaultValue = std::string(), .flags = s},
        {.name = "follower", .type = PlugType::String, .defaultValue = std::string(), .flags = s},
        {.name = sim::kRecordingsAttr, .type = PlugType::String, .defaultValue = std::string(), .flags = s | AttrHidden},
    };
    types::registerType(std::move(t));
    sim::registerSolverType({.nodeType = "cleanBakeSpring",
                             .create = [](Graph&, NodeId n) -> std::unique_ptr<sim::Solver> { return std::make_unique<Spring>(n); }});
}

void joint(const char* name, glm::dvec3 at, const char* parent)
{
    run("joint", {{"name", name}, {"position", v3(at.x, at.y, at.z)}, {"parent", parent}});
}

/**
 * A character using every driver: a locked reverse-foot leg under a bouncing pelvis, an arm posed through its IK
 * controller, a twist joint, a looker, a joint switching spaces, a constrained joint, an additive layer, a physics
 * recording, and a rig-only joint between two deform joints.
 */
void riggedScene()
{
    registerSpring();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"fps", kFps}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
    joint("root", {0, 0, 0}, "");
    joint("pelvis", {0, 100, 0}, "root");
    joint("hip_l", {10, 95, 0}, "pelvis");
    joint("knee_l", {10, 52, 6}, "hip_l");
    joint("ankle_l", {10, 10, 0}, "knee_l");
    joint("ball_l", {10, 3, 12}, "ankle_l");
    joint("toe_l", {10, 0, 20}, "ball_l");
    joint("spine", {0, 120, 0}, "pelvis");
    joint("chest", {0, 140, 0}, "spine");
    joint("arm_l", {20, 140, 0}, "chest");
    joint("elbow_l", {45, 140, -4}, "arm_l");
    joint("hand_l", {70, 140, 0}, "elbow_l");
    joint("arm_twist_l", {32, 140, -2}, "arm_l");
    joint("head", {0, 160, 0}, "chest");
    joint("head_end", {0, 170, 0}, "head");
    joint("antenna", {0, 175, 0}, "head");
    joint("prop_r", {-30, 130, 10}, "chest");
    joint("held", {-40, 120, 10}, "chest");
    joint("ik_helper", {0, 0, 10}, "root");
    joint("ik_child", {0, 0, 20}, "ik_helper");
    run("setAttr", {{"args", {"ik_helper.deform", false}}});
    run("spaceLocator", {{"name", "wind"}});
    run("spaceLocator", {{"name", "grip"}});
    run("setAttr", {{"args", {"grip.translate", v3(-45, 115, 20)}}});

    run("setKeyframe", {{"args", {"pelvis.translate", "pelvis.rotate", "hip_l.rotate", "knee_l.rotate", "ankle_l.rotate", "ball_l.rotate",
                                  "spine.rotate", "arm_l.rotate", "elbow_l.rotate", "hand_l.rotate", "prop_r.translate", "prop_r.rotate"}},
                        {"time", 0.0}});
    run("setKeyframe", {{"args", {"pelvis.ty"}}, {"time", 0.5}, {"value", 93.0}});
    run("setKeyframe", {{"args", {"pelvis.ry"}}, {"time", 1.0}, {"value", 25.0}});
    run("setKeyframe", {{"args", {"pelvis.ty"}}, {"time", 1.0}, {"value", 99.0}});
    run("setKeyframe", {{"args", {"hip_l.rotate", "knee_l.rotate", "ankle_l.rotate", "ball_l.rotate", "arm_l.rotate", "elbow_l.rotate",
                                  "hand_l.rotate"}},
                        {"time", 1.0}});
    run("setKeyframe", {{"args", {"wind.tx"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"wind.tx"}}, {"time", 0.5}, {"value", 30.0}});
    run("setKeyframe", {{"args", {"wind.tx"}}, {"time", 1.0}, {"value", -10.0}});
    run("setKeyframe", {{"args", {"grip.tz"}}, {"time", 0.0}, {"value", 20.0}});
    run("setKeyframe", {{"args", {"grip.tz"}}, {"time", 1.0}, {"value", -5.0}});

    // IK: a planted foot, held by its lock while the pelvis bounces, and an arm posed through its controller.
    run("ikChain", {{"args", {"hip_l", "ankle_l"}}});
    run("reverseFoot", {{"args", {"ankle_l"}}});
    run("ikLock", {{"args", {"ankle_l"}}, {"time", 0.0}, {"state", true}});
    const std::string arm = run("ikChain", {{"args", {"arm_l", "hand_l"}}}).get<std::string>();
    run("currentTime", {{"args", {0.5}}});
    run("ikPose", {{"args", {arm}}, {"moveBy", v3(-5, 12, 8)}});
    run("currentTime", {{"args", {0.0}}});

    run("twistDriver", {{"args", {"arm_twist_l"}}});
    const std::string looker = run("looker", {{"args", {"head"}}, {"aimAxis", "z"}, {"upAxis", "y"}, {"distance", 40.0}}).get<std::string>();
    run("setKeyframe", {{"args", {looker + ".tx"}}, {"time", 0.0}, {"value", -20.0}});
    run("setKeyframe", {{"args", {looker + ".tx"}}, {"time", 1.0}, {"value", 30.0}});

    // Spaces: the prop joint goes to the world halfway.
    run("addSpace", {{"args", {"prop_r"}}, {"space", {"world"}}});
    run("switchSpace", {{"args", {"prop_r"}}, {"space", "world"}, {"time", 0.5}});
    // A constraint holding a joint on a locator.
    run("parentConstraint", {{"args", {"grip", "held"}}, {"maintainOffset", true}});

    // Physics recorded onto a layer, then an additive layer of hand keys on the spine.
    Graph& g = Graph::current();
    Node* spring = g.createNode(*types::find("cleanBakeSpring"), "spring");
    g.setValue({spring->id(), spring->type().attrIndex("target")}, std::string("wind"));
    g.setValue({spring->id(), spring->type().attrIndex("follower")}, std::string("antenna"));
    run("recordSimulation", {{"args", {"spring"}}, {"start", 0}, {"end", kFrames}, {"rotate", false}});
    g.deleteNode(g.find("spring")->id());
    run("select", {{"args", {"spine"}}});
    run("createAnimLayer", {{"name", "lean"}});
    run("setKeyframe", {{"args", {"spine.rz"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"spine.rz"}}, {"time", 1.0}, {"value", 20.0}});
    run("select", {{"args", json::array()}});
}

std::map<std::string, std::vector<glm::dmat4>> evaluated(const std::vector<std::string>& names)
{
    Graph& g = Graph::current();
    std::map<std::string, std::vector<glm::dmat4>> out;
    for (int f = 0; f <= kFrames; ++f)
    {
        g.setTime(f / kFps);
        g.evaluate();
        for (const std::string& n : names)
            out[n].push_back(g.find(n)->as<glm::dmat4>(Transform::kWorldMatrix));
    }
    g.setTime(0.0);
    return out;
}

double columnError(const glm::dmat4& a, const glm::dmat4& b)
{
    double e = 0.0;
    for (int c = 0; c < 4; ++c)
        e = std::max(e, glm::length(a[c] - b[c]));
    return e;
}

const std::vector<std::string> kDeform = {"root", "pelvis", "hip_l", "knee_l", "ankle_l", "ball_l", "toe_l", "spine", "chest", "arm_l", "elbow_l",
                                          "hand_l", "arm_twist_l", "head", "head_end", "antenna", "prop_r", "held", "ik_child"};
} // namespace

TEST_CASE("clean bake: exported samples are the evaluated world of every deform joint, whatever drives it")
{
    riggedScene();
    Graph& g = Graph::current();
    const auto want = evaluated(kDeform);
    // Every driver really moves something off its plain keys.
    CHECK(columnError(want.at("ankle_l")[0], want.at("ankle_l")[12]) < 1e-4); // the lock plants the foot
    CHECK(columnError(want.at("knee_l")[0], want.at("knee_l")[12]) > 0.5);
    CHECK(columnError(want.at("antenna")[0], want.at("antenna")[12]) > 0.5);

    fbx::ExportOptions opts;
    opts.kind = fbx::ExportKind::Animation;
    opts.start = 0.0;
    opts.end = 1.0;
    const fbx::ExportScene out = fbx::gatherExport(g, {g.find("root")->id()}, opts);
    REQUIRE(out.take);
    std::map<std::string, int> index;
    for (size_t i = 0; i < out.nodes.size(); ++i)
        index[out.nodes[i].name] = int(i);
    // Deform joints only, the hierarchy closed over the rig-only joint.
    CHECK_FALSE(index.contains("ik_helper"));
    REQUIRE(index.contains("ik_child"));
    CHECK(out.nodes[size_t(index["ik_child"])].parent == index["root"]);
    for (const std::string& n : kDeform)
        CHECK_MESSAGE(index.contains(n), n);
    CHECK(out.nodes.size() == kDeform.size());

    REQUIRE(out.take->times.size() == size_t(kFrames + 1));
    double worst = 0.0;
    std::string worstName;
    for (int f = 0; f <= kFrames; ++f)
    {
        std::vector<glm::dmat4> world(out.nodes.size());
        for (size_t i = 0; i < out.nodes.size(); ++i)
        {
            const auto& samples = out.take->locals[i];
            const glm::dmat4 local = samples.empty() ? out.nodes[i].local : samples[size_t(f)];
            world[i] = out.nodes[i].parent >= 0 ? world[size_t(out.nodes[i].parent)] * local : local;
        }
        for (const std::string& n : kDeform)
            if (double e = columnError(world[size_t(index[n])], want.at(n)[size_t(f)]); e > worst)
                worst = e, worstName = n;
    }
    CHECK_MESSAGE(worst < 1e-9, worstName);

    // Without deformOnly the rig-only joint goes out too.
    opts.deformOnly = false;
    const fbx::ExportScene all = fbx::gatherExport(g, {g.find("root")->id()}, opts);
    bool helper = false;
    for (const fbx::ExportNode& e : all.nodes)
        helper = helper || e.name == "ik_helper";
    CHECK(helper);
}

TEST_CASE("clean bake: bakeToDeformSkeleton makes a plain keyed copy of the deform joints")
{
    riggedScene();
    Graph& g = Graph::current();
    const auto want = evaluated(kDeform);
    const json tops = run("bakeToDeformSkeleton", {{"args", {"pelvis"}}, {"start", 0.0}, {"end", 1.0}, {"fps", kFps}});
    REQUIRE(tops.size() == 1);
    CHECK(tops[0] == "root_bake");
    CHECK_FALSE(g.find("ik_helper_bake"));
    REQUIRE(g.find("ik_child_bake"));
    CHECK(g.parent(g.find("ik_child_bake")->id()) == g.find("root_bake")->id());
    std::vector<std::string> copies;
    for (const std::string& n : kDeform)
        copies.push_back(n + "_bake");
    const auto got = evaluated(copies);
    double worst = 0.0;
    std::string worstName;
    for (const std::string& n : kDeform)
        for (int f = 0; f <= kFrames; ++f)
            if (double e = columnError(got.at(n + "_bake")[size_t(f)], want.at(n)[size_t(f)]); e > worst)
                worst = e, worstName = n;
    CHECK_MESSAGE(worst < 1e-6, worstName);
    // Plain keys: nothing but curves drives the copies.
    for (const auto& [dst, src] : g.connections())
        if (const std::string n = g.node(dst.node)->name(); n.ends_with("_bake"))
            CHECK_MESSAGE(g.node(src.node)->type().name.starts_with("animCurve"), n);
}
