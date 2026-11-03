#include "gn/Stateful.h"

#include "gn/Exec.h"
#include "gn/Gn.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"

#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>

namespace lila::gn
{
using scene::Graph;
using scene::Node;
using scene::NodeId;
namespace sim = scene::sim;

namespace
{
uint64_t hashOf(const void* data, size_t size, uint64_t h) { return sim::hashBytes(data, size, h); }

template <class T> uint64_t hashPod(const T& v, uint64_t h) { return hashOf(&v, sizeof v, h); }

uint64_t hashString(const std::string& s, uint64_t h)
{
    h = hashPod(s.size(), h);
    return hashOf(s.data(), s.size(), h);
}

uint64_t hashAttributes(const geo::AttributeSet& set, uint64_t h)
{
    h = hashPod(set.layerCount(), h);
    for (size_t i = 0; i < set.layerCount(); ++i)
    {
        const geo::Attribute& a = set.layer(i);
        h = hashString(a.name(), h);
        h = hashPod(uint8_t(a.type()), h);
        if (a.size() > 0)
            h = hashOf(a.raw(0), a.size() * a.stride(), h);
    }
    return h;
}

uint64_t hashMesh(const geo::Mesh& m, uint64_t h)
{
    std::span<const glm::dvec3> pos = m.positions();
    h = hashPod(pos.size(), h);
    h = hashOf(pos.data(), pos.size_bytes(), h);
    std::vector<geo::Index> corners;
    corners.reserve(size_t(m.numCorners()) + size_t(m.numFaces()));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            corners.push_back(m.faceSize(f));
            m.forEachFaceCorner(f, [&](geo::Index c) { corners.push_back(m.cornerPoint(c)); });
        }
    h = hashOf(corners.data(), corners.size() * sizeof(geo::Index), h);
    for (size_t d = 0; d < size_t(geo::Domain::Count); ++d)
        h = hashAttributes(m.attributes(geo::Domain(d)), h);
    for (const std::string& name : m.materialNames())
        h = hashString(name, h);
    return h;
}

/** Hashes of immutable components by address, checked against a weak pointer so a reused address never hits. */
struct HashMemo
{
    std::mutex mutex;
    std::map<const void*, std::pair<std::weak_ptr<const void>, uint64_t>> entries;

