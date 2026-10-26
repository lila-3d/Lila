#include "stream/ChangeStream.h"

#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "stream/Internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace lila::stream
{
namespace
{
using detail::Entry;
using detail::Message;
using detail::MessagePtr;
using detail::NodeInfo;
using Kind = Entry::Kind;
using scene::Graph;
using scene::NodeId;
using Clock = std::chrono::steady_clock;

/** Data values whose JSON is longer than this go as a summary unless the filter asks for full values. */
constexpr size_t kInlineDataBytes = 4096;

/** Live values for one plug, and frame messages, go out at most this often. */
constexpr auto kLiveInterval = std::chrono::milliseconds(33);

std::atomic<uint64_t> gSeq{0};
std::atomic<bool> gServerActive{false};
std::atomic<bool> gServerLive{false};
std::atomic<bool> gServerWorld{false};
std::atomic<bool> gServerFiltersChanged{false};

/** The server's client filters, set from its threads and read on the main thread. */
struct ServerFilters
{
    std::mutex m;
    std::vector<Filter> filters;
};

ServerFilters& serverFilters()
{
    static ServerFilters s;
    return s;
}

// Hashing

struct Fnv
{
    uint64_t h = 14695981039346656037ull;
    void add(const void* data, size_t n)
    {
        auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
    }
};

/** A 64-bit hash taken eight bytes at a time, for meshes, which are too big for Fnv's byte loop. */
struct WordHash
{
    uint64_t h = 0x9e3779b97f4a7c15ull;
    void word(uint64_t w)
    {
        h ^= w * 0xbf58476d1ce4e5b9ull;
        h = ((h << 27) | (h >> 37)) * 0x94d049bb133111ebull;
    }
    void add(const void* data, size_t n)
    {
        auto* p = static_cast<const unsigned char*>(data);
        size_t i = 0;
        for (; i + 8 <= n; i += 8)
        {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            word(w);
        }
        if (i < n)
        {
            uint64_t w = 0;
            std::memcpy(&w, p + i, n - i);
            word(w ^ (uint64_t(n - i) << 56));
        }
    }
    template <class T> void value(const T& v) { add(&v, sizeof(T)); }
    void text(std::string_view s)
    {
        value(s.size());
        add(s.data(), s.size());
    }
    uint64_t done() const
    {
        uint64_t x = h;
        x ^= x >> 31;
        x *= 0x7fb5d329728ea185ull;
        return x ^ (x >> 27);
    }
};

std::string hex(uint64_t h)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

bool elementValid(const geo::Mesh& m, geo::Domain d, geo::Index i)
{
    switch (d)
    {
    case geo::Domain::Point: return m.pointValid(i);
    case geo::Domain::Edge: return m.edgeValid(i);
    case geo::Domain::Face: return m.faceValid(i);
    default: return m.cornerValid(i);
    }
}

/** Everything the mesh holds (points, faces, attribute layers, material names), so any edit to it changes the hash. */
uint64_t meshHash(const geo::Mesh& m)
{
    LILA_ZONE();

    WordHash h;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            h.value(p);
            h.value(m.position(p));
        }
    for (geo::Index face = 0; face < m.faceCapacity(); ++face)
    {
        if (!m.faceValid(face))
            continue;
        m.forEachFaceCorner(face, [&](geo::Index c) { h.value(m.cornerPoint(c)); });
        h.value(geo::kInvalid);
    }
    for (size_t d = 0; d < size_t(geo::Domain::Count); ++d)
    {
        const geo::AttributeSet& set = m.attributes(geo::Domain(d));
        for (size_t l = 0; l < set.layerCount(); ++l)
        {
            const geo::Attribute& a = set.layer(l);
            h.text(a.name());
            h.value(a.type());
            for (size_t i = 0; i < a.size(); ++i)
                if (elementValid(m, geo::Domain(d), geo::Index(i)))
                    h.add(a.raw(i), a.stride());
        }
    }
    for (const std::string& name : m.materialNames())
        h.text(name);
    return h.done();
}

json meshSummary(const geo::Mesh& m, uint64_t hash)
{
    return {{"summary", "mesh"}, {"points", m.numPoints()}, {"edges", m.numEdges()}, {"faces", m.numFaces()}, {"hash", hex(hash)}};
}

/** Mesh hashes worked out once per pass over a message, by the mesh's address. */
using MeshHashes = std::unordered_map<const void*, uint64_t>;

uint64_t cachedMeshHash(const scene::MeshPtr& mesh, MeshHashes& cache)
{
    auto [it, fresh] = cache.try_emplace(mesh.get(), 0);
    if (fresh)
        it->second = meshHash(*mesh);
    return it->second;
}

/** Equal for equal values, compared by content: a mesh edited in place gets a new one. */
uint64_t fingerprint(const scene::PlugValue& v, MeshHashes& meshes)
{
    WordHash h;
    h.value(v.index());
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>)
                h.text(x);
            else if constexpr (std::is_same_v<T, scene::MeshPtr>)
                h.value(x ? cachedMeshHash(x, meshes) : 0);
            else if constexpr (std::is_same_v<T, scene::DataPtr>)
                h.text(x ? x->toJson().dump(-1, ' ', false, json::error_handler_t::replace) : std::string());
            else
                h.value(x);
        },
        v);
    return h.done();
}

/** The message's own summaries, worked out as it went out; a mesh missing from them is summarized now. */
json valueJson(const scene::PlugValue& v, bool full, const Message& msg)
{
    if (const scene::MeshPtr* mesh = std::get_if<scene::MeshPtr>(&v); mesh && *mesh && !full)
    {
        if (auto it = msg.summaries.find(mesh->get()); it != msg.summaries.end())
            return it->second;
        return meshSummary(**mesh, meshHash(**mesh));
    }
    if (const scene::DataPtr* data = std::get_if<scene::DataPtr>(&v); data && *data && !full)
    {
        json j = (*data)->toJson();
        const std::string text = j.dump(-1, ' ', false, json::error_handler_t::replace);
        if (text.size() <= kInlineDataBytes)
            return j;
        Fnv f;
        f.add(text.data(), text.size());
        return {{"summary", "data"}, {"kind", (*data)->kind()}, {"bytes", text.size()}, {"hash", hex(f.h)}};
    }
    return scene::toJson(v);
}

