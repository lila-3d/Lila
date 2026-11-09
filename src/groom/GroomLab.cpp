#include "groom/GroomLab.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/NodeHelpers.h"
#include "geo/Mesh.h"
#include "groom/Attrs.h"
#include "groom/Groom.h"
#include "groom/GroomEvaluator.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Simulation.h"

#include <groomlab/guide_set.h>
#include <groomlab/scalp_history.h>
#include <groomlab/scalp_mesh.h>
#include <groomlab/strand_set.h>

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>

namespace lila::groom
{
namespace groomLabBridge
{
groomlab::Vec3f toVec(const glm::dvec3& p)
{
    return {float(p.x), float(p.y), float(p.z)};
}

/** The mesh's live points in order, moved by `matrix`: the scalp's points as scalpMesh numbers them. */
std::vector<groomlab::Vec3f> worldPoints(const geo::Mesh& mesh, const glm::dmat4& matrix)
{
    std::vector<groomlab::Vec3f> points;
    points.reserve(mesh.numPoints());
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            points.push_back(toVec(glm::dvec3(matrix * glm::dvec4(mesh.position(p), 1.0))));
    return points;
}

/** The scalp's faces fanned into triangles over its live points, renumbered densely and moved by `matrix`. */
std::shared_ptr<const groomlab::ScalpMesh> scalpMesh(const geo::Mesh& mesh, const glm::dmat4& matrix = glm::dmat4(1.0))
{
    std::vector<int32_t> remap(mesh.pointCapacity(), -1);
    int32_t next = 0;
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            remap[p] = next++;
    std::vector<groomlab::ScalpMesh::Triangle> triangles;
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> v = mesh.facePoints(f);
        for (size_t i = 1; i + 1 < v.size(); ++i)
            triangles.push_back({remap[v[0]], remap[v[i]], remap[v[i + 1]]});
    }
    return std::make_shared<groomlab::ScalpMesh>(worldPoints(mesh, matrix), std::move(triangles));
}

std::shared_ptr<const groomlab::GuideSet> guideSet(const Splines& guides, const groomlab::ScalpMesh& scalp)
{
    auto set = std::make_shared<groomlab::GuideSet>();
    uint64_t next = 1;
    for (const Spline& s : guides)
    {
        if (s.size() < 2)
            continue;
        std::vector<groomlab::Vec3f> points;
        points.reserve(s.size());
        for (const glm::dvec3& p : s.points)
            points.push_back(toVec(p));
        set->add(next++, std::move(points));
    }
    set->rootOnMesh(scalp);
    return set;
}

Splines toSplines(const groomlab::StrandSet& strands)
{
    Splines out(strands.strandCount());
    const std::vector<float>& widths = strands.widths();
    const bool hasWidths = widths.size() == strands.pointCount();
    for (size_t i = 0; i < out.size(); ++i)
    {
        Spline& s = out[i];
        s.kind = Kind::Poly;
        const size_t n = strands.strandPointCount(i), begin = strands.strandBegin(i);
        const groomlab::Vec3f* p = strands.strandPoints(i);
        s.points.reserve(n);
        for (size_t k = 0; k < n; ++k)
            s.points.push_back({p[k].x, p[k].y, p[k].z});
        if (hasWidths)
            s.pointAttrs[std::string(attr::kWidth)].assign(widths.begin() + ptrdiff_t(begin), widths.begin() + ptrdiff_t(begin + n));
        s.attrs[std::string(attr::kId)] = double(strands.ids()[i]);
    }
    return out;
}

void setGraph(GroomEvaluator& evaluator, const std::string& graphJson)
{
    try
    {
        evaluator.setGraphJson(graphJson);
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error(std::string("groom graph: ") + e.what());
    }
}

/** The evaluator's strands at `seconds` on its scalp and guides as set; throws when it makes none. */
const GroomEvaluator::Result& evaluateAt(GroomEvaluator& evaluator, double seconds)
{
    GroomEvaluator::Request request;
    request.time = seconds;
    const GroomEvaluator::Result& result = evaluator.evaluate(request);
    if (!result.strands)
        throw std::runtime_error(result.errors.empty() ? std::string("groom graph: its output makes no strands") : result.errors.front());
    return result;
}

std::vector<std::string> warningsOf(const GroomEvaluator& evaluator, const GroomEvaluator::Result& result)
{
    std::vector<std::string> out = result.errors;
    for (const auto& [name, note] : evaluator.nodeNotes())
        out.push_back(name + ": " + note);
    return out;
}
} // namespace groomLabBridge