    template <class T> uint64_t get(const std::shared_ptr<const T>& p, const std::function<uint64_t()>& compute)
    {
        {
            std::lock_guard lock(mutex);
            if (auto it = entries.find(p.get()); it != entries.end())
            {
                if (!it->second.first.expired() && it->second.first.lock().get() == static_cast<const void*>(p.get()))
                    return it->second.second;
                entries.erase(it);
            }
        }
        const uint64_t h = compute();
        std::lock_guard lock(mutex);
        if (entries.size() > 512)
            std::erase_if(entries, [](const auto& e) { return e.second.first.expired(); });
        entries[p.get()] = {std::shared_ptr<const void>(p, p.get()), h};
        return h;
    }
};

HashMemo& memo()
{
    // Leaked: its weak_ptrs can hold control blocks made in extension DLLs, which are unloaded before LilaScene's statics die.
    static HashMemo* m = new HashMemo;
    return *m;
}

uint64_t hashComponent(const std::shared_ptr<const geo::Mesh>& m)
{
    return memo().get(m, [&] { return hashMesh(*m, 0x9e3779b97f4a7c15ull); });
}

uint64_t hashComponent(const std::shared_ptr<const CurveSet>& c)
{
    return memo().get(c, [&] {
        uint64_t h = 0x9e3779b97f4a7c15ull;
        h = hashOf(c->positions.data(), c->positions.size() * sizeof(glm::dvec3), h);
        h = hashOf(c->offsets.data(), c->offsets.size() * sizeof(geo::Index), h);
        h = hashOf(c->cyclic.data(), c->cyclic.size(), h);
        h = hashAttributes(c->pointAttrs, h);
        h = hashAttributes(c->curveAttrs, h);
        for (const std::vector<double>& k : c->knots)
        {
            h = hashPod(k.size(), h);
            h = hashOf(k.data(), k.size() * sizeof(double), h);
        }
        return h;
    });
}

uint64_t hashComponent(const std::shared_ptr<const Instances>& inst)
{
    return memo().get(inst, [&] {
        uint64_t h = 0x9e3779b97f4a7c15ull;
        h = hashOf(inst->transforms.data(), inst->transforms.size() * sizeof(glm::dmat4), h);
        h = hashOf(inst->reference.data(), inst->reference.size() * sizeof(int), h);
        for (const GeometryPtr& r : inst->references)
            h = r ? hashGeometry(*r, h) : hashPod(uint64_t(0), h);
        return hashAttributes(inst->attrs, h);
    });
}

/** What a modifier running a simulation group has published, and the last frame handed back to it. */
struct Entry
{
    std::shared_ptr<const SimInputs> inputs;
    bool inputsUseTime = false;
    int64_t frame = std::numeric_limits<int64_t>::min();
    std::shared_ptr<const std::vector<uint8_t>> state;
    std::shared_ptr<const sim::Frame> source;
    uint64_t checkedVersion = 0;
    std::string error;
};

struct SimStore
{
    std::mutex mutex;
    std::map<NodeId, Entry> entries;
    std::set<NodeId> simulating;
};

SimStore& simStore()
{
    // Never destroyed: the scene graph's static nodes clear their entries during exit, after this would be gone.
    static SimStore* s = new SimStore;
    return *s;
}

std::shared_ptr<const SimInputs> latestInputs(NodeId modifier)
{
    std::lock_guard lock(simStore().mutex);
    auto it = simStore().entries.find(modifier);
    return it == simStore().entries.end() ? nullptr : it->second.inputs;
}

void pack(const std::vector<uint8_t>& bytes, std::vector<double>& state)
{
    state.assign(1 + (bytes.size() + sizeof(double) - 1) / sizeof(double), 0.0);
    state[0] = double(bytes.size());
    if (!bytes.empty())
        std::memcpy(state.data() + 1, bytes.data(), bytes.size());
}

std::vector<uint8_t> unpack(const std::vector<double>& state)
{
    if (state.empty())
        return {};
    const size_t n = std::min(size_t(state[0]), (state.size() - 1) * sizeof(double));
    std::vector<uint8_t> bytes(n);
    if (n)
        std::memcpy(bytes.data(), state.data() + 1, n);
    return bytes;
}

/** Whether anything feeding the modifier in the scene can change with time (keys, expressions, other simulations). */
bool upstreamUsesTime(Graph& g, NodeId modifier)
{
    std::multimap<NodeId, NodeId> sources;
    for (const auto& [dst, src] : g.connections())
        sources.emplace(dst.node, src.node);
    std::set<NodeId> seen{modifier};
    std::vector<NodeId> stack{modifier};
    while (!stack.empty())
    {
        NodeId cur = stack.back();
        stack.pop_back();
        auto visit = [&](NodeId up) {
            if (up == scene::kNoNode || !seen.insert(up).second)
                return false;
            const Node* n = g.node(up);
            if (n && n->type().timeDependent)
                return true;
            stack.push_back(up);
            return false;
        };
        for (auto [it, end] = sources.equal_range(cur); it != end; ++it)
            if (visit(it->second))
                return true;
        if (visit(g.parent(cur)))
            return true;
    }
    return false;
}

/** The framework's solver for a modifier whose group holds a simulation node. */
class GnSolver final : public sim::Solver
{
public:
    explicit GnSolver(NodeId modifier) : mModifier(modifier) {}