/** Summarizes every mesh the message holds, here on the main thread, before it goes to other threads. */
void summarize(Message& m, MeshHashes& hashes)
{
    LILA_ZONE();

    auto one = [&](const scene::PlugValue& v) {
        const scene::MeshPtr* mesh = std::get_if<scene::MeshPtr>(&v);
        if (mesh && *mesh && !m.summaries.contains(mesh->get()))
            m.summaries.emplace(mesh->get(), meshSummary(**mesh, cachedMeshHash(*mesh, hashes)));
    };
    for (const Entry& e : m.entries)
    {
        one(e.value);
        for (const auto& [name, v] : e.attrs)
            one(v);
    }
    for (const Message::Node& n : m.nodes)
    {
        for (const auto& [name, v] : n.attrs)
            one(v);
        for (const auto& [name, v] : n.outputs)
            one(v);
    }
}

// Filters

std::string normalPath(std::string p)
{
    if (p.empty())
        return p;
    if (p.front() != '|')
        p.insert(p.begin(), '|');
    while (p.size() > 1 && p.back() == '|')
        p.pop_back();
    return p;
}

bool isUnder(const Filter& f, std::string_view path)
{
    if (f.under.empty())
        return true;
    for (const std::string& u : f.under)
        if (path == u || (path.size() > u.size() && path.starts_with(u) && path[u.size()] == '|'))
            return true;
    return false;
}

bool typeOk(const Filter& f, const scene::NodeType* t)
{
    if (f.types.empty())
        return true;
    if (!t)
        return false;
    return std::any_of(f.types.begin(), f.types.end(), [t](const std::string& name) { return t->isA(name); });
}

bool attrOk(const Filter& f, std::string_view name)
{
    return f.attrs.empty() || std::find(f.attrs.begin(), f.attrs.end(), name) != f.attrs.end();
}

bool nodeOk(const Filter& f, const NodeInfo& n) { return typeOk(f, n.type) && isUnder(f, n.path); }

std::string_view plugNode(std::string_view plug)
{
    const size_t dot = plug.rfind('.');
    return dot == std::string_view::npos ? plug : plug.substr(0, dot);
}

json attrsJson(const detail::Attrs& attrs, const Filter& f, const Message& msg)
{
    json out = json::object();
    for (const auto& [name, v] : attrs)
        if (attrOk(f, name))
            out[name] = valueJson(v, f.full, msg);
    return out;
}

json nodeJson(const char* kind, NodeId id, const NodeInfo& n)
{
    return {{"kind", kind}, {"id", id}, {"path", n.path}, {"type", n.type ? n.type->name : std::string()}};
}

json changeJson(const Entry& e, const Filter& f, const Message& msg)
{
    switch (e.kind)
    {
    case Kind::Created:
    {
        if (!nodeOk(f, e.node))
            return nullptr;
        json j = nodeJson("created", e.id, e.node);
        j["attrs"] = attrsJson(e.attrs, f, msg);
        return j;
    }
    case Kind::Deleted:
        return nodeOk(f, e.node) ? nodeJson("deleted", e.id, e.node) : json(nullptr);
    case Kind::Renamed:
    case Kind::Reparented:
    {
        if (!e.live || !typeOk(f, e.node.type) || !(isUnder(f, e.node.path) || isUnder(f, e.oldPath)))
            return nullptr;
        const bool renamed = e.kind == Kind::Renamed;
        json j = nodeJson(renamed ? "renamed" : "reparented", e.id, e.node);
        j["oldPath"] = e.oldPath;
        if (!renamed)
            j["parent"] = e.text;
        return j;
    }
    case Kind::Value:
    {
        if (!e.live || !nodeOk(f, e.node) || !attrOk(f, e.attrName))
            return nullptr;
        json j = nodeJson("value", e.id, e.node);
        j["attr"] = e.attrName;
        j["value"] = valueJson(e.value, f.full, msg);
        if (e.locked)
            j["locked"] = true;
        return j;
    }
    case Kind::Connection:
    {
        if (!e.live || !nodeOk(f, e.node) || !attrOk(f, e.attrName))
            return nullptr;
        json j = nodeJson("connection", e.id, e.node);
        j["attr"] = e.attrName;
        j["source"] = e.text.empty() ? json(nullptr) : json(e.text);
        return j;
    }
    case Kind::Selection:
    {
        json paths = json::array();
        for (const std::string& p : e.paths)
            if (isUnder(f, p))
                paths.push_back(p);
        return {{"kind", "selection"}, {"paths", std::move(paths)}};
    }
    case Kind::Time:
        return {{"kind", "time"}, {"time", e.time}};
    case Kind::World:
    {
        if (!f.world || !nodeOk(f, e.node))
            return nullptr;
        json j = nodeJson("world", e.id, e.node);
        j["matrix"] = scene::toJson(e.value);
        return j;
    }
    case Kind::Evaluated:
    {
        if (!f.evaluated || !nodeOk(f, e.node) || !attrOk(f, e.attrName))
            return nullptr;
        json j = nodeJson("evaluated", e.id, e.node);
        j["attr"] = e.attrName;
        j["value"] = valueJson(e.value, f.full, msg);
        return j;
    }
    }
    return nullptr;
}

/** The message's changes this filter lets through, leaving out those meant for other filters. */
json changesJson(const Message& m, const Filter& f)
{
    json changes = json::array();
    std::optional<std::string> key;
    for (const Entry& e : m.entries)
    {
        if (e.only >= 0)
        {
            if (!key)
                key = f.toJson().dump();
            if (m.keys[size_t(e.only)] != *key)
                continue;
        }
        if (json c = changeJson(e, f, m); !c.is_null())
            changes.push_back(std::move(c));
    }
    return changes;
}

// Recording

detail::Attrs storedAttrs(const Graph& g, const scene::Node& n)
{
    detail::Attrs out;
    const auto& defs = n.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
    {
        const scene::AttrDef& a = defs[size_t(i)];
        if (!(a.flags & scene::AttrStorable) || !(a.flags & scene::AttrInput) || g.source({n.id(), i, -1}))
            continue;
        out.emplace_back(a.name, n.value(i));
    }
    return out;
}

NodeInfo infoOf(const Graph& g, NodeId id)
{
    const scene::Node* n = g.node(id);
    return n ? NodeInfo{g.fullPath(id), &n->type()} : NodeInfo{};
}