GrownStrands growStrands(const geo::Mesh& scalp, const Splines& guides, const std::string& graphJson, double seconds)
{
    LILA_ZONE();
    using namespace groomLabBridge;
    GroomEvaluator evaluator;
    // One evaluation: the first area measured is the reference, so k would be 1 anyway.
    evaluator.setScaleWithScalp(false);
    setGraph(evaluator, graphJson);
    const std::shared_ptr<const groomlab::ScalpMesh> mesh = scalpMesh(scalp);
    evaluator.setScalp(mesh);
    evaluator.setGuides(guides.empty() ? nullptr : guideSet(guides, *mesh));
    const GroomEvaluator::Result& result = evaluateAt(evaluator, seconds);
    GrownStrands out;
    out.strands = toSplines(*result.strands);
    out.warnings = warningsOf(evaluator, result);
    return out;
}

namespace
{
using namespace scene;
using namespace curve::helpers;

class GroomStrandsNode;

/** The groomStrands nodes computed so far, for the hook that reads their scalps' earlier frames. */
struct Registry
{
    std::mutex mutex;
    std::set<NodeId> nodes;
};

Registry& registry()
{
    static Registry r;
    return r;
}

/** Strands grown on the scalp by a GroomLab graph, guided by the incoming curves. */
class GroomStrandsNode final : public Node
{
public:
    ~GroomStrandsNode() override
    {
        std::lock_guard lock(registry().mutex);
        registry().nodes.erase(id());
    }

    void compute(const EvalContext& ctx) override
    {
        LILA_ZONE();
        bool first = false;
        {
            std::lock_guard lock(registry().mutex);
            first = registry().nodes.insert(id()).second;
        }
        using namespace groomLabBridge;
        const MeshPtr& m = as<MeshPtr>(idx(*this, "inMesh"));
        const double seconds = b(*this, "useSceneTime") ? ctx.time : f(*this, "time");
        std::string warning;
        DataPtr out;
        int64_t count = 0;
        if (m && m->numFaces() > 0)
        {
            try
            {
                const glm::dmat4& matrix = as<glm::dmat4>(idx(*this, "meshMatrix"));
                if (!mEvaluator)
                    mEvaluator = std::make_unique<GroomEvaluator>();
                GroomEvaluator& evaluator = *mEvaluator;
                setGraph(evaluator, s(*this, "graph"));
                evaluator.setFramesPerSecond(anim::playback().fps);
                const bool scale = b(*this, "scaleWithScalp");
                evaluator.setScaleWithScalp(scale);
                evaluator.setScaleReference(f(*this, "scaleReference"));
                if (m != mScalpFrom || matrix != mScalpMatrix || !mScalp)
                {
                    mScalp = scalpMesh(*m, matrix);
                    mScalpFrom = m;
                    mScalpMatrix = matrix;
                    mGuidesFrom.reset();
                }
                evaluator.setScalp(mScalp);
                const DataPtr& guides = as<DataPtr>(idx(*this, "inCurve"));
                if (guides != mGuidesFrom || !mGuides)
                {
                    const Splines& curves = curve::splinesOf(value(idx(*this, "inCurve")));
                    mGuides = curves.empty() ? nullptr : guideSet(curves, *mScalp);
                    mGuidesFrom = guides;
                }
                evaluator.setGuides(mGuides);
                evaluator.setScalpHistory(historyAt(ctx.time));
                const GroomEvaluator::Result& result = evaluateAt(evaluator, seconds);
                mMissedHistory = !historyAt(ctx.time) && evaluator.scalpHistoryFrames() > 0;
                mMissedFeed = first && steppedPast(evaluator);
                mReferenceToStore = scale && !(f(*this, "scaleReference") > 0.0) ? evaluator.scaleReference() : 0.0;
                // A frame that grows the same strands (nothing in the graph moves) hands on the same curve data.
                if (result.strands != mGrownFrom || !mGrown)
                {
                    mGrown = curve::makeCurveData(toSplines(*result.strands));
                    mGrownFrom = result.strands;
                    mGrownCount = int64_t(result.strands->strandCount());
                }
                out = mGrown;
                count = mGrownCount;
                for (const std::string& w : warningsOf(evaluator, result))
                    warning += (warning.empty() ? "" : "; ") + w;
            }
            catch (const std::exception& e)
            {
                warning = e.what();
            }
        }
        if (!out)
            out = curve::makeCurveData(Splines());
        if (!warning.empty() && warning != as<std::string>(idx(*this, "warning")))
            log::warn("{}: {}", name(), warning);
        setOutput(idx(*this, "strandCount"), count);
        setOutput(idx(*this, "outCurve"), out);
        setOutput(idx(*this, "warning"), warning);
    }