    sim::Setup configure(Graph&) override
    {
        std::shared_ptr<const SimInputs> latest = latestInputs(mModifier);
        if (!latest)
            throw std::invalid_argument("its geometry nodes haven't been evaluated yet");
        const NodeDecl* d = findDecl(latest->node().type);
        if (!d || !d->stateful || !d->stateful->create)
            throw std::invalid_argument(latest->node().name + " is not a simulation node");
        const std::string identity = latest->node().type + "/" + latest->node().name;
        if (!mSim || mIdentity != identity)
        {
            mSim = d->stateful->create();
            mIdentity = identity;
        }
        NodeSim::Setup ns = mSim->configure(*latest);
        sim::Setup su;
        su.startFrame = ns.startFrame;
        su.substeps = std::max(1, ns.substeps);
        su.variableState = true;
        su.paramsHash = hashString(identity, ns.paramsHash);
        return su;
    }

    void sample(Graph& g, std::vector<double>& inputs) override
    {
        std::shared_ptr<const SimInputs> in = latestInputs(mModifier);
        if (!in || in->time() != g.time())
            throw std::runtime_error("the simulation's inputs weren't evaluated (is the modifier enabled?)");
        mCurrent = std::move(in);
        const uint64_t h = mCurrent->hash();
        double d = 0.0;
        std::memcpy(&d, &h, sizeof d);
        inputs.push_back(d);
    }

    void reset(const std::vector<double>&, std::vector<double>& state) override
    {
        LILA_ZONE();

        std::vector<uint8_t> bytes;
        mSim->reset(*mCurrent, bytes);
        mStartHash = mCurrent->hash();
        pack(bytes, state);
    }

    void step(const std::vector<double>&, std::vector<double>& state, const sim::StepInfo& at) override
    {
        LILA_ZONE();

        std::vector<uint8_t> bytes = unpack(state);
        mSim->step(*mCurrent, bytes, at);
        pack(bytes, state);
    }

    void pose(const std::vector<double>&, const std::vector<double>&, std::vector<glm::dmat4>& world) override { world.clear(); }

    bool inputsMayHaveChanged(Graph& g) override
    {
        bool inputsUseTime = true;
        std::shared_ptr<const SimInputs> latest;
        {
            std::lock_guard lock(simStore().mutex);
            if (auto it = simStore().entries.find(mModifier); it != simStore().entries.end())
            {
                latest = it->second.inputs;
                inputsUseTime = it->second.inputsUseTime;
            }
        }
        if (!latest || inputsUseTime || upstreamUsesTime(g, mModifier))
            return true;
        return latest->hash() != mStartHash;
    }

private:
    NodeId mModifier;
    std::unique_ptr<NodeSim> mSim;
    std::string mIdentity;
    std::shared_ptr<const SimInputs> mCurrent;
    uint64_t mStartHash = 0;
};

/** Before an evaluation: hands each simulating modifier its frame straight from the cache, so it computes once. */
void beforeEvaluate(Graph& g)
{
    std::vector<NodeId> ids;
    {
        std::lock_guard lock(simStore().mutex);
        ids.assign(simStore().simulating.begin(), simStore().simulating.end());
    }
    if (ids.empty())
        return;
    const int64_t frame = sim::frameOf(g.time());
    for (NodeId id : ids)
    {
        {
            std::lock_guard lock(simStore().mutex);
            auto it = simStore().entries.find(id);
            if (it == simStore().entries.end() || it->second.frame == frame)
                continue;
        }
        if (!g.node(id))
            continue;
        std::shared_ptr<const sim::Frame> f = sim::cached(g, id, frame);
        if (!f)
            continue;
        std::lock_guard lock(simStore().mutex);
        Entry& e = simStore().entries[id];
        e.source = f;
        e.state = std::make_shared<const std::vector<uint8_t>>(unpack(f->state));
        e.frame = frame;
        g.touch(id);
    }
}

/** After each evaluation: brings every simulating modifier's published state to the current frame. */
void afterEvaluate(Graph& g)
{
    // The framework is moving time itself (a command asked for a frame's state directly): simulating from here would
    // step the same simulation again in the middle of a step.
    if (sim::sampling())
        return;
    std::vector<NodeId> ids;
    {
        std::lock_guard lock(simStore().mutex);
        ids.assign(simStore().simulating.begin(), simStore().simulating.end());
    }
    if (ids.empty())
        return;
    LILA_ZONE();

    const int64_t frame = sim::frameOf(g.time());
    bool touched = false;
    for (NodeId id : ids)
    {
        Node* n = g.node(id);
        if (!n || !isGnModifier(*n))
        {
            forgetSimulationModifier(id);
            continue;
        }
        {
            std::lock_guard lock(simStore().mutex);
            Entry& e = simStore().entries[id];
            if (e.checkedVersion == g.version() && e.frame == frame)
                continue;
        }
        std::shared_ptr<const sim::Frame> f;
        std::string error;
        try
        {
            f = sim::simulate(g, id, frame);
        }
        catch (const std::exception& ex)
        {
            error = ex.what();
        }
        std::lock_guard lock(simStore().mutex);
        Entry& e = simStore().entries[id];
        if (!error.empty())
        {
            if (error != e.error)
                log::warn("{}: {}", n->path(), error);
            e.error = error;
        }
        else
        {
            e.error.clear();
            if (f != e.source || e.frame != frame)
            {
                e.source = f;
                e.state = std::make_shared<const std::vector<uint8_t>>(unpack(f->state));
                e.frame = frame;
                g.touch(id);
                touched = true;
            }
        }
        e.checkedVersion = g.version();
    }
    if (touched)
        g.evaluate();
}
} // namespace