int resyncRank(std::string_view reason)
{
    return reason == "open" ? 3 : reason == "new" ? 2 : reason.empty() ? 0 : 1;
}

/** The changes gathered for the group being built: one command, or one undo step made outside commands. */
struct Pending
{
    std::string command;
    std::string source;
    bool ok = true;
    std::string undo;
    std::string redo;
    std::vector<Entry> entries;
    std::map<std::tuple<Kind, NodeId, int, int, int>, size_t> plugs; // coalesces repeated changes to one plug
    long selection = -1;
    long time = -1;
    std::string resync;
    std::string resyncPath;
    std::vector<Message> after;
    std::vector<NodeId> moved; // transforms whose world matrix may have changed, each once
    std::unordered_set<NodeId> movedSet;
    bool started = false;
    uint64_t step = 0; // the undo stack's newest step when gathering began
    uint64_t seq = 0;  // reserved by a snapshot taken while the group was open; 0 takes the next when sent

    bool empty() const { return entries.empty() && resync.empty() && after.empty() && undo.empty() && redo.empty(); }
};

struct Subscriber
{
    Handler fn;
    Filter filter;
    std::string owner;
};

/** What one distinct filter that wants evaluated values or world matrices was last sent, to diff against. */
struct Tracker
{
    Filter filter;
    /** Nodes last computed in this evaluation pass or an earlier one are accounted for. */
    uint64_t pass = 0;
    /** Fingerprints of the values last sent, by node and attr; attr -1 is the world matrix. */
    std::map<std::pair<NodeId, int>, uint64_t> sent;
};

/** The attrs evaluated changes report for one node type. */
struct EvaluatedAttrs
{
    std::vector<int> outputs; // every output but worldMatrix
    std::vector<int> inputs;  // reported while a connection drives them
};

struct Recorder
{
    bool on = false;
    int graphListener = 0;
    int executeListener = 0;
    bool evaluateHooked = false;
    std::vector<events::SubscriptionId> eventSubs;
    std::unordered_map<NodeId, NodeInfo> cache; // full paths as last seen, for deletes and old paths
    bool inCommand = false;
    bool flushPosted = false;
    Pending p;
    /** Groups a snapshot closed while their command or edit was still open, oldest first; they go out ahead of p. */
    std::vector<Pending> sealed;
    // Live values of the interactive edit in progress
    std::set<std::pair<NodeId, int>> liveDirty;
    std::map<std::pair<NodeId, int>, Clock::time_point> liveSent;
    bool livePosted = false;
    std::unordered_map<const scene::NodeType*, int> worldAttr; // worldMatrix's index per type, -1 for none
    std::unordered_map<const scene::NodeType*, EvaluatedAttrs> evaluatedAttrs;
    std::map<uint64_t, Subscriber> subscribers;
    uint64_t nextSubscriber = 1;
    // Evaluated values and world matrices diffed per filter, and the frame messages that carry what time changed
    std::map<std::string, Tracker> trackers; // by Filter::toJson dumped
    bool evaluating = false; // the recorder's own evaluate: it diffs right after, so the hook leaves it alone
    bool framePosted = false;
    Clock::time_point frameSent;
};

Recorder& rec()
{
    static Recorder r;
    return r;
}

/** Posts work to the main queue at a given time, from a thread of its own. One piece of work waits at a time. */
class Timer
{
public:
    ~Timer()
    {
        {
            std::scoped_lock lock(mMutex);
            mQuit = true;
        }
        mCv.notify_all();
        if (mThread.joinable())
            mThread.join();
    }

    void postAt(Clock::time_point at, std::function<void()> fn)
    {
        {
            std::scoped_lock lock(mMutex);
            mAt = at;
            mFn = std::move(fn);
            if (!mThread.joinable())
                mThread = std::thread([this] { loop(); });
        }
        mCv.notify_all();
    }

private:
    void loop()
    {
        LILA_THREAD_NAME("Change stream timer");
        std::unique_lock lock(mMutex);
        while (!mQuit)
        {
            if (!mFn)
            {
                mCv.wait(lock, [this] { return mQuit || mFn; });
                continue;
            }
            if (mCv.wait_until(lock, mAt, [this] { return mQuit; }))
                break;
            std::function<void()> fn = std::exchange(mFn, nullptr);
            lock.unlock();
            mainq::post(std::move(fn));
            lock.lock();
        }
    }

    std::mutex mMutex;
    std::condition_variable mCv;
    std::thread mThread;
    Clock::time_point mAt;
    std::function<void()> mFn;
    bool mQuit = false;
};

Timer& frameTimer()
{
    static Timer t;
    return t;
}

void flushNow();

bool wantsLive()
{
    if (gServerLive.load())
        return true;
    for (const auto& [id, s] : rec().subscribers)
        if (s.filter.live)
            return true;
    return false;
}

bool wantsWorld()
{
    if (gServerWorld.load())
        return true;
    for (const auto& [id, s] : rec().subscribers)
        if (s.filter.world)
            return true;
    return false;
}

/** The node's worldMatrix attr, or -1 when it has none (not a transform). */
int worldAttrOf(const scene::Node& n)
{
    auto [it, fresh] = rec().worldAttr.try_emplace(&n.type(), -1);
    if (fresh)
        it->second = n.type().attrIndex("worldMatrix");
    return it->second;
}

const EvaluatedAttrs& evaluatedAttrsOf(const scene::Node& n)
{
    auto [it, fresh] = rec().evaluatedAttrs.try_emplace(&n.type());
    if (fresh)
    {
        const int world = worldAttrOf(n);
        const auto& defs = n.type().attrs;
        for (int i = 0; i < int(defs.size()); ++i)
            if (defs[size_t(i)].flags & scene::AttrOutput)
            {
                if (i != world)
                    it->second.outputs.push_back(i);
            }
            else if (defs[size_t(i)].flags & scene::AttrInput)
                it->second.inputs.push_back(i);
    }
    return it->second;
}

bool driven(const Graph& g, NodeId id, int attr, const scene::AttrDef& a)
{
    if (g.source({id, attr, -1}))
        return true;
    if (a.type == scene::PlugType::Vec3)
        for (int c = 0; c < 3; ++c)
            if (g.source({id, attr, c}))
                return true;
    return false;
}

