#include "uv/LiveSession.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/Uv.h"

#include <algorithm>
#include <map>
#include <mutex>

namespace lila::uv::live
{
namespace
{
using Clock = std::chrono::steady_clock;
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::NodeId;

std::map<NodeId, std::unique_ptr<Session>>& sessions()
{
    static std::map<NodeId, std::unique_ptr<Session>> s;
    return s;
}

struct Staged
{
    NodeId node;
    MeshPtr mesh;
};

std::map<uint64_t, Staged>& staged()
{
    static std::map<uint64_t, Staged> s;
    return s;
}

/** Scene edits that must not be recorded: the preview of an open transaction. */
struct NoUndo
{
    bool was = UndoStack::get().enabled();
    NoUndo() { UndoStack::get().setEnabled(false); }
    ~NoUndo() { UndoStack::get().setEnabled(was); }
};

/** Same uvs, pins and stiffness. */
bool sameUvState(const geo::Mesh& a, const geo::Mesh& b, std::string_view set)
{
    auto eq = [](auto x, auto y) { return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin()); };
    return eq(a.cornerAttrs().get<glm::vec2>(set), b.cornerAttrs().get<glm::vec2>(set)) &&
           eq(a.cornerAttrs().get<bool>("uvPin"), b.cornerAttrs().get<bool>("uvPin")) &&
           eq(a.cornerAttrs().get<float>("uvStiffness"), b.cornerAttrs().get<float>("uvStiffness"));
}

MeshPtr outMesh(NodeId id)
{
    Graph& g = Graph::current();
    scene::Node* n = g.node(id);
    if (!n || !n->type().isA("mesh"))
        return nullptr;
    g.evaluate();
    return n->as<MeshPtr>(MeshNode::kOutMesh);
}
} // namespace

LiveOptions& options()
{
    static LiveOptions o;
    return o;
}

void optionsChanged()
{
    LiveOptions o = options();
    for (auto& [id, s] : sessions())
        s->solver().edit([o](std::vector<LiveIsland>& is) {
            for (LiveIsland& i : is)
                i.setOptions(o);
        });
}

Session::Session(NodeId node, std::string uvSet) : mNode(node), mSet(std::move(uvSet)), mSolver(std::make_unique<LiveSolver>())
{
    reload();
}

Session::~Session() = default;

bool Session::alive() const
{
    scene::Node* n = Graph::current().node(mNode);
    return n && n->type().isA("mesh");
}

bool Session::current() const
{
    return mStamp && outMesh(mNode) == mStamp;
}

void Session::reload()
{
    LILA_ZONE();

    if (mOpen)
        commit();
    mBase = outMesh(mNode);
    mStamp = mBase;
    std::vector<LiveIsland> islands;
    if (mBase)
        islands = liveIslands(*mBase, {}, mSet, options());
    mTopology.clear();
    mTopology.reserve(islands.size());
    for (const LiveIsland& is : islands)
        mTopology.push_back(is.chart());
    mCornerWedge.assign(mBase ? mBase->cornerCapacity() : 0, {-1, kInvalid});
    mFaceIsland.assign(mBase ? mBase->faceCapacity() : 0, -1);
    for (size_t i = 0; i < mTopology.size(); ++i)
    {
        const Chart& ch = mTopology[i];
        for (size_t j = 0; j < ch.corners.size(); ++j)
            mCornerWedge[ch.corners[j]] = {int(i), ch.cornerVertex[j]};
        for (Index f : ch.faces)
            mFaceIsland[f] = int(i);
    }
    mSolver->load(std::move(islands));
    mSolver->setPaused(false);
    ++mEpoch;
}

std::pair<int, Index> Session::wedge(Index corner) const
{
    return corner < mCornerWedge.size() ? mCornerWedge[corner] : std::pair<int, Index>{-1, kInvalid};
}

int Session::islandOfFace(Index face) const
{
    return face < mFaceIsland.size() ? mFaceIsland[face] : -1;
}