SimInputs::SimInputs(const GNode& node, const NodeDecl& decl, Sockets sockets, std::vector<std::vector<Value>> inputs, double time)
    : mNode(node), mDecl(&decl), mSockets(std::move(sockets)), mInputs(std::move(inputs)), mTime(time)
{
    uint64_t h = hashString(mNode.type, 0xcbf29ce484222325ull);
    for (size_t k = 0; k < mSockets.inputs.size() && k < mInputs.size(); ++k)
    {
        const SocketDecl& sd = mSockets.inputs[k];
        h = hashString(sd.id, h);
        for (const Value& v : mInputs[k])
        {
            switch (sd.type)
            {
            case SocketType::Geometry: h = v.geometry ? hashGeometry(*v.geometry, h) : hashPod(uint64_t(0), h); break;
            case SocketType::String: h = hashString(v.string, h); break;
            case SocketType::Float: h = hashPod(evaluateSingle<double>(v.field), h); break;
            case SocketType::Int: h = hashPod(evaluateSingle<int64_t>(v.field), h); break;
            case SocketType::Bool: h = hashPod(evaluateSingle<uint8_t>(v.field), h); break;
            case SocketType::Color: h = hashPod(evaluateSingle<glm::dvec4>(v.field), h); break;
            case SocketType::Matrix: h = hashPod(evaluateSingle<glm::dmat4>(v.field), h); break;
            case SocketType::Bundle:
            case SocketType::Closure: h = hashPod(uintptr_t(v.bundle.get()) ^ uintptr_t(v.closure.get()), h); break;
            default: h = hashPod(evaluateSingle<glm::dvec3>(v.field), h); break;
            }
        }
    }
    for (const PropDecl& p : decl.props)
        h = hashString(decl.propValue(mNode, p.id), h);
    mHash = h;
}

const Value& SimInputs::in(std::string_view id) const
{
    static const Value empty;
    int k = mSockets.inputIndex(id);
    if (k < 0 || size_t(k) >= mInputs.size() || mInputs[size_t(k)].empty())
        return empty;
    return mInputs[size_t(k)][0];
}

GeometryPtr SimInputs::geometry(std::string_view id) const
{
    const Value& v = in(id);
    return v.geometry ? v.geometry : makeGeometry({});
}

double SimInputs::f(std::string_view id) const { return evaluateSingle<double>(in(id).field); }
int64_t SimInputs::i(std::string_view id) const { return evaluateSingle<int64_t>(in(id).field); }
bool SimInputs::b(std::string_view id) const { return evaluateSingle<uint8_t>(in(id).field) != 0; }
glm::dvec3 SimInputs::v(std::string_view id) const { return evaluateSingle<glm::dvec3>(in(id).field); }
std::string SimInputs::s(std::string_view id) const { return in(id).string; }