/** Calls fn(attr index) for each attr of the node evaluated changes report. */
template <class F> void forEachEvaluated(const Graph& g, const scene::Node& n, F&& fn)
{
    const EvaluatedAttrs& ea = evaluatedAttrsOf(n);
    for (int i : ea.outputs)
        fn(i);
    for (int i : ea.inputs)
        if (driven(g, n.id(), i, n.type().attrs[size_t(i)]))
            fn(i);
}

/** Starts and ends trackers to match the filters of the subscribers and server clients. */
void syncTrackers()
{
    LILA_ZONE();

    Recorder& r = rec();
    gServerFiltersChanged = false;
    std::map<std::string, Filter> want;
    auto consider = [&want](const Filter& f) {
        if (f.evaluated || f.world)
            want.try_emplace(f.toJson().dump(), f);
    };
    if (r.on)
    {
        for (const auto& [id, s] : r.subscribers)
            consider(s.filter);
        ServerFilters& sf = serverFilters();
        std::scoped_lock lock(sf.m);
        for (const Filter& f : sf.filters)
            consider(f);
    }
    std::erase_if(r.trackers, [&want](const auto& t) { return !want.contains(t.first); });
    if (want.empty())
        return;
    const uint64_t pass = Graph::current().evaluationPass();
    for (auto& [key, f] : want)
        r.trackers.try_emplace(key, Tracker{.filter = std::move(f), .pass = pass});
}

void syncIfChanged()
{
    if (gServerFiltersChanged.load())
        syncTrackers();
}

/** Evaluates for the recorder's own reads, which diff what changed themselves. */
void evaluateQuietly(Graph& g)
{
    Recorder& r = rec();
    const bool outer = std::exchange(r.evaluating, true);
    try
    {
        g.evaluate();
    }
    catch (...)
    {
        r.evaluating = outer;
        throw;
    }
    r.evaluating = outer;
}

void markMoved(Pending& p, const Graph& g, NodeId id)
{
    const scene::Node* n = g.node(id);
    if (n && worldAttrOf(*n) >= 0 && p.movedSet.insert(id).second)
        p.moved.push_back(id);
}

/** World entries, evaluated now, for each moved transform and every transform under it, parents first. */
void appendWorld(std::vector<Entry>& out, const std::vector<NodeId>& moved)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::unordered_set<NodeId> seen;
    auto one = [&](NodeId id) {
        const scene::Node* n = g.node(id);
        if (!n || !seen.insert(id).second)
            return;
        const int attr = worldAttrOf(*n);
        if (attr < 0)
            return;
        out.push_back({.kind = Kind::World, .id = id, .attr = attr, .node = infoOf(g, id), .value = g.value({id, attr, -1})});
    };
    for (NodeId id : moved)
    {
        if (seen.contains(id) || !g.node(id))
            continue;
        one(id);
        for (NodeId d : g.descendants(id))
            one(d);
    }
}

/** Notes the world entries every world filter is sent with this message, so diffs don't send them again. */
void recordWorld(const Message& m, bool liveOnly, MeshHashes& hashes)
{
    for (auto& [key, t] : rec().trackers)
    {
        if (!t.filter.world || (liveOnly && !t.filter.live))
            continue;
        for (const Entry& e : m.entries)
            if (e.kind == Kind::World && e.only < 0)
                t.sent[{e.id, -1}] = fingerprint(e.value, hashes);
    }
}

/**
 * Adds, for each tracker, the evaluated values and world matrices that changed since it last looked, as entries only
 * its filter sees. Evaluates first. liveOnly looks only for filters that take live messages.
 */
void appendTracked(Message& m, bool liveOnly, MeshHashes& hashes)
{
    LILA_ZONE();

    Recorder& r = rec();
    syncIfChanged();
    if (r.trackers.empty())
        return;
    Graph& g = Graph::current();
    evaluateQuietly(g);
    const uint64_t pass = g.evaluationPass();
    uint64_t oldest = pass;
    for (const auto& [key, t] : r.trackers)
        if (!liveOnly || t.filter.live)
            oldest = std::min(oldest, t.pass);
    if (oldest >= pass)
        return;

    std::vector<NodeId> computed;
    for (NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n && n->evaluatedPass() > oldest)
            computed.push_back(id);
    std::sort(computed.begin(), computed.end());
    std::unordered_map<NodeId, NodeInfo> infos;
    std::map<std::pair<NodeId, int>, uint64_t> prints;
    auto printOf = [&](NodeId id, int attr, const scene::PlugValue& v) {
        auto [it, fresh] = prints.try_emplace({id, attr}, 0);
        if (fresh)
            it->second = fingerprint(v, hashes);
        return it->second;
    };

    for (auto& [key, t] : r.trackers)
    {
        if (liveOnly && !t.filter.live)
            continue;
        int keyIndex = -1;
        // Sends the value when it differs from the last one this tracker sent.
        auto report = [&](Kind kind, NodeId id, int attr, const NodeInfo& info, const scene::PlugValue& v) {
            const uint64_t print = printOf(id, kind == Kind::World ? -1 : attr, v);
            auto [it, fresh] = t.sent.try_emplace({id, kind == Kind::World ? -1 : attr}, print);
            if (!fresh && it->second == print)
                return;
            it->second = print;
            if (keyIndex < 0)
            {
                keyIndex = int(m.keys.size());
                m.keys.push_back(key);
            }
            const scene::Node* n = g.node(id);
            m.entries.push_back({.kind = kind,
                                 .id = id,
                                 .attr = attr,
                                 .only = keyIndex,
                                 .node = info,
                                 .attrName = kind == Kind::World ? std::string() : n->type().attrs[size_t(attr)].name,
                                 .value = v});
        };
        for (NodeId id : computed)
        {
            const scene::Node* n = g.node(id);
            if (n->evaluatedPass() <= t.pass)
                continue;
            auto info = infos.find(id);
            if (info == infos.end())
                info = infos.emplace(id, infoOf(g, id)).first;
            if (!nodeOk(t.filter, info->second))
                continue;
            if (const int world = worldAttrOf(*n); t.filter.world && world >= 0)
                report(Kind::World, id, world, info->second, n->value(world));
            if (t.filter.evaluated)
                forEachEvaluated(g, *n, [&](int attr) {
                    if (attrOk(t.filter, n->type().attrs[size_t(attr)].name))
                        report(Kind::Evaluated, id, attr, info->second, n->value(attr));
                });
        }
        t.pass = pass;
    }
}