    /**
     * Before an evaluation at `now` (scene seconds): reads the scalp's earlier frames the graph needs (its Jiggle
     * nodes'), moving the scene's time to each one not read yet. True when the node has a new history to compute with.
     */
    bool sampleHistory(Graph& g, double now)
    {
        if (!mEvaluator)
            mEvaluator = std::make_unique<GroomEvaluator>();
        try
        {
            mEvaluator->setGraphJson(s(*this, "graph"));
        }
        catch (const std::exception&)
        {
            return dropHistory();
        }
        const int frames = mEvaluator->scalpHistoryFrames();
        if (frames <= 0)
            return dropHistory();
        const std::optional<PlugRef> meshSource = g.source({id(), idx(*this, "inMesh"), -1});
        if (!meshSource)
            return dropHistory();
        const std::optional<PlugRef> matrixSource = g.source({id(), idx(*this, "meshMatrix"), -1});
        std::vector<NodeId> reads{meshSource->node};
        if (matrixSource)
            reads.push_back(matrixSource->node);
        const auto pointsNow = [&]() -> std::vector<groomlab::Vec3f> {
            g.evaluateUpstream(reads);
            const Node* source = g.node(meshSource->node);
            const MeshPtr mesh = source ? source->as<MeshPtr>(meshSource->attr) : MeshPtr();
            if (!mesh)
                return {};
            const Node* matrixNode = matrixSource ? g.node(matrixSource->node) : nullptr;
            return groomLabBridge::worldPoints(*mesh, matrixNode ? matrixNode->as<glm::dmat4>(matrixSource->attr) : glm::dmat4(1.0));
        };
        const std::vector<groomlab::Vec3f> current = pointsNow();
        if (current.empty())
            return dropHistory();

        const double fps = anim::playback().fps;
        const auto key = [](double seconds) { return std::int64_t(std::llround(seconds * 1e6)); };
        std::vector<double> times;
        std::vector<std::int64_t> keys;
        for (int k = 1; k <= frames; ++k)
        {
            times.push_back(anim::secondsOf(anim::frameOf(now) - k));
            keys.push_back(key(times.back()));
        }
        std::vector<double> loopSeconds;
        for (const double frame : mEvaluator->scalpHistoryLoopFrames(anim::frameOf(now)))
        {
            times.push_back(anim::secondsOf(frame));
            loopSeconds.push_back(times.back());
            keys.push_back(key(times.back()));
        }
        bool moved = false;
        const groomlab::ScalpHistoryPtr history = mSampler.history(
            key(now), keys, 1.0 / fps, current,
            [&](std::size_t k) {
                moved = true;
                g.setTime(times[k], false);
                return pointsNow();
            },
            false, loopSeconds);
        if (moved)
            g.setTime(now, false);
        const bool changed = history != mHistory || mHistoryAt != now;
        mHistory = history;
        mHistoryAt = now;
        return changed;
    }