uint64_t hashGeometry(const Geometry& g, uint64_t h)
{
    h = hashPod(uint64_t(g.mesh ? hashComponent(g.mesh) : 0), h);
    h = hashPod(uint64_t(g.points ? hashComponent(g.points) : 0), h);
    h = hashPod(uint64_t(g.curves ? hashComponent(g.curves) : 0), h);
    return hashPod(uint64_t(g.instances ? hashComponent(g.instances) : 0), h);
}

void execStateful(ExecContext& c)
{
    const StatefulDecl& sd = *c.decl.stateful;
    const NodeId owner = NodeId(c.env.owner);
    if (owner == scene::kNoNode)
    {
        if (sd.output)
            sd.output(c, nullptr);
        return;
    }
    auto inputs = std::make_shared<const SimInputs>(c.node, c.decl, c.sockets, c.inputs, c.env.time);
    std::shared_ptr<const std::vector<uint8_t>> state;
    const int64_t frame = sim::frameOf(c.env.time);
    {
        std::lock_guard lock(simStore().mutex);
        Entry& e = simStore().entries[owner];
        e.inputs = std::move(inputs);
        if (e.frame == frame)
            state = e.state;
    }
    if (c.env.sampleOnly)
        return;
    if (sd.output)
        sd.output(c, state.get());
}

std::vector<uint8_t> simulationState(Graph& g, NodeId modifier, int64_t frame) { return unpack(sim::simulate(g, modifier, frame)->state); }

std::vector<uint8_t> cachedSimulationState(Graph& g, NodeId modifier, int64_t frame)
{
    std::shared_ptr<const sim::Frame> f = sim::cached(g, modifier, frame);
    return f ? unpack(f->state) : std::vector<uint8_t>{};
}

std::shared_ptr<const SimInputs> simulationInputs(NodeId modifier) { return latestInputs(modifier); }

bool simulationStateReady(NodeId modifier, double time)
{
    std::lock_guard lock(simStore().mutex);
    auto it = simStore().entries.find(modifier);
    return it != simStore().entries.end() && it->second.state && it->second.frame == sim::frameOf(time);
}

void restoreSimulationInputs(NodeId modifier, std::shared_ptr<const SimInputs> inputs)
{
    std::lock_guard lock(simStore().mutex);
    if (simStore().simulating.contains(modifier))
        simStore().entries[modifier].inputs = std::move(inputs);
}

bool hasStateful(const Group& g)
{
    for (const GNode& n : g.nodes)
        if (const NodeDecl* d = findDecl(n.type); d && (d->stateful || d->type == "SimulationInput"))
            return true;
    return false;
}

void registerSimulationSupport()
{
    if (sim::solverTypes().find("gnSimulation"))
        return;
    sim::registerSolverType({
        .nodeType = "gnSimulation",
        .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> { return std::make_unique<GnSolver>(node); },
        .matches =
            [](const Node& n) {
                std::lock_guard lock(simStore().mutex);
                return simStore().simulating.contains(n.id());
            },
    });
    Graph::setEvaluateHook([](Graph& g, bool after) {
        if (after)
            afterEvaluate(g);
        else
            beforeEvaluate(g);
    });
}

void noteSimulationModifier(NodeId modifier, bool simulating, bool inputsUseTime)
{
    std::lock_guard lock(simStore().mutex);
    if (simulating)
    {
        simStore().simulating.insert(modifier);
        simStore().entries[modifier].inputsUseTime = inputsUseTime;
    }
    else if (simStore().simulating.erase(modifier))
        simStore().entries.erase(modifier);
}

void forgetSimulationModifier(NodeId modifier)
{
    std::lock_guard lock(simStore().mutex);
    simStore().simulating.erase(modifier);
    simStore().entries.erase(modifier);
}
} // namespace lila::gn