void fill(Message& m, std::vector<Entry>& entries);
void dispatch(Message m, bool numbered = true);

/** Sends what the open interactive edit has changed since its last live message, per plug no faster than kLiveInterval. */
void flushLive()
{
    LILA_ZONE();

    Recorder& r = rec();
    r.livePosted = false;
    if (!r.on || r.inCommand || !UndoStack::get().chunkOpen())
    {
        r.liveDirty.clear();
        return;
    }
    const auto now = Clock::now();
    const bool world = wantsWorld();
    const Graph& g = Graph::current();
    std::vector<Entry> entries;
    std::vector<NodeId> moved;
    for (auto it = r.liveDirty.begin(); it != r.liveDirty.end();)
    {
        auto& last = r.liveSent[*it];
        if (now - last < kLiveInterval)
        {
            ++it;
            continue;
        }
        last = now;
        entries.push_back({.kind = Kind::Value, .id = it->first, .attr = it->second});
        const scene::Node* n = g.node(it->first);
        if (world && n && worldAttrOf(*n) >= 0 && std::find(moved.begin(), moved.end(), it->first) == moved.end())
            moved.push_back(it->first);
        it = r.liveDirty.erase(it);
    }
    if (entries.empty())
        return;
    Message m;
    m.type = Message::Type::Live;
    m.source = "interactive";
    fill(m, entries);
    appendWorld(m.entries, moved);
    MeshHashes hashes;
    recordWorld(m, true, hashes);
    appendTracked(m, true, hashes);
    dispatch(std::move(m), false);
}

void scheduleLive()
{
    Recorder& r = rec();
    if (r.livePosted)
        return;
    r.livePosted = true;
    mainq::post(flushLive);
}

/** Sends the evaluated values and world matrices that changed outside any group: time moving, a simulation stepping. */
void flushFrame()
{
    LILA_ZONE();

    Recorder& r = rec();
    r.framePosted = false;
    // An open command or edit sends what it changed with its group.
    if (!r.on || r.inCommand || UndoStack::get().chunkOpen())
        return;
    r.frameSent = Clock::now();
    Message m;
    m.type = Message::Type::Frame;
    m.time = Graph::current().time();
    MeshHashes hashes;
    appendTracked(m, false, hashes);
    if (!m.entries.empty())
        dispatch(std::move(m), false);
}

/** A frame message soon: now when the last went out at least kLiveInterval ago, else once that much has passed. */
void scheduleFrame()
{
    Recorder& r = rec();
    syncIfChanged();
    if (r.framePosted || r.trackers.empty())
        return;
    r.framePosted = true;
    const Clock::time_point due = r.frameSent + kLiveInterval;
    if (Clock::now() >= due)
        mainq::post(flushFrame);
    else
        frameTimer().postAt(due, flushFrame);
}

void scheduleFlush()
{
    Recorder& r = rec();
    if (r.flushPosted)
        return;
    r.flushPosted = true;
    mainq::post([] {
        Recorder& r = rec();
        r.flushPosted = false;
        // A drag or tool still has its undo chunk open: its step's end sends it.
        if (!r.on || r.inCommand || UndoStack::get().chunkOpen())
            return;
        flushNow();
    });
}

void touched()
{
    Recorder& r = rec();
    if (r.inCommand)
        return;
    if (!r.p.started)
    {
        r.p.started = true;
        r.p.step = UndoStack::get().newestId();
    }
    scheduleFlush();
}

void add(Entry e)
{
    Pending& p = rec().p;
    if (!p.resync.empty())
        return;
    p.entries.push_back(std::move(e));
}

void setResync(std::string reason, std::string path = {})
{
    Pending& p = rec().p;
    if (resyncRank(reason) < resyncRank(p.resync))
        return;
    p.resync = std::move(reason);
    if (!path.empty())
        p.resyncPath = std::move(path);
    p.entries.clear();
    p.plugs.clear();
    p.selection = p.time = -1;
}

void rebuildCache()
{
    LILA_ZONE();

    Graph& g = Graph::current();
    Recorder& r = rec();
    r.cache.clear();
    for (NodeId id : g.nodes())
        r.cache[id] = infoOf(g, id);
}

void forgetNode(NodeId id)
{
    for (auto& [key, t] : rec().trackers)
        for (auto it = t.sent.lower_bound({id, INT_MIN}); it != t.sent.end() && it->first.first == id;)
            it = t.sent.erase(it);
}

void onChange(scene::Change c, NodeId id)
{
    Recorder& r = rec();
    Graph& g = Graph::current();
    Pending& p = r.p;
    switch (c)
    {
    case scene::Change::Created:
        r.cache[id] = infoOf(g, id);
        add({.kind = Kind::Created, .id = id, .node = r.cache[id]});
        break;
    case scene::Change::Deleted:
    {
        Entry e{.kind = Kind::Deleted, .id = id, .live = false};
        if (auto it = r.cache.find(id); it != r.cache.end())
        {
            e.node = std::move(it->second);
            r.cache.erase(it);
        }
        add(std::move(e));
        forgetNode(id);
        break;
    }
    case scene::Change::Renamed:
    case scene::Change::Reparented:
    {
        Entry e{.kind = c == scene::Change::Renamed ? Kind::Renamed : Kind::Reparented, .id = id};
        e.oldPath = r.cache[id].path;
        r.cache[id] = infoOf(g, id);
        for (NodeId d : g.descendants(id))
            r.cache[d] = infoOf(g, d);
        e.node = r.cache[id];
        add(std::move(e));
        break;
    }
    case scene::Change::Value:
    case scene::Change::Connection:
    {
        const scene::PlugRef* plug = g.changedPlug();
        const Kind kind = c == scene::Change::Value ? Kind::Value : Kind::Connection;
        const int attr = plug ? plug->attr : -1;
        // A value is reported per attr whatever component changed; a connection per plug it lands on.
        const int component = kind == Kind::Connection && plug ? plug->component : -1;
        const int element = kind == Kind::Connection && plug ? plug->element : -1;
        auto key = std::make_tuple(kind, id, attr, component, element);
        if (p.resync.empty() && !p.plugs.contains(key))
        {
            p.plugs[key] = p.entries.size();
            add({.kind = kind, .id = id, .attr = attr, .component = component, .element = element});
        }
        break;
    }
    case scene::Change::Selection:
        if (p.resync.empty() && p.selection < 0)
        {
            p.selection = long(p.entries.size());
            add({.kind = Kind::Selection});
        }
        break;
    case scene::Change::Cleared:
        r.cache.clear();
        setResync("new");
        break;
    case scene::Change::Reset:
        rebuildCache();
        setResync("reset");
        break;
    }
    const bool moves = c == scene::Change::Created || c == scene::Change::Reparented || c == scene::Change::Value ||
                       c == scene::Change::Connection;
    if (moves && p.resync.empty() && wantsWorld())
        markMoved(p, g, id);
    if (c == scene::Change::Value && !r.inCommand && UndoStack::get().chunkOpen() && wantsLive())
        if (const scene::PlugRef* plug = g.changedPlug())
        {
            r.liveDirty.insert({id, plug->attr});
            scheduleLive();
        }
    touched();
}

