// Animated garments: cloth simulated over the timeline against a moving (skinned or animated) collider, through Lila's
// simulation framework, which owns time, the per-frame cache, loops and scrubbing. The state is the cloth's positions and
// velocities; the inputs are the collider's world points at each step.

#include "Tailor.h"

#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/SimPreview.h>
#include <lila/Simulation.h>
#include <lila/Transform.h>
#include <lila/ViewportTool.h>

#include <QCoreApplication>

#include <map>

using namespace lila;
using namespace lila::scene;

namespace tailor
{
namespace
{
/** The latest simulated frame of each animated garment, read by the garment node's compute. */
std::map<NodeId, AnimatedFrame>& frames()
{
    static std::map<NodeId, AnimatedFrame> f;
    return f;
}

int attr(const Node& n, const char* name) { return n.type().attrIndex(name); }

bool isAnimated(const Node& n) { return n.type().isA(kGarmentType) && attr(n, "animate") >= 0 && n.as<bool>(attr(n, "animate")); }

class GarmentSim final : public sim::Solver
{
public:
    explicit GarmentSim(NodeId garment) : mGarment(garment) {}

    sim::Setup configure(Graph& g) override
    {
        const Node* op = g.node(mGarment);
        if (!op || !isAnimated(*op))
            throw std::invalid_argument("not an animated garment");
        GarmentInputs in = garmentInputs(g, *op);
        if (in.collider == kNoNode)
            throw std::invalid_argument("an animated garment needs a collider");
        mInputs = std::move(in);
        // The collider's triangles stay fixed; only its points are sampled per step.
        const PlugValue v = g.value({mInputs.collider, MeshNode::kOutMesh});
        const MeshPtr* m = std::get_if<MeshPtr>(&v);
        if (!m || !*m)
            throw std::invalid_argument("the collider has no mesh");
        std::vector<Vec3> pts;
        colliderTriangles(**m, glm::dmat4(1.0), pts, mTris);

        sim::Setup s;
        s.startFrame = op->as<int64_t>(attr(*op, "startFrame"));
        s.substeps = 1;
        const uint32_t n = uint32_t(mInputs.cloth.rest3d.size()) * 3;
        s.layout = {{sim::Channel::Position, n}, {sim::Channel::Ignore, n}};
        s.sources = {mInputs.collider};
        s.reads = {mInputs.collider};
        s.paramsHash = sim::hashBytes(&mInputs.key, sizeof mInputs.key);
        return s;
    }

    void sample(Graph& g, std::vector<double>& inputs) override
    {
        const PlugValue v = g.value({mInputs.collider, MeshNode::kOutMesh});
        const PlugValue w = g.value({mInputs.collider, Transform::kWorldMatrix});
        const MeshPtr* m = std::get_if<MeshPtr>(&v);
        const glm::dmat4* world = std::get_if<glm::dmat4>(&w);
        if (!m || !*m || !world)
            return;
        std::vector<Vec3> pts;
        std::vector<std::array<int, 3>> tris;
        colliderTriangles(**m, *world, pts, tris);
        for (const Vec3& p : pts)
            inputs.insert(inputs.end(), {p[0], p[1], p[2]});
    }

    void reset(const std::vector<double>& inputs, std::vector<double>& state) override
    {
        // Pre-roll: drape onto the collider as it stands at the start frame.
        const Sdf sdf = bodySdf(inputs);
        ClothSolver solver;
        solver.reset(mInputs.cloth, mInputs.garment, mInputs.settings);
        if (sdf.nx > 0)
            solver.setColliders({&sdf});
        const std::vector<Vec3> body = bodyPoints(inputs);
        solver.setPins(resolvePins(mInputs.cloth, mInputs.garment, &body, &mTris));
        for (int64_t k = 0; k < mInputs.frames; ++k)
            solver.step();
        write(solver, state);
        mSolver.reset();
    }

    void step(const std::vector<double>& inputs, std::vector<double>& state, const sim::StepInfo& at) override
    {
        const size_t n = mInputs.cloth.rest3d.size();
        if (state.size() < 6 * n)
            return;
        // Keep the solver between steps (seams stay closed, constraints warm); rebuild it from the state after a jump.
        if (!mSolver || sim::hashDoubles(state) != mLastHash)
        {
            ClothMesh from = mInputs.cloth;
            for (size_t i = 0; i < n; ++i)
                from.rest3d[i] = {float(state[3 * i]), float(state[3 * i + 1]), float(state[3 * i + 2])};
            SolverSettings s = mInputs.settings;
            s.dt = float(at.dt);
            mSolver = std::make_unique<ClothSolver>();
            mSolver->reset(from, mInputs.garment, s);
            std::vector<Vec3>& v = mSolver->velocitiesMutable();
            for (size_t i = 0; i < n && i < v.size(); ++i)
                v[i] = {float(state[3 * (n + i)]), float(state[3 * (n + i) + 1]), float(state[3 * (n + i) + 2])};
        }
        mSdf = bodySdf(inputs);
        if (mSdf.nx > 0)
            mSolver->setColliders({&mSdf});
        const std::vector<Vec3> body = bodyPoints(inputs);
        mSolver->setPins(resolvePins(mInputs.cloth, mInputs.garment, &body, &mTris));
        mSolver->step();
        write(*mSolver, state);
        mLastHash = sim::hashDoubles(state);
    }