    /**
     * Before an evaluation at `now`: steps the graph's Simulate nodes over the frames before `now` they haven't stepped,
     * moving the scene's time to each and reading the scalp and guides there, so a jump ahead simulates as playing does.
     * True when it stepped any.
     */
    bool feedSimulation(Graph& g, double now)
    {
        mMissedFeed = false;
        if (!b(*this, "useSceneTime"))
            return false;
        if (!mEvaluator)
            mEvaluator = std::make_unique<GroomEvaluator>();
        try
        {
            mEvaluator->setGraphJson(s(*this, "graph"));
        }
        catch (const std::exception&)
        {
            return false;
        }
        const std::optional<PlugRef> meshSource = g.source({id(), idx(*this, "inMesh"), -1});
        if (!meshSource)
            return false;
        const std::optional<PlugRef> matrixSource = g.source({id(), idx(*this, "meshMatrix"), -1});
        const std::optional<PlugRef> guideSource = g.source({id(), idx(*this, "inCurve"), -1});
        std::vector<NodeId> reads{meshSource->node};
        if (matrixSource)
            reads.push_back(matrixSource->node);
        if (guideSource)
            reads.push_back(guideSource->node);
        GroomEvaluator& evaluator = *mEvaluator;
        evaluator.setFramesPerSecond(anim::playback().fps);
        bool moved = false;
        evaluator.setSimulationFeed([&](double seconds) {
            moved = true;
            g.setTime(seconds, false);
            g.evaluateUpstream(reads);
            const Node* source = g.node(meshSource->node);
            const MeshPtr mesh = source ? source->as<MeshPtr>(meshSource->attr) : MeshPtr();
            if (!mesh || mesh->numFaces() == 0)
                return false;
            const Node* matrixNode = matrixSource ? g.node(matrixSource->node) : nullptr;
            const auto scalp =
                groomLabBridge::scalpMesh(*mesh, matrixNode ? matrixNode->as<glm::dmat4>(matrixSource->attr) : glm::dmat4(1.0));
            const Node* guideNode = guideSource ? g.node(guideSource->node) : nullptr;
            static const Splines none;
            const Splines& curves = guideNode ? curve::splinesOf(guideNode->value(guideSource->attr)) : none;
            evaluator.setScalp(scalp);
            evaluator.setGuides(curves.empty() ? nullptr : groomLabBridge::guideSet(curves, *scalp));
            return true;
        });
        const groomlab::SimRun run = evaluator.simulateBefore(now);
        evaluator.setSimulationFeed({});
        if (moved)
            g.setTime(now, false);
        return run.frames > 0;
    }

    /** The last compute needed the scalp's earlier frames and had none (its first, before the hook knew its graph). */
    bool missedHistory() const { return mMissedHistory; }

    /** The node's first compute stepped its Simulate nodes over earlier frames without the scalp there: they start again
     * through feedSimulation. */
    bool missedFeed() const { return mMissedFeed; }

    /** The area the groom is 1x at, measured on a compute while Scale Reference was 0, for the hook to store (0: none). */
    double referenceToStore() const { return mReferenceToStore; }

    void restartSimulation()
    {
        if (mEvaluator)
            mEvaluator->invalidateSimulation();
    }

private:
    static bool steppedPast(const GroomEvaluator& evaluator)
    {
        for (const groomlab::SimStatus& status : evaluator.simulationStatus())
            if (status.frames > 1)
                return true;
        return false;
    }

    groomlab::ScalpHistoryPtr historyAt(double now) const { return mHistory && mHistoryAt == now ? mHistory : nullptr; }