/** Reads the values and paths of what changed, as they stand now. */
void fill(Message& m, std::vector<Entry>& entries)
{
    Graph& g = Graph::current();
    for (Entry& e : entries)
    {
        const scene::Node* n = e.id != scene::kNoNode ? g.node(e.id) : nullptr;
        switch (e.kind)
        {
        case Kind::Created:
            if (n)
            {
                e.node = infoOf(g, e.id);
                e.attrs = storedAttrs(g, *n);
            }
            break;
        case Kind::Deleted:
            break;
        case Kind::Renamed:
        case Kind::Reparented:
            e.live = n != nullptr;
            if (!n)
                break;
            e.node = infoOf(g, e.id);
            if (e.kind == Kind::Reparented)
                if (NodeId parent = g.parent(e.id); parent != scene::kNoNode)
                    e.text = g.fullPath(parent);
            break;
        case Kind::Value:
        case Kind::Connection:
        {
            e.live = n && e.attr >= 0 && size_t(e.attr) < n->type().attrs.size();
            if (!e.live)
                break;
            e.node = infoOf(g, e.id);
            const scene::AttrDef& a = n->type().attrs[size_t(e.attr)];
            e.attrName = e.component >= 0 ? a.childNames[size_t(e.component)] : a.name;
            if (e.element >= 0)
                e.attrName += "[" + std::to_string(e.element) + "]";
            if (e.kind == Kind::Value)
            {
                e.value = n->value(e.attr);
                e.locked = g.anyLocked({e.id, e.attr, -1});
            }
            else if (std::optional<scene::PlugRef> src = g.source({e.id, e.attr, e.component, e.element}))
                e.text = g.fullPlugName(*src);
            break;
        }
        case Kind::Selection:
            for (NodeId id : g.selection())
                e.paths.push_back(g.fullPath(id));
            break;
        case Kind::Time:
            e.time = g.time();
            break;
        case Kind::World:
        case Kind::Evaluated:
            break;
        }
    }
    m.entries = std::move(entries);
}

/** numbered: takes the seq the group reserved, else the next; a live or frame message carries the newest one instead. */
void dispatch(Message m, bool numbered)
{
    LILA_ZONE();

    if (!numbered)
        m.seq = gSeq.load();
    else if (m.seq == 0)
        m.seq = ++gSeq;
    MeshHashes hashes;
    summarize(m, hashes);
    auto msg = std::make_shared<const Message>(std::move(m));
    if (!rec().subscribers.empty())
    {
        // Copied: a handler may unsubscribe itself or others.
        std::vector<Subscriber> subs;
        for (auto& [id, s] : rec().subscribers)
            subs.push_back(s);
        for (const Subscriber& s : subs)
        {
            json j = detail::toJson(*msg, s.filter);
            if (j.is_null())
                continue;
            try
            {
                s.fn(j);
            }
            catch (const std::exception& e)
            {
                log::error("change stream subscriber{}: {}", s.owner.empty() ? "" : " " + s.owner, e.what());
            }
        }
    }
    if (gServerActive.load())
        detail::serverSend(std::move(msg));
}

/** The message a gathered group becomes, its values read as they stand now. */
Message groupOf(Pending& p, MeshHashes& hashes)
{
    Message m;
    m.seq = p.seq;
    m.command = p.command;
    m.source = p.source.empty() ? "interactive" : p.source;
    m.ok = p.ok;
    if (m.command.empty())
    {
        UndoStack& u = UndoStack::get();
        if (!p.undo.empty())
            m.command = "undo";
        else if (!p.redo.empty())
            m.command = "redo";
        else if (u.newestId() != p.step)
            m.command = u.undoLabel();
    }
    if (!p.resync.empty())
    {
        m.type = Message::Type::Resync;
        m.reason = p.resync;
        m.path = p.resyncPath;
        return m;
    }
    m.undo = p.undo;
    m.redo = p.redo;
    fill(m, p.entries);
    if (!p.moved.empty() && wantsWorld())
    {
        appendWorld(m.entries, p.moved);
        recordWorld(m, false, hashes);
    }
    return m;
}

void flushNow()
{
    LILA_ZONE();

    Recorder& r = rec();
    std::vector<Pending> groups = std::move(r.sealed);
    r.sealed.clear();
    groups.push_back(std::move(r.p));
    r.p = {};
    if (!r.inCommand)
    {
        r.liveDirty.clear();
        r.liveSent.clear();
    }
    const bool ok = groups.back().ok;
    MeshHashes hashes;
    std::vector<std::pair<Message, std::vector<Message>>> out;
    bool timeMoved = false;
    bool resync = false;
    for (Pending& p : groups)
    {
        if (p.empty())
            continue;
        p.ok = ok;
        timeMoved |= p.time >= 0;
        out.emplace_back(groupOf(p, hashes), std::move(p.after));
        resync |= out.back().first.type == Message::Type::Resync;
    }
    if (resync)
        for (auto& [key, t] : r.trackers)
            t.sent.clear();
    else if (timeMoved)
        scheduleFrame();
    // What the edits changed downstream goes with the newest group.
    else if (!out.empty())
        appendTracked(out.back().first, false, hashes);
    for (auto& [m, after] : out)
    {
        if (m.type == Message::Type::Resync || !m.entries.empty())
            dispatch(std::move(m));
        for (Message& a : after)
            dispatch(std::move(a));
    }
}