    void pose(const std::vector<double>&, const std::vector<double>&, std::vector<glm::dmat4>&) override {}

private:
    static std::vector<Vec3> bodyPoints(const std::vector<double>& inputs)
    {
        std::vector<Vec3> pts(inputs.size() / 3);
        for (size_t i = 0; i < pts.size(); ++i)
            pts[i] = {float(inputs[3 * i]), float(inputs[3 * i + 1]), float(inputs[3 * i + 2])};
        return pts;
    }

    Sdf bodySdf(const std::vector<double>& inputs) const
    {
        std::vector<Vec3> pts(inputs.size() / 3);
        for (size_t i = 0; i < pts.size(); ++i)
            pts[i] = {float(inputs[3 * i]), float(inputs[3 * i + 1]), float(inputs[3 * i + 2])};
        if (pts.empty() || mTris.empty())
            return {};
        // Coarser than a still drape's grid: it is rebuilt every frame.
        return sdfFromMesh(pts, mTris, 0.015f, 0.05f);
    }

    void write(const ClothSolver& s, std::vector<double>& state) const
    {
        const auto& x = s.positions();
        const auto& v = s.velocities();
        state.assign(6 * x.size(), 0.0);
        for (size_t i = 0; i < x.size(); ++i)
            for (int k = 0; k < 3; ++k)
            {
                state[3 * i + k] = x[i][size_t(k)];
                state[3 * (x.size() + i) + k] = i < v.size() ? v[i][size_t(k)] : 0.f;
            }
    }

    NodeId mGarment;
    GarmentInputs mInputs;
    std::vector<std::array<int, 3>> mTris;
    std::unique_ptr<ClothSolver> mSolver;
    uint64_t mLastHash = 0;
    Sdf mSdf;
};

/** Brings every animated garment to the current frame: cached or nearly cached frames at once, longer runs when idle. */
void refresh(bool idle)
{
    if (sim::sampling())
        return;
    Graph& g = Graph::current();
    bool changed = false;
    for (NodeId id : sim::simulations(g))
    {
        const Node* n = g.node(id);
        if (!n || !isAnimated(*n))
            continue;
        const int64_t frame = sim::frameOf(g.time());
        AnimatedFrame& have = frames()[id];
        if (have.frame == frame && !have.positions.empty())
            continue;
        std::shared_ptr<const sim::Frame> f;
        try
        {
            f = QCoreApplication::instance() && !idle ? ui::simPreviewFrame(g, id) : sim::simulate(g, id, frame);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", n->name(), e.what());
            continue;
        }
        if (!f)
            continue;
        const size_t count = f->state.size() / 6;
        have.frame = f->frame;
        have.positions.resize(count);
        for (size_t i = 0; i < count; ++i)
            have.positions[i] = {float(f->state[3 * i]), float(f->state[3 * i + 1]), float(f->state[3 * i + 2])};
        g.touch(id);
        changed = true;
    }
    if (changed)
    {
        g.evaluate();
        ui::viewportTools::requestRedraw();
    }
}
} // namespace

const AnimatedFrame* animatedFrame(NodeId garment)
{
    const auto it = frames().find(garment);
    return it == frames().end() ? nullptr : &it->second;
}

void registerAnimation(ext::Context& ctx)
{
    sim::registerSolverType(ctx, {.nodeType = "tailorAnimatedGarment",
                                  .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> { return std::make_unique<GarmentSim>(node); },
                                  .matches = [](const Node& n) { return isAnimated(n); }});
    events::subscribe(events::Event::TimeChanged, [](const events::EventData&) { refresh(false); }, ctx.owner());
    events::subscribe(events::Event::AppIdle, [](const events::EventData&) { refresh(true); }, ctx.owner());
    // An edit may change any frame: forget what was shown and ask again.
    events::subscribe(events::Event::UndoStackChanged, [](const events::EventData&) {
        frames().clear();
        refresh(false);
    }, ctx.owner());
}
} // namespace tailor