std::vector<int> Session::islandsOfFaces(std::span<const Index> faces) const
{
    std::vector<int> out;
    if (faces.empty())
    {
        for (size_t i = 0; i < mTopology.size(); ++i)
            out.push_back(int(i));
        return out;
    }
    for (Index f : faces)
        if (int i = islandOfFace(f); i >= 0)
            out.push_back(i);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Session::begin(std::string label)
{
    if (!mOpen)
    {
        if (!current())
            reload();
        Graph& g = Graph::current();
        scene::Node* n = g.node(mNode);
        if (!n)
            return;
        mOpen = true;
        mLabel = std::move(label);
        mPreviewed = false;
        mBeforeIn = n->as<MeshPtr>(MeshNode::kInMesh);
        mHasHistory = g.source({mNode, MeshNode::kInMesh, -1}).has_value();
        mReleased = Clock::now();
        mLastPreview = Clock::now();
    }
    mSolver->setPaused(false);
}

void Session::setStroking(bool on)
{
    mStroking = on;
    if (!on)
        mReleased = Clock::now();
}

void Session::wake(std::vector<int> islands, std::function<void(LiveIsland&)> fn)
{
    mSolver->edit([islands = std::move(islands), fn = std::move(fn)](std::vector<LiveIsland>& is) {
        auto one = [&](LiveIsland& i) {
            if (fn)
                fn(i);
            i.wake();
        };
        if (islands.empty())
            for (LiveIsland& i : is)
                one(i);
        else
            for (int k : islands)
                if (k >= 0 && size_t(k) < is.size())
                    one(is[size_t(k)]);
    });
}

geo::Mesh Session::meshFromFrame(const LiveSolver::Frame& f) const
{
    LILA_ZONE();

    geo::Mesh m = *mBase;
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(mSet);
    bool anyPin = !m.cornerAttrs().get<bool>("uvPin").empty(), anyStiff = !m.cornerAttrs().get<float>("uvStiffness").empty();
    for (size_t i = 0; i < f.islands.size() && i < mTopology.size(); ++i)
    {
        const LiveSolver::IslandState& s = *f.islands[i];
        const Chart& ch = mTopology[i];
        for (size_t j = 0; j < ch.corners.size(); ++j)
            uv[ch.corners[j]] = glm::vec2(s.uv[ch.cornerVertex[j]]);
        for (uint8_t p : s.pinned)
            anyPin |= p != 0;
        for (float k : s.stiffness)
            anyStiff |= k != 1.0f;
    }
    if (anyPin)
    {
        std::span<bool> pin = m.cornerAttrs().getOrAdd<bool>("uvPin");
        for (size_t i = 0; i < f.islands.size() && i < mTopology.size(); ++i)
            for (size_t j = 0; j < mTopology[i].corners.size(); ++j)
                pin[mTopology[i].corners[j]] = f.islands[i]->pinned[mTopology[i].cornerVertex[j]] != 0;
    }
    if (anyStiff)
    {
        std::span<float> st = m.cornerAttrs().getOrAdd<float>("uvStiffness");
        for (size_t i = 0; i < f.islands.size() && i < mTopology.size(); ++i)
            for (size_t j = 0; j < mTopology[i].corners.size(); ++j)
                st[mTopology[i].corners[j]] = f.islands[i]->stiffness[mTopology[i].cornerVertex[j]];
    }
    return m;
}

void Session::writePreview(const LiveSolver::Frame& f)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    auto mesh = std::make_shared<const geo::Mesh>(meshFromFrame(f));
    {
        NoUndo off;
        g.setValue({mNode, MeshNode::kInMesh, -1}, MeshPtr(mesh));
    }
    mStamp = outMesh(mNode);
    mPreviewed = true;
    mLastPreview = Clock::now();
}

bool Session::tick()
{
    std::shared_ptr<const LiveSolver::Frame> f = mSolver->frame();
    bool fresh = f->generation != mSeen;
    mSeen = f->generation;
    if (!mOpen)
        return fresh;
    if (!current())
    {
        // Something else edited the mesh under the transaction: drop it rather than overwrite that edit.
        mOpen = false;
        mStroking = false;
        mSolver->setPaused(true);
        reload();
        return true;
    }
    if (f->epoch == mEpoch && fresh && !mHasHistory && Clock::now() - mLastPreview > std::chrono::milliseconds(60))
        writePreview(*f);
    if (!mStroking && mSolver->idle())
        commit();
    else if (!mStroking && Clock::now() - mReleased > std::chrono::seconds(4))
    {
        mSolver->setPaused(true);
        commit();
    }
    return fresh;
}