/**
 * For a snapshot taken while a command or an interactive edit is still open: closes the group gathered so far under
 * the next seq, to go out when the edit ends, and gathers the rest as a group of its own. Returns the seq the
 * snapshot reflects.
 */
uint64_t sealOpenGroup()
{
    Recorder& r = rec();
    if (r.on && !r.p.empty() && (r.inCommand || UndoStack::get().chunkOpen()))
    {
        Pending next;
        next.command = r.p.command;
        next.source = r.p.source;
        next.started = r.p.started;
        next.step = r.p.step;
        r.p.seq = ++gSeq;
        r.sealed.push_back(std::move(r.p));
        r.p = std::move(next);
    }
    return gSeq.load();
}

void onExecute(const ExecuteEvent& e)
{
    Recorder& r = rec();
    if (!e.done)
    {
        flushNow();
        syncIfChanged();
        r.inCommand = true;
        r.p.command = std::string(e.name);
        r.p.source = e.source == CallSource::UI ? "ui" : "script";
        return;
    }
    r.p.ok = e.ok;
    r.inCommand = false;
    flushNow();
}

void onEvent(const events::EventData& d)
{
    Recorder& r = rec();
    std::u8string u8 = d.path.generic_u8string();
    std::string path(u8.begin(), u8.end());
    switch (d.type)
    {
    case events::Event::SceneOpened:
        setResync("open", path);
        break;
    case events::Event::SceneNew:
        setResync("new");
        break;
    case events::Event::Undo:
        r.p.undo = d.name;
        break;
    case events::Event::Redo:
        r.p.redo = d.name;
        break;
    case events::Event::TimeChanged:
        if (r.p.resync.empty() && r.p.time < 0)
        {
            r.p.time = long(r.p.entries.size());
            add({.kind = Kind::Time});
        }
        break;
    default:
    {
        Message m;
        m.type = Message::Type::Event;
        m.event = events::name(d.type);
        m.path = path;
        r.p.after.push_back(std::move(m));
        break;
    }
    }
    touched();
}

void start()
{
    Recorder& r = rec();
    r.on = true;
    r.p = {};
    r.sealed.clear();
    r.inCommand = false;
    rebuildCache();
    r.graphListener = Graph::current().addListener(onChange);
    r.executeListener = ops::addExecuteListener(onExecute);
    if (!r.evaluateHooked)
    {
        r.evaluateHooked = true;
        // Graph has no way to remove an evaluate hook, so this one stays, returning at once while nothing is diffed.
        Graph::addEvaluateHook([](Graph& g, bool after) {
            Recorder& r = rec();
            if (!r.on || r.evaluating || &g != &Graph::current())
                return;
            if (!after)
                syncIfChanged();
            else if (!r.trackers.empty() && !r.inCommand)
                scheduleFrame();
        });
    }
    using E = events::Event;
    for (E e : {E::SceneNew, E::SceneOpened, E::SceneSaved, E::SceneImported, E::SceneExported, E::Undo, E::Redo, E::TimeChanged})
        r.eventSubs.push_back(events::subscribe(e, onEvent));
    // Undo steps made outside commands go out once the stack has them.
    r.eventSubs.push_back(events::subscribe(E::UndoStackChanged, [](const events::EventData&) {
        Recorder& r = rec();
        if (r.on && !r.inCommand && !r.p.empty())
            scheduleFlush();
    }));
    syncTrackers();
}

void stop()
{
    Recorder& r = rec();
    r.on = false;
    Graph::current().removeListener(r.graphListener);
    ops::removeExecuteListener(r.executeListener);
    for (events::SubscriptionId id : r.eventSubs)
        events::unsubscribe(id);
    r.eventSubs.clear();
    r.cache.clear();
    r.p = {};
    r.sealed.clear();
    r.trackers.clear();
    r.worldAttr.clear();
    r.evaluatedAttrs.clear();
}

void updateRecording()
{
    const bool want = gServerActive.load() || !rec().subscribers.empty();
    if (want && !rec().on)
        start();
    else if (!want && rec().on)
        stop();
    else
        syncTrackers();
}
} // namespace

Filter Filter::fromJson(const json& j)
{
    if (j.is_null())
        return {};
    if (!j.is_object())
        throw std::invalid_argument("a filter is an object");
    auto list = [&j](const char* key) {
        std::vector<std::string> out;
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return out;
        if (it->is_string())
            return std::vector<std::string>{it->get<std::string>()};
        if (!it->is_array())
            throw std::invalid_argument(std::string(key) + " must be a list of strings");
        for (const json& s : *it)
        {
            if (!s.is_string())
                throw std::invalid_argument(std::string(key) + " must be a list of strings");
            out.push_back(s.get<std::string>());
        }
        return out;
    };
    Filter f;
    f.types = list("types");
    f.attrs = list("attrs");
    for (std::string& p : list("under"))
        f.under.push_back(normalPath(std::move(p)));
    auto flag = [&j](const char* key) {
        auto it = j.find(key);
        if (it == j.end() || it->is_null())
            return false;
        if (!it->is_boolean())
            throw std::invalid_argument(std::string(key) + " must be true or false");
        return it->get<bool>();
    };
    f.full = flag("full");
    f.live = flag("live");
    f.world = flag("world");
    f.evaluated = flag("evaluated");
    return f;
}

json Filter::toJson() const
{
    return {{"types", types}, {"attrs", attrs}, {"under", under},         {"full", full},
            {"live", live},   {"world", world}, {"evaluated", evaluated}};
}

uint64_t subscribe(Handler fn, Filter filter, std::string owner)
{
    Recorder& r = rec();
    const uint64_t id = r.nextSubscriber++;
    r.subscribers[id] = {std::move(fn), std::move(filter), std::move(owner)};
    updateRecording();
    return id;
}

uint64_t subscribe(ext::Context& ctx, Handler fn, Filter filter)
{
    const uint64_t id = subscribe(std::move(fn), std::move(filter), ctx.owner());
    ctx.track("changeStream:" + std::to_string(id), [id] { unsubscribe(id); });
    return id;
}

bool unsubscribe(uint64_t id)
{
    const bool erased = rec().subscribers.erase(id) > 0;
    updateRecording();
    return erased;
}