    bool dropHistory()
    {
        const bool had = mHistory != nullptr;
        mHistory.reset();
        mSampler.clear();
        return had;
    }

    /** Keeps the graph's results between evaluations, so an edit re-runs only the nodes it reaches. */
    std::unique_ptr<GroomEvaluator> mEvaluator;
    MeshPtr mScalpFrom;
    glm::dmat4 mScalpMatrix{1.0};
    std::shared_ptr<const groomlab::ScalpMesh> mScalp;
    DataPtr mGuidesFrom;
    std::shared_ptr<const groomlab::GuideSet> mGuides;
    groomlab::StrandsPtr mGrownFrom;
    DataPtr mGrown;
    int64_t mGrownCount = 0;
    groomlab::ScalpHistorySampler mSampler;
    groomlab::ScalpHistoryPtr mHistory;
    double mHistoryAt = 0.0;
    bool mMissedHistory = false;
    bool mMissedFeed = false;
    double mReferenceToStore = 0.0;
};

/**
 * Before each evaluation every groomStrands node whose graph reads the scalp's earlier frames gets them and its Simulate
 * nodes step the frames skipped; after it, a node that computed without them (its first compute) gets them and computes
 * again, and a scalp area measured as the scale reference is stored on the node.
 */
void sampleHistories(Graph& g, bool after)
{
    if (sim::sampling())
        return;
    std::vector<NodeId> ids;
    {
        std::lock_guard lock(registry().mutex);
        ids.assign(registry().nodes.begin(), registry().nodes.end());
    }
    const double now = g.time();
    bool touched = false;
    for (NodeId id : ids)
    {
        Node* n = g.node(id);
        if (!n || n->type().name != "groomStrands")
            continue;
        auto* node = static_cast<GroomStrandsNode*>(n);
        if (after && node->referenceToStore() > 0.0)
        {
            g.setValue({id, node->type().attrIndex("scaleReference"), -1}, node->referenceToStore());
            touched = true;
        }
        const bool missedFeed = node->missedFeed();
        if (after && !node->missedHistory() && !missedFeed)
            continue;
        const bool sampled = node->sampleHistory(g, now);
        if (after && missedFeed)
            node->restartSimulation();
        const bool fed = (!after || missedFeed) && node->feedSimulation(g, now);
        if (fed || sampled || (after && missedFeed))
        {
            g.touch(id);
            touched = true;
        }
    }
    if (after && touched)
        g.evaluate();
}
} // namespace

void registerGroomLabType()
{
    NodeType t;
    t.name = "groomStrands";
    t.icon = "feather";
    t.timeDependent = true;
    t.doc = "Strands grown on the scalp by a groom graph (scatter, interpolate, clump, curl, noise and the rest), guided by "
            "the incoming curves.";
    const std::string c = "Strands";
    t.ownAttrs = {
        {.name = "inCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Guides, world space."},
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The scalp."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The scalp's world matrix."},
        sAttrDefault("graph", "", "The groom graph as JSON; empty grows the default hair.", c),
        bAttr("useSceneTime", true, "Wind, Jiggle and Simulate run on the scene's time; off, on Time.", c),
        fAttr("time", 0.0, "Seconds the graph is evaluated at while Use Scene Time is off.", c),
        bAttr("scaleWithScalp", true, "Lengths, widths and densities scale with the scalp's area against Scale Reference.", c),
        fAttr("scaleReference", 0.0, "The scalp area the graph's sizes are 1x at; 0 takes the area on the next evaluation.", c),
        dataOut("outCurve", "The strands, world space, with a width per point."),
        {.name = "strandCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Strands grown."},
        stringOut("warning", "Problems with the last evaluation."),
    };
    t.create = [] { return std::make_unique<GroomStrandsNode>(); };
    types::registerType(std::move(t));
    Graph::addEvaluateHook(sampleHistories);
}
} // namespace lila::groom