void Session::commit()
{
    LILA_ZONE();

    if (!mOpen)
        return;
    mOpen = false;
    mStroking = false;
    Graph& g = Graph::current();
    scene::Node* n = g.node(mNode);
    if (!n)
        return;
    mSolver->sync({});
    std::shared_ptr<const LiveSolver::Frame> f = mSolver->frame();
    if (!f->idle)
        mSolver->setPaused(true);
    if (mPreviewed)
    {
        NoUndo off;
        g.setValue({mNode, MeshNode::kInMesh, -1}, mBeforeIn);
    }
    if (f->epoch != mEpoch || !mBase)
    {
        mStamp = outMesh(mNode);
        return;
    }
    auto mesh = std::make_shared<const geo::Mesh>(meshFromFrame(*f));
    if (sameUvState(*mesh, *mBase, mSet))
    {
        mStamp = outMesh(mNode);
        return;
    }
    uint64_t token = stage(mNode, mesh);
    try
    {
        UndoChunk chunk(mLabel.empty() ? "UV Live" : mLabel);
        ops::execute("uvLiveCommit", {{"live", int64_t(token)}}, CallSource::Script);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    mBase = outMesh(mNode);
    mStamp = mBase;
}

void Session::finish(int ms)
{
    setStroking(false);
    mSolver->waitIdle(ms);
    tick();
    commit();
}

Session* get(NodeId node, bool create)
{
    auto& all = sessions();
    auto it = all.find(node);
    if (it != all.end())
    {
        Session& s = *it->second;
        if (!s.alive() || s.uvSet() != currentUvSet())
        {
            if (s.alive())
                s.commit();
            all.erase(it);
        }
        else
        {
            if (!s.open() && !s.current())
                s.reload();
            return &s;
        }
    }
    if (!create || !outMesh(node))
        return nullptr;
    auto s = std::make_unique<Session>(node, currentUvSet());
    Session* raw = s.get();
    all[node] = std::move(s);
    return raw;
}

Session* find(NodeId node)
{
    auto& all = sessions();
    auto it = all.find(node);
    if (it == all.end() || !it->second->alive() || it->second->uvSet() != currentUvSet() || !it->second->current())
        return nullptr;
    return it->second.get();
}

bool tickAll()
{
    bool any = false;
    auto& all = sessions();
    for (auto it = all.begin(); it != all.end();)
    {
        if (!it->second->alive())
        {
            it = all.erase(it);
            continue;
        }
        any |= it->second->tick();
        ++it;
    }
    return any;
}

bool anyOpen()
{
    for (auto& [id, s] : sessions())
        if (s->open())
            return true;
    return false;
}

void commitAll()
{
    for (auto& [id, s] : sessions())
        if (s->open() && s->alive())
            s->commit();
}

void clear()
{
    sessions().clear();
}

uint64_t stage(NodeId node, MeshPtr mesh)
{
    static uint64_t next = 1;
    uint64_t t = next++;
    staged()[t] = {node, std::move(mesh)};
    return t;
}

MeshPtr takeStaged(uint64_t token, NodeId& node)
{
    auto it = staged().find(token);
    if (it == staged().end())
        return nullptr;
    node = it->second.node;
    MeshPtr m = std::move(it->second.mesh);
    staged().erase(it);
    return m;
}

void installAutoCommit()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    ops::addExecuteListener([](const ExecuteEvent& e) {
        static bool busy = false;
        if (e.done || busy || e.name == "uvLiveCommit")
            return;
        const CommandDef* def = ops::find(e.name);
        if (!def || (!def->undoable && e.name != "undo" && e.name != "redo"))
            return;
        if (!anyOpen())
            return;
        busy = true;
        commitAll();
        busy = false;
    });
}
} // namespace lila::uv::live