size_t unsubscribeOwned(std::string_view owner)
{
    const size_t n = std::erase_if(rec().subscribers, [owner](const auto& s) { return s.second.owner == owner; });
    updateRecording();
    return n;
}

json snapshot(const Filter& filter)
{
    return detail::toJson(*detail::buildSnapshot(filter.world, filter.evaluated), filter);
}

void flush()
{
    Recorder& r = rec();
    if (r.on && !r.inCommand && !UndoStack::get().chunkOpen())
        flushNow();
}

uint64_t sequence() { return gSeq.load(); }
bool recording() { return rec().on; }

namespace detail
{
void setServerActive(bool on)
{
    gServerActive = on;
    updateRecording();
}

void setServerFilters(std::vector<Filter> filters)
{
    bool live = false, world = false;
    for (const Filter& f : filters)
    {
        live |= f.live;
        world |= f.world;
    }
    {
        ServerFilters& sf = serverFilters();
        std::scoped_lock lock(sf.m);
        sf.filters = std::move(filters);
    }
    gServerLive = live;
    gServerWorld = world;
    gServerFiltersChanged = true;
}

void syncServerFilters()
{
    if (rec().on)
        syncIfChanged();
}

MessagePtr buildSnapshot(bool world, bool evaluated)
{
    LILA_ZONE();

    flush();
    Graph& g = Graph::current();
    auto m = std::make_shared<Message>();
    m->type = Message::Type::Snapshot;
    m->seq = sealOpenGroup();
    std::u8string u8 = io::currentPath().generic_u8string();
    m->path = std::string(u8.begin(), u8.end());
    m->time = g.time();
    if (world || evaluated)
        g.evaluate();

    // Parents before children, then the nodes outside the hierarchy.
    std::vector<NodeId> order;
    std::unordered_map<NodeId, bool> seen;
    for (NodeId root : g.roots())
    {
        order.push_back(root);
        for (NodeId d : g.descendants(root))
            order.push_back(d);
    }
    for (NodeId id : order)
        seen[id] = true;
    for (NodeId id : g.nodes())
        if (!seen.contains(id))
            order.push_back(id);

    m->nodes.reserve(order.size());
    for (NodeId id : order)
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        Message::Node out{.id = id, .info = infoOf(g, id), .attrs = storedAttrs(g, *n)};
        if (NodeId parent = g.parent(id); parent != scene::kNoNode)
            out.parent = g.fullPath(parent);
        if (const int attr = world ? worldAttrOf(*n) : -1; attr >= 0)
            out.world = std::get<glm::dmat4>(n->value(attr));
        if (evaluated)
            forEachEvaluated(g, *n, [&](int attr) { out.outputs.emplace_back(n->type().attrs[size_t(attr)].name, n->value(attr)); });
        m->nodes.push_back(std::move(out));
    }
    for (const auto& [dst, src] : g.connections())
        m->connections.emplace_back(g.fullPlugName(src), g.fullPlugName(dst));
    for (NodeId id : g.selection())
        m->selection.push_back(g.fullPath(id));
    MeshHashes hashes;
    summarize(*m, hashes);
    return m;
}

json toJson(const Message& m, const Filter& f)
{
    LILA_ZONE();

    switch (m.type)
    {
    case Message::Type::Group:
    {
        json changes = changesJson(m, f);
        if (changes.empty())
            return nullptr;
        json j = {{"type", "group"}, {"seq", m.seq}, {"command", m.command}, {"source", m.source}, {"ok", m.ok}};
        if (!m.undo.empty())
            j["undo"] = m.undo;
        if (!m.redo.empty())
            j["redo"] = m.redo;
        j["changes"] = std::move(changes);
        return j;
    }
    case Message::Type::Resync:
    {
        json j = {{"type", "resync"}, {"seq", m.seq}, {"reason", m.reason}, {"command", m.command}, {"source", m.source}};
        if (!m.path.empty())
            j["path"] = m.path;
        return j;
    }
    case Message::Type::Event:
        return {{"type", "event"}, {"seq", m.seq}, {"event", m.event}, {"path", m.path}};
    case Message::Type::Live:
    {
        if (!f.live)
            return nullptr;
        json changes = changesJson(m, f);
        if (changes.empty())
            return nullptr;
        json j = {{"type", "live"}, {"seq", m.seq}, {"source", m.source}};
        j["changes"] = std::move(changes);
        return j;
    }
    case Message::Type::Frame:
    {
        if (!f.world && !f.evaluated)
            return nullptr;
        json changes = changesJson(m, f);
        if (changes.empty())
            return nullptr;
        json j = {{"type", "frame"}, {"seq", m.seq}, {"time", m.time}};
        j["changes"] = std::move(changes);
        return j;
    }
    case Message::Type::Snapshot:
    {
        json nodes = json::array();
        for (const Message::Node& n : m.nodes)
        {
            if (!nodeOk(f, n.info))
                continue;
            json j = nodeJson("node", n.id, n.info);
            j.erase("kind");
            j["parent"] = n.parent.empty() ? json(nullptr) : json(n.parent);
            j["attrs"] = attrsJson(n.attrs, f, m);
            if (f.world && n.world)
                j["world"] = scene::toJson(*n.world);
            if (f.evaluated && !n.outputs.empty())
                j["outputs"] = attrsJson(n.outputs, f, m);
            nodes.push_back(std::move(j));
        }
        json connections = json::array();
        for (const auto& [src, dst] : m.connections)
        {
            const std::string_view node = plugNode(dst);
            const std::string_view attr = node.size() < dst.size() ? std::string_view(dst).substr(node.size() + 1) : std::string_view();
            if (isUnder(f, node) && attrOk(f, attr))
                connections.push_back({{"source", src}, {"destination", dst}});
        }
        json selection = json::array();
        for (const std::string& p : m.selection)
            if (isUnder(f, p))
                selection.push_back(p);
        json j = {{"type", "snapshot"}, {"seq", m.seq}, {"scene", m.path}, {"time", m.time}, {"selection", std::move(selection)}};
        j["nodes"] = std::move(nodes);
        j["connections"] = std::move(connections);
        return j;
    }
    }
    return nullptr;
}

std::string line(const json& j)
{
    std::string s = j.dump(-1, ' ', false, json::error_handler_t::replace);
    s += '\n';
    return s;
}
} // namespace detail
} // namespace lila::stream
