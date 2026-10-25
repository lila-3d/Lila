#include "scene/Jiggle.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/RigUtil.h"
#include "scene/Simulation.h"
#include "scene/Skin.h"

#include <map>
#include <mutex>
#include <set>

namespace lila::scene::jiggle
{
using namespace rigutil;

namespace
{
constexpr const char* kType = "jiggle";
/** Inputs ahead of the points: stiffness, damping, gravity xyz. */
constexpr size_t kHeader = 5;

/** Each jiggle's points for one frame, object space, published by the evaluate hook for compute to read. */
struct Published
{
    int64_t frame = 0;
    uint64_t checkedVersion = 0;
    std::shared_ptr<const sim::Frame> source;
    std::vector<glm::dvec3> points;
    std::string error;
};

struct Store
{
    std::mutex mutex;
    std::set<NodeId> nodes;
    std::map<NodeId, Published> published;
};

Store& store()
{
    static Store s;
    return s;
}

class JiggleNode : public DeformerNode
{
public:
    ~JiggleNode() override
    {
        std::lock_guard lock(store().mutex);
        store().nodes.erase(id());
        store().published.erase(id());
    }

    void compute(const EvalContext& ctx) override
    {
        {
            std::lock_guard lock(store().mutex);
            store().nodes.insert(id());
        }
        DeformerNode::compute(ctx);
    }

    bool cacheable() const override { return false; }

    void deform(const geo::Mesh&, std::vector<glm::dvec3>& points) const override
    {
        // While the framework samples, this shows its input: the input is what the simulation reads.
        if (sim::sampling())
            return;
        std::lock_guard lock(store().mutex);
        auto it = store().published.find(id());
        if (it == store().published.end() || it->second.points.size() != points.size())
            return;
        points = it->second.points;
    }
};

NodeId meshOfJiggle(Graph& g, NodeId node) { return modifiers::meshOf(g, node); }

class JiggleSolver : public sim::Solver
{
public:
    explicit JiggleSolver(NodeId node) : mNode(node) {}

    sim::Setup configure(Graph& g) override
    {
        Node* n = g.node(mNode);
        if (!n)
            throw std::invalid_argument("the jiggle is gone");
        const auto& d = static_cast<const DeformerNode&>(*n);
        mMesh = meshOfJiggle(g, mNode);
        if (mMesh == kNoNode)
            throw std::invalid_argument(n->name() + " deforms no mesh");
        MeshPtr in = std::get<MeshPtr>(g.value({mNode, DeformerNode::kInMesh, -1}));
        if (!in)
            throw std::invalid_argument(n->name() + ": the mesh has no geometry");
        mPoints = in->pointCapacity();

        sim::Setup s;
        s.startFrame = d.integer("startFrame");
        s.substeps = int(std::clamp<int64_t>(d.integer("substeps"), 1, 64));
        s.layout.assign(mPoints, {sim::Channel::Position, 3});
        s.layout.insert(s.layout.end(), mPoints, {sim::Channel::Velocity, 3});
        s.sources.push_back(mMesh);
        if (NodeId skin = skin::find(g, mMesh); skin != kNoNode)
            for (const skin::Influence& i : skin::influences(g, skin))
                if (i.joint != kNoNode)
                    s.sources.push_back(i.joint);
        s.paramsHash = sim::hashBytes(&mPoints, sizeof mPoints, sim::hashBytes(&mMesh, sizeof mMesh));
        return s;
    }

    void sample(Graph& g, std::vector<double>& inputs) override
    {
        const auto& d = static_cast<const DeformerNode&>(*g.node(mNode));
        const glm::dvec3 gravity = d.as<glm::dvec3>(d.attr("gravity"));
        inputs.insert(inputs.end(), {std::max(0.0, d.num("stiffness")), std::max(0.0, d.num("damping")), gravity.x, gravity.y, gravity.z});
        MeshPtr in = std::get<MeshPtr>(g.value({mNode, DeformerNode::kInMesh, -1}));
        const glm::dmat4 world = worldOf(*g.node(mMesh));
        for (size_t p = 0; p < mPoints; ++p)
        {
            const glm::dvec3 w = in && p < in->pointCapacity() ? glm::dvec3(world * glm::dvec4(in->position(geo::Index(p)), 1.0)) : glm::dvec3(0.0);
            inputs.insert(inputs.end(), {w.x, w.y, w.z});
        }
    }

    void reset(const std::vector<double>& inputs, std::vector<double>& state) override
    {
        state.assign(mPoints * 6, 0.0);
        std::copy(inputs.begin() + kHeader, inputs.begin() + kHeader + mPoints * 3, state.begin());
    }

    void step(const std::vector<double>& inputs, std::vector<double>& state, const sim::StepInfo& at) override
    {
        const double k = inputs[0], c = inputs[1], dt = at.dt;
        const glm::dvec3 gravity(inputs[2], inputs[3], inputs[4]);
        const double denom = 1.0 + dt * c + dt * dt * k;
        double* x = state.data();
        double* v = state.data() + mPoints * 3;
        const double* target = inputs.data() + kHeader;
        // Backward Euler on each point's spring to its input: stable however stiff, and independent per point.
        for (size_t i = 0; i < mPoints * 3; i += 3)
            for (int a = 0; a < 3; ++a)
            {
                const double vn = (v[i + a] + dt * (k * (target[i + a] - x[i + a]) + gravity[a])) / denom;
                v[i + a] = vn;
                x[i + a] += dt * vn;
            }
    }

    void pose(const std::vector<double>&, const std::vector<double>&, std::vector<glm::dmat4>& world) override { world.clear(); }

private:
    NodeId mNode;
    NodeId mMesh = kNoNode;
    size_t mPoints = 0;
};

/** After each evaluation: brings every jiggle's published points to the current frame. */
void afterEvaluate(Graph& g)
{
    if (sim::sampling())
        return;
    std::vector<NodeId> ids;
    {
        std::lock_guard lock(store().mutex);
        ids.assign(store().nodes.begin(), store().nodes.end());
    }
    if (ids.empty())
        return;
    LILA_ZONE();

    const int64_t frame = sim::frameOf(g.time());
    bool touched = false;
    for (NodeId id : ids)
    {
        Node* n = g.node(id);
        if (!n || n->type().name != kType)
            continue;
        const auto& d = static_cast<const DeformerNode&>(*n);
        if (!d.as<bool>(d.enabledAttr))
            continue;
        {
            std::lock_guard lock(store().mutex);
            Published& e = store().published[id];
            if (e.checkedVersion == g.version() && e.frame == frame && !e.points.empty())
                continue;
        }
        std::shared_ptr<const sim::Frame> f;
        std::string error;
        std::vector<glm::dvec3> local;
        try
        {
            f = sim::simulate(g, id, frame);
            const NodeId mesh = meshOfJiggle(g, id);
            const glm::dmat4 inv = glm::inverse(worldOf(*g.node(mesh)));
            local.resize(f->state.size() / 6);
            for (size_t p = 0; p < local.size(); ++p)
                local[p] = glm::dvec3(inv * glm::dvec4(f->state[p * 3], f->state[p * 3 + 1], f->state[p * 3 + 2], 1.0));
        }
        catch (const std::exception& ex)
        {
            error = ex.what();
        }
        std::lock_guard lock(store().mutex);
        Published& e = store().published[id];
        if (!error.empty())
        {
            if (error != e.error)
                log::warn("{}: {}", n->path(), error);
            e.error = error;
        }
        else
        {
            e.error.clear();
            if (f != e.source || e.frame != frame || e.points.empty())
            {
                e.source = f;
                e.frame = frame;
                e.points = std::move(local);
                g.touch(id);
                touched = true;
            }
        }
        e.checkedVersion = g.version();
    }
    if (touched)
        g.evaluate();
}

AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, bool keyable)
{
    AttrDef a{.name = std::move(name), .type = type, .defaultValue = std::move(def),
              .flags = uint32_t(AttrInput | AttrStorable | (keyable ? AttrKeyable : 0)), .doc = std::move(doc)};
    return a;
}
} // namespace

void registerTypes()
{
    if (types::find(kType))
        return;
    deformers::registerDeformer({kType, "wave-square",
                                 "Makes points trail their input on springs: flesh, bellies and loose gear that lag, overshoot and settle. "
                                 "Simulated from the start frame and cached, so scrubbing matches playback; loops with looping takes. Paint "
                                 "its weights to choose what jiggles.",
                                 {param("stiffness", PlugType::Float, 150.0, "Spring rate (per second squared): higher follows the input more tightly.", true),
                                  param("damping", PlugType::Float, 8.0, "How fast the wobble dies out (per second).", true),
                                  param("gravity", PlugType::Vec3, glm::dvec3(0.0), "Constant acceleration, world space, cm per second squared.", true),
                                  param("substeps", PlugType::Int, int64_t(2), "Solver steps per frame.", false),
                                  param("startFrame", PlugType::Int, int64_t(0), "Frame the simulation starts at rest on its input.", false)},
                                 [] { return std::make_unique<JiggleNode>(); }});
    sim::registerSolverType({.nodeType = kType, .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> {
                                 return std::make_unique<JiggleSolver>(node);
                             }});
    Graph::addEvaluateHook([](Graph& g, bool after) {
        if (after)
            afterEvaluate(g);
    });
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "jiggle",
        .doc = "Add a jiggle deformer to the top of each mesh's stack: points trail the deformation below on springs and settle. "
               "Starts at the playback start. Returns the deformers.",
        .positionalDoc = "meshes",
        .flags = {{"stiffness", "", T::Float, 150.0, "Spring rate: higher follows the input more tightly."},
                  {"damping", "", T::Float, 8.0, "How fast the wobble dies out."},
                  {"gravity", "", T::Vec3, json::array({0, 0, 0}), "Constant acceleration, world space, cm/s^2."},
                  {"substeps", "", T::Int, 2, "Solver steps per frame."},
                  {"name", "", T::String, nullptr, "Name for the deformer."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> meshes;
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "jiggle");
                if (!n.type().isA("mesh") || n.type().isA("lattice"))
                    throw CommandError("jiggle: " + name + " is not a mesh");
                meshes.push_back(n.id());
            }
            if (meshes.empty())
                throw CommandError("jiggle: give or select at least one mesh");
            const int64_t start = sim::frameOf(anim::playback().minTime);
            json out = json::array();
            for (NodeId id : deformers::addDeformer(g, kType, meshes, a.has("name") ? a.get<std::string>("name") : "Jiggle"))
            {
                const NodeType& t = g.node(id)->type();
                g.setValue({id, t.attrIndex("stiffness"), -1}, a.get<double>("stiffness"));
                g.setValue({id, t.attrIndex("damping"), -1}, a.get<double>("damping"));
                g.setValue({id, t.attrIndex("gravity"), -1}, vec3Of(a.flags["gravity"]));
                g.setValue({id, t.attrIndex("substeps"), -1}, int64_t(std::max(1, a.get<int>("substeps"))));
                g.setValue({id, t.attrIndex("startFrame"), -1}, start);
                out.push_back(g.node(id)->name());
            }
            return out;
        },
    });
}
} // namespace lila::scene::jiggle
