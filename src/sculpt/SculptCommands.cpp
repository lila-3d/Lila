#include "sculpt/SculptSession.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "sculpt/Reproject.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>

namespace lila::sculpt
{
namespace
{
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::Node;
using scene::NodeId;
using scene::PlugRef;
using scene::Transform;
namespace colors = geo::colors;

std::shared_ptr<Session>& activeRef()
{
    static std::shared_ptr<Session> s;
    return s;
}

std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}

void setActive(std::shared_ptr<Session> s)
{
    activeRef() = std::move(s);
    notify();
}

/** Rough: the cage, the proxy and the per-point layers the proxy carries (normals, mask, adjacency, BVH). */
size_t sessionBytes(const Session& s)
{
    return sizeof(Session) + s.cage.memoryBytes() + s.sculpt.mesh().memoryBytes() * 2;
}

size_t stackBytes(const SubdStack& stack)
{
    size_t n = 0;
    for (const geo::PointStencil& st : stack.stencils)
        n += (st.offsets.capacity() + st.cols.capacity()) * sizeof(Index) + st.weights.capacity() * sizeof(double);
    for (const auto& l : stack.levels)
        n += l->mesh.memoryBytes() + (l->parentPoint.capacity() + l->framePeer.capacity() + l->down.cols.capacity()) * sizeof(Index) +
             l->down.weights.capacity() * sizeof(double);
    return n;
}

size_t detailBytes(const LevelDetail& d)
{
    return d.offset.capacity() * sizeof(glm::dvec3) + d.layers.color.capacity() * sizeof(glm::vec4) +
           (d.layers.mask.capacity() + d.layers.freeze.capacity()) * sizeof(float);
}

/** The session and the level data it shares with the sessions of its other levels. */
void addSessionBytes(UndoShared& shared, const Session& s)
{
    shared.add(&s, sessionBytes(s));
    if (s.stack)
        shared.add(s.stack.get(), stackBytes(*s.stack));
    for (const DetailPtr& d : s.detail)
        if (d)
            shared.add(d.get(), detailBytes(*d));
}

/** Switches the open session; undo switches back. */
struct SwitchAction : UndoAction
{
    std::shared_ptr<Session> from, to;
    SwitchAction(std::shared_ptr<Session> f, std::shared_ptr<Session> t) : from(std::move(f)), to(std::move(t)) {}
    void undo() override { setActive(from); }
    void redo() override { setActive(to); }
    void sharedBytes(UndoShared& shared) const override
    {
        for (const auto& s : {from, to})
            if (s)
                addSessionBytes(shared, *s);
    }
};

std::function<void(const std::optional<glm::dvec3>&)>& pivotHandler()
{
    static std::function<void(const std::optional<glm::dvec3>&)> fn;
    return fn;
}

struct StrokeAction : UndoAction
{
    std::shared_ptr<Session> session;
    StrokeRecord record;
    StrokeAction(std::shared_ptr<Session> s, StrokeRecord r) : session(std::move(s)), record(std::move(r)) {}
    void undo() override
    {
        session->sculpt.apply(record, true);
        if (record.pivotMoved && pivotHandler())
            pivotHandler()(record.pivotBefore);
        notify();
    }
    void redo() override
    {
        session->sculpt.apply(record, false);
        if (record.pivotMoved && pivotHandler())
            pivotHandler()(record.pivotAfter);
        notify();
    }
    size_t bytes() const override
    {
        const StrokeRecord& r = record;
        return sizeof(*this) + r.points.capacity() * sizeof(Index)
            + (r.before.capacity() + r.after.capacity()) * sizeof(glm::dvec3)
            + (r.maskBefore.capacity() + r.maskAfter.capacity()) * sizeof(float) + r.frozenBefore.capacity()
            + r.frozenAfter.capacity() + (r.colorBefore.capacity() + r.colorAfter.capacity()) * sizeof(glm::vec4);
    }
    void sharedBytes(UndoShared& shared) const override { addSessionBytes(shared, *session); }
};

Graph& graph() { return Graph::current(); }

Node& meshArg(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

std::shared_ptr<Session> requireSession(const char* cmd)
{
    std::shared_ptr<Session> s = activeRef();
    if (!s)
        throw CommandError(std::string(cmd) + ": not in sculpt mode; run sculptEnter first");
    if (!graph().node(s->node))
        throw CommandError(std::string(cmd) + ": the sculpted mesh no longer exists; run sculptCancel");
    return s;
}

glm::dmat4 worldOf(const Node& n) { return n.as<glm::dmat4>(Transform::kWorldMatrix); }

double symmetryTolerance(const geo::Mesh& m) { return std::max(1e-4, glm::length(m.bounds().size()) * 1e-5); }

geo::Mesh storedMesh(const PlugRef& p)
{
    const MeshPtr& m = std::get<MeshPtr>(graph().storedValue(p));
    return m ? *m : geo::Mesh();
}

/** The drawn result of src, in the object space of another node. */
geo::Mesh meshInSpaceOf(const Node& src, const Node& space)
{
    graph().evaluate();
    MeshPtr m = src.as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        return {};
    geo::Mesh out = *m;
    out.compact();
    glm::dmat4 x = glm::inverse(worldOf(space)) * worldOf(src);
    for (glm::dvec3& p : out.positions())
        p = glm::dvec3(x * glm::dvec4(p, 1.0));
    return out;
}

Node* keptProxy(const Node& n)
{
    Graph& g = graph();
    const std::string want = n.name() + kProxySuffix;
    for (NodeId c : g.children(n.id()))
        if (Node* child = g.node(c); child && child->type().isA("mesh") && child->name() == want)
            return child;
    return nullptr;
}

/** The kept high poly's hidden attr, added on first use. */
int keptAttr(Node& kept, const char* name, scene::PlugType type, scene::PlugValue def, const char* doc)
{
    int i = kept.type().attrIndex(name);
    if (i < 0)
        i = graph().addAttr(kept.id(), scene::AttrDef{.name = name, .type = type, .defaultValue = std::move(def), .flags = scene::AttrHidden, .doc = doc});
    return i;
}

int64_t keptInt(const Node& kept, const char* name, int64_t fallback)
{
    const int i = kept.type().attrIndex(name);
    return i < 0 ? fallback : kept.as<int64_t>(i);
}

/** The mesh a resume rebuilds the levels from: the finest level, which the kept high poly holds itself unless it is a lower one. */
MeshPtr keptFinest(const Node& kept)
{
    if (const int i = kept.type().attrIndex(kFinestAttr); i >= 0)
        if (const MeshPtr& m = std::get<MeshPtr>(graph().storedValue({kept.id(), i, -1})); m)
            return m;
    const MeshPtr& m = std::get<MeshPtr>(graph().storedValue({kept.id(), MeshNode::kInMesh, -1}));
    return m ? m : std::make_shared<geo::Mesh>();
}

glm::dvec3 vec3(const json& j, const glm::dvec3& fallback = glm::dvec3(0.0))
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

glm::vec3 colorArg(const Args& a, const char* cmd)
{
    const json c = a.flags.value("color", json::array({1.0, 1.0, 1.0}));
    if (!c.is_array() || c.size() < 3 || !c[0].is_number() || !c[1].is_number() || !c[2].is_number())
        throw CommandError(std::string(cmd) + ": color must be [r, g, b], linear 0 to 1");
    return {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
}

BrushSettings settingsFrom(const Args& a)
{
    BrushSettings s;
    std::string name = a.get<std::string>("brush", "draw");
    if (!setBrush(s, name))
        throw CommandError("sculptStroke: unknown brush " + name);
    s.paint = a.get<bool>("paint");
    s.color = colorArg(a, "sculptStroke");
    std::string falloff = a.get<std::string>("falloff", "smooth");
    auto f = brush::Falloff::find(falloff);
    if (!f)
        throw CommandError("sculptStroke: unknown falloff " + falloff);
    s.falloff = *f;
    const std::string axis = a.get<std::string>("radialAxis", "y");
    s.radial = {std::clamp(a.get<int>("radialCount", 1), 1, 64), axis == "x" ? 0 : axis == "z" ? 2 : 1};
    s.strength = std::clamp(a.get<double>("strength", 0.5), 0.0, 1.0);
    s.invert = a.get<bool>("invert");
    s.frontFacesOnly = a.get<bool>("frontFacesOnly");
    return s;
}

/** Dabs given in world space, moved into the mesh's object space. */
std::vector<Dab> dabsFrom(const json& list, const glm::dmat4& world)
{
    std::vector<Dab> out;
    if (!list.is_array())
        throw CommandError("sculptStroke: dabs must be a list");
    const glm::dmat4 inv = glm::inverse(world);
    const glm::dmat3 normalToLocal = glm::transpose(glm::dmat3(world));
    const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(world)))));
    for (const json& j : list)
    {
        if (!j.is_object() || !j.contains("center"))
            throw CommandError("sculptStroke: each dab needs at least a center");
        Dab d;
        d.center = glm::dvec3(inv * glm::dvec4(vec3(j["center"]), 1.0));
        glm::dvec3 n = normalToLocal * vec3(j.value("normal", json()), glm::dvec3(0.0, 1.0, 0.0));
        d.normal = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0.0, 1.0, 0.0);
        d.radius = j.value("radius", 1.0) / scale;
        d.pressure = j.value("pressure", 1.0);
        // Scripts from before the brush core gave the amount as pressure.
        d.opacity = j.value("opacity", d.pressure);
        d.flow = j.value("flow", 1.0);
        d.viewDir = glm::dvec3(inv * glm::dvec4(vec3(j.value("view", json())), 0.0));
        d.delta = glm::dvec3(inv * glm::dvec4(vec3(j.value("delta", json())), 0.0));
        d.direction = glm::dvec3(inv * glm::dvec4(vec3(j.value("direction", json())), 0.0));
        out.push_back(d);
    }
    return out;
}

std::atomic<uint64_t> gLiveToken{1};

constexpr int kMaxLevels = 6;

void loadLayers(SculptMesh& sm, const LevelLayers& layers)
{
    if (!layers.mask.empty())
        sm.setMask(layers.mask);
    if (!layers.freeze.empty())
    {
        StrokeRecord r;
        for (Index p = 0; p < layers.freeze.size(); ++p)
            if (layers.freeze[p] > 0.5f)
            {
                r.points.push_back(p);
                r.frozenAfter.push_back(1);
            }
        sm.apply(r, false);
    }
    if (!layers.color.empty())
        sm.setColors(layers.color);
}

LevelLayers layersOf(const SculptMesh& sm)
{
    LevelLayers out;
    if (std::ranges::any_of(sm.mask(), [](float m) { return m > 0.0f; }))
        out.mask.assign(sm.mask().begin(), sm.mask().end());
    if (std::ranges::any_of(sm.frozen(), [](uint8_t f) { return f != 0; }))
        for (uint8_t f : sm.frozen())
            out.freeze.push_back(f ? 1.0f : 0.0f);
    if (sm.painted())
        out.color.assign(sm.colors().begin(), sm.colors().end());
    return out;
}

/**
 * The session sculpting `target` of stack, carrying s's sculpt there: down by least squares, recording the detail each
 * level adds, or up through the detail recorded before.
 */
std::shared_ptr<Session> atLevel(const Session& s, int target, std::shared_ptr<const SubdStack> stack)
{
    LILA_ZONE();

    std::vector<glm::dvec3> pos(s.sculpt.mesh().positions().begin(), s.sculpt.mesh().positions().end());
    LevelLayers layers = layersOf(s.sculpt);
    std::vector<DetailPtr> detail = s.detail;
    detail.resize(size_t(stack->top()) + 1);
    int l = s.level;
    for (; l > target; --l)
        detail[size_t(l)] = stepDown(*stack, l, pos, layers);
    for (; l < target; ++l)
    {
        stepUp(*stack, l + 1, detail[size_t(l) + 1], pos, layers);
        detail[size_t(l) + 1] = nullptr;
    }

    auto out = std::make_shared<Session>();
    out->node = s.node;
    out->mode = s.mode;
    out->subdivide = stack->opts;
    out->cage = s.cage;
    out->proxyNode = s.proxyNode;
    out->stack = stack;
    out->level = target;
    out->detail = std::move(detail);
    if (stack == s.stack)
        out->exitSolver = s.exitSolver;
    geo::Mesh mesh = stack->levels[size_t(target)]->mesh;
    std::ranges::copy(pos, mesh.positions().begin());
    out->sculpt = SculptMesh(std::move(mesh));
    loadLayers(out->sculpt, layers);
    syncSymmetry(*out);
    return out;
}

/** The level sculptExit's high and highLevel pick, from 1 to top. */
int highLevel(const std::string& high, int x, int top)
{
    const int level = high == "belowHighest" ? top - x : high == "level" ? x : top;
    return std::clamp(level, std::min(1, top), top);
}

json levelJson(const Session& s)
{
    return {{"level", s.level},
            {"levels", s.stack ? s.stack->top() : 0},
            {"points", s.sculpt.mesh().numPoints()},
            {"faces", s.sculpt.mesh().numFaces()}};
}

/** The finest level's positions, the ones Exit Sculpt fits the cage to. */
std::vector<glm::dvec3> topPositions(const Session& s)
{
    std::vector<glm::dvec3> pos(s.sculpt.mesh().positions().begin(), s.sculpt.mesh().positions().end());
    if (!s.stack)
        return pos;
    for (int l = s.level + 1; l <= s.stack->top(); ++l)
    {
        LevelLayers none;
        stepUp(*s.stack, l, s.detail[size_t(l)], pos, none);
    }
    return pos;
}

/** Level `level`'s positions: stepped down from the sculpted level by least squares, or up through the detail. */
std::vector<glm::dvec3> positionsAt(const Session& s, int level)
{
    std::vector<glm::dvec3> pos(s.sculpt.mesh().positions().begin(), s.sculpt.mesh().positions().end());
    int l = s.level;
    for (; l > level; --l)
    {
        LevelLayers none;
        stepDown(*s.stack, l, pos, none);
    }
    for (; l < level; ++l)
    {
        LevelLayers none;
        stepUp(*s.stack, l + 1, s.detail[size_t(l) + 1], pos, none);
    }
    return pos;
}

/** Builds the proxy for a new session. */
void buildProxy(Session& s, const Args& a, Node& n)
{
    LILA_ZONE();

    Graph& g = graph();
    const std::string mode = a.get<std::string>("mode", "subdivide");
    const int levels = std::clamp(a.get<int>("levels", 2), 1, 6);
    s.subdivide.levels = levels;
    geo::Mesh proxy;
    bool resumed = false;

    if (a.get<bool>("resume", true) && mode != "direct")
        if (Node* kept = keptProxy(n))
        {
            proxy = *keptFinest(*kept);
            // A kept subdivision sculpt is recognised by its point count, provided the cage kept its topology.
            s.mode = ProxyMode::Remesh;
            geo::SubdivideOptions o = s.subdivide;
            o.levels = 0;
            std::shared_ptr<const SubdStack> stack = buildSubdStack(s.cage, o);
            while (stack->top() < kMaxLevels)
            {
                stack = addSubdLevel(*stack);
                const geo::Mesh& sub = stack->levels.back()->mesh;
                if (sub.pointCapacity() == proxy.pointCapacity() && sub.numFaces() == proxy.numFaces())
                {
                    s.mode = ProxyMode::Subdivide;
                    s.subdivide = stack->opts;
                    s.stack = std::move(stack);
                    break;
                }
                if (sub.pointCapacity() > proxy.pointCapacity())
                    break;
            }
            s.proxyNode = kept->id();
            resumed = true;
        }

    if (!resumed)
    {
        if (mode == "subdivide")
        {
            s.mode = ProxyMode::Subdivide;
            s.stack = buildSubdStack(s.cage, s.subdivide);
            proxy = s.stack->levels.back()->mesh;
        }
        else if (mode == "remesh")
        {
            s.mode = ProxyMode::Remesh;
            retopo::VoxelOptions vo;
            vo.resolution = std::clamp(a.get<int>("resolution", 192), 16, 2048);
            proxy = retopo::voxelRemesh(geo::subdivide(s.cage, s.subdivide), vo);
        }
        else if (mode == "direct")
        {
            s.mode = ProxyMode::Direct;
            proxy = s.cage;
        }
        else
            throw CommandError("sculptEnter: mode must be subdivide, remesh or direct");
        if (s.mode != ProxyMode::Direct)
            geo::clearSymmetryKeys(proxy, 0);
    }
    if (proxy.numFaces() == 0)
        throw CommandError("sculptEnter: " + n.path() + " has no faces");
    if (s.stack)
    {
        s.level = s.stack->top();
        s.detail.assign(size_t(s.level) + 1, nullptr);
    }

    LevelLayers layers;
    if (auto m = std::as_const(proxy).pointAttrs().get<float>(kMaskLayer); !m.empty())
        layers.mask.assign(m.begin(), m.end());
    if (auto f = std::as_const(proxy).pointAttrs().get<float>(kFreezeLayer); !f.empty())
        layers.freeze.assign(f.begin(), f.end());
    if (colors::has(proxy, kColorSet))
        layers.color = colors::pointColors(proxy, kColorSet);
    s.sculpt = SculptMesh(std::move(proxy));
    loadLayers(s.sculpt, layers);
    (void)g;
}

/** Each cage point's polypaint: the sculpt's color at the closest point of its surface. */
void paintCage(const Session& s, geo::Mesh& cage)
{
    LILA_ZONE();

    const SculptMesh& sm = s.sculpt;
    const auto& corners = sm.bvh().triangles().corners;
    std::vector<glm::vec4> perPoint(cage.pointCapacity(), glm::vec4(1.0f));
    tbb::parallel_for(Index(0), cage.pointCapacity(), [&](Index p) {
        geo::BvhClosest hit;
        if (!cage.pointValid(p) || !sm.bvh().closestPoint(cage.position(p), hit))
            return;
        glm::vec4 c(0.0f);
        for (int k = 0; k < 3; ++k)
            c += sm.colors()[sm.mesh().cornerPoint(corners[size_t(hit.triangle) * 3 + k])] * float(hit.bary[k]);
        perPoint[p] = c;
    });
    colors::add(cage, kColorSet);
    colors::setFromPoints(cage, kColorSet, perPoint);
    colors::settle(cage, kColorSet);
}

/** The proxy as a mesh to keep in the scene, with its mask and freeze state. */
MeshPtr proxyToKeep(const Session& s)
{
    auto m = std::make_shared<geo::Mesh>(s.sculpt.mesh());
    auto mask = m->pointAttrs().getOrAdd<float>(kMaskLayer);
    auto freeze = m->pointAttrs().getOrAdd<float>(kFreezeLayer);
    for (Index p = 0; p < m->pointCapacity(); ++p)
    {
        mask[p] = s.sculpt.mask()[p];
        freeze[p] = s.sculpt.frozen()[p] ? 1.0f : 0.0f;
    }
    if (s.sculpt.painted())
    {
        colors::add(*m, kColorSet);
        colors::setFromPoints(*m, kColorSet, s.sculpt.colors());
    }
    else
        colors::remove(*m, kColorSet);
    return m;
}
} // namespace

std::shared_ptr<Session> active() { return activeRef(); }

std::optional<KeptSculpt> keptSculpt(const scene::Node& mesh)
{
    Node* kept = keptProxy(mesh);
    if (!kept)
        return std::nullopt;
    KeptSculpt k;
    k.node = kept->id();
    k.level = int(keptInt(*kept, kResumeLevelAttr, -1));
    // The finest level's face count names how many levels it has: the first splits each face into one quad per corner,
    // each after it quadruples.
    const MeshPtr finest = keptFinest(*kept);
    const MeshPtr cage = scene::modifiers::cage(graph(), mesh.id());
    if (cage && cage->numFaces() > 0)
        for (Index faces = cage->numCorners(), l = 1; l <= kMaxLevels; ++l, faces *= 4)
            if (faces == finest->numFaces())
                k.levels = int(l);
    return k;
}

int addListener(std::function<void()> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeListener(int id) { listeners().erase(id); }

void notify()
{
    auto copy = listeners();
    for (auto& [id, fn] : copy)
        fn();
}

void syncSymmetry(Session& s)
{
    Node* n = graph().node(s.node);
    const std::optional<geo::SymmetrySpec> spec = n ? scene::symmetrySpec(*n) : std::nullopt;
    const std::string key = !spec ? std::string() : spec->radial ? geo::radialLayerName(*spec->radial) : geo::symmetryLayerName(spec->axis);
    if (key == s.symmetryKey)
        return;
    s.symmetryKey = key;
    s.sculpt.setSymmetry(spec, symmetryTolerance(s.sculpt.mesh()));
}

uint64_t nextLiveToken() { return gLiveToken.fetch_add(1); }

void setPivotHandler(std::function<void(const std::optional<glm::dvec3>&)> handler) { pivotHandler() = std::move(handler); }

struct ExitSolver
{
    /** The symmetry layer the map keyed (empty without symmetry) and its plane offset. */
    std::string symmetryKey;
    double offset = 0.0;
    /** The cage the symmetry map keyed, kept apart from the session's. */
    geo::Mesh keyed;
    std::optional<geo::SymmetryMap> map;
    /** The cage's own subdivision, for fitting a remeshed proxy. */
    std::vector<geo::PointStencil> stencils;
    std::unique_ptr<CageFitter> fitter;
};

std::vector<glm::dvec3> exitPositions(Session& s, const std::string& method, int iterations)
{
    LILA_ZONE();

    const geo::Mesh& proxy = s.sculpt.mesh();
    if (s.mode == ProxyMode::Direct)
        return {proxy.positions().begin(), proxy.positions().end()};
    const Node* n = graph().node(s.node);
    const std::optional<geo::SymmetrySpec> spec = n ? scene::symmetrySpec(*n) : std::nullopt;
    const std::string key = !spec ? std::string() : spec->radial ? geo::radialLayerName(*spec->radial) : geo::symmetryLayerName(spec->axis);
    const double offset = spec && !spec->radial ? scene::symmetryOffset(*n, spec->axis) : 0.0;
    std::shared_ptr<ExitSolver>& x = s.exitSolver;
    if (!x || x->symmetryKey != key || x->offset != offset)
    {
        x = std::make_shared<ExitSolver>();
        x->symmetryKey = key;
        x->offset = offset;
        if (spec)
        {
            x->keyed = s.cage;
            x->map = scene::symmetryMapFor(*n, x->keyed);
        }
    }
    FitOptions fo;
    fo.method = method == "closest" ? FitMethod::Closest : method == "normal" ? FitMethod::Normal : FitMethod::LeastSquares;
    fo.subdivide = s.subdivide;
    fo.iterations = iterations;
    fo.symmetry = x->map ? &*x->map : nullptr;
    const bool belowTop = s.stack && s.level < s.stack->top();
    if (fo.method != FitMethod::LeastSquares)
    {
        if (!belowTop)
        {
            geo::Mesh fitted = fitCageToSurface(s.cage, proxy, s.sculpt.bvh(), fo);
            return {fitted.positions().begin(), fitted.positions().end()};
        }
        geo::Mesh top = s.stack->levels.back()->mesh;
        std::ranges::copy(topPositions(s), top.positions().begin());
        geo::MeshBvh bvh(top);
        geo::Mesh fitted = fitCageToSurface(s.cage, top, bvh, fo);
        return {fitted.positions().begin(), fitted.positions().end()};
    }
    if (!x->fitter)
    {
        if (s.mode != ProxyMode::Subdivide)
            x->stencils = geo::subdivideStencils(s.cage, s.subdivide);
        x->fitter = std::make_unique<CageFitter>(s.cage, s.mode == ProxyMode::Subdivide ? s.stack->stencils : x->stencils, fo);
    }
    if (s.mode == ProxyMode::Subdivide && belowTop)
        return x->fitter->solve(s.cage.positions(), topPositions(s));
    if (s.mode == ProxyMode::Subdivide)
        return x->fitter->solve(s.cage.positions(), proxy.positions());
    return fitToSurface(*x->fitter, x->stencils, s.cage.positions(), s.sculpt.bvh(), iterations);
}

geo::Mesh lowPoly(Session& s, const std::string& method, int level, int iterations)
{
    LILA_ZONE();

    if (!s.stack || level <= 0)
    {
        geo::Mesh m = s.cage;
        const std::vector<glm::dvec3> pos = exitPositions(s, method, iterations);
        for (Index p = 0; p < m.pointCapacity() && p < pos.size(); ++p)
            if (m.pointValid(p))
                m.setPosition(p, pos[p]);
        return m;
    }
    const int top = s.stack->top();
    level = std::min(level, top);
    geo::Mesh m = s.stack->levels[size_t(level)]->mesh;
    std::ranges::copy(positionsAt(s, level), m.positions().begin());
    // A level's own points are its subdivision cage; the finest level is the sculpt itself.
    if (method == "fit" || level == top)
        return m;
    FitOptions fo;
    fo.method = method == "normal" ? FitMethod::Normal : FitMethod::Closest;
    if (s.level == top)
        return fitCageToSurface(m, s.sculpt.mesh(), s.sculpt.bvh(), fo);
    geo::Mesh surface = s.stack->levels.back()->mesh;
    std::ranges::copy(topPositions(s), surface.positions().begin());
    const geo::MeshBvh bvh(surface);
    return fitCageToSurface(m, surface, bvh, fo);
}

void registerCommands()
{
    using T = ArgType;

    graph().addListener([](scene::Change c, NodeId) {
        if (c == scene::Change::Cleared && activeRef())
            setActive(nullptr);
    }, true);

    ops::registerCommand({
        .name = "sculptEnter",
        .doc = "Enter sculpt mode on a mesh: builds a high-resolution copy of its cage to sculpt on, outside the scene. "
               "mode='subdivide' gives Catmull-Clark levels to step between (sculptLevel) and fits back exactly on exit; "
               "'remesh' sculpts a uniform voxel remesh; 'direct' sculpts the mesh itself. Resumes from the high poly a previous "
               "exit kept (<mesh>_high). Returns {mesh, mode, level, levels, points, faces}.",
        .positionalDoc = "mesh",
        .flags = {{"mode", "", T::String, "subdivide", "subdivide, remesh or direct.", {"subdivide", "remesh", "direct"}},
                  {"levels", "", T::Int, 2, "Subdivision levels to start with, 1 to 6.", {}, {}, 1.0, 6.0},
                  {"resolution", "", T::Int, 192, "Voxels along the longest side, for mode='remesh'.", {}, {}, 16.0, 2048.0},
                  {"resume", "", T::Bool, true, "Continue from the mesh's kept high poly when it has one."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("sculptEnter: select a mesh");
            Graph& g = graph();
            Node& n = meshArg(objs.front(), "sculptEnter");
            UndoChunk chunk("Enter Sculpt");
            std::shared_ptr<Session> prev = activeRef();
            if (prev && prev->node == n.id())
                return {{"mesh", n.path()}, {"points", prev->sculpt.mesh().numPoints()}};
            if (prev)
                ops::execute("sculptExit", json::object());

            g.evaluate();
            MeshPtr cage = scene::modifiers::cage(g, n.id());
            if (!cage || cage->numFaces() == 0)
                throw CommandError("sculptEnter: " + n.path() + " has no faces");
            auto s = std::make_shared<Session>();
            s->node = n.id();
            s->cage = *cage;
            buildProxy(*s, a, n);
            syncSymmetry(*s);
            if (Node* kept = s->proxyNode != scene::kNoNode ? g.node(s->proxyNode) : nullptr; kept && s->stack)
                if (const int64_t at = keptInt(*kept, kResumeLevelAttr, -1); at >= 0 && at < s->stack->top())
                    s = atLevel(*s, int(at), s->stack);
            UndoStack::get().record(std::make_unique<SwitchAction>(activeRef(), s));
            setActive(s);
            const char* modes[] = {"subdivide", "remesh", "direct"};
            log::info("sculptEnter {}: {}, {} points, {} faces", n.path(), modes[int(s->mode)], s->sculpt.mesh().numPoints(),
                      s->sculpt.mesh().numFaces());
            return {{"mesh", n.path()},
                    {"mode", modes[int(s->mode)]},
                    {"level", s->stack ? s->level : 0},
                    {"levels", s->stack ? s->stack->top() : 0},
                    {"points", s->sculpt.mesh().numPoints()},
                    {"faces", s->sculpt.mesh().numFaces()}};
        },
    });

    ops::registerCommand({
        .name = "sculptExit",
        .doc = "Leave sculpt mode, moving the mesh (the low poly) onto the sculpt. method='closest' moves each point to the "
               "nearest spot on the sculpt and 'normal' along its normal onto it: a game mesh to bake onto. 'fit' solves for "
               "the cage whose subdivision best matches the sculpt, its points off the surface: a mesh to subdivide again. "
               "The fit is to the finest level from any level. Only positions change: UVs and other attributes stay, except "
               "that polypaint lands on the mesh's polypaint color set. high keeps the sculpt as the hidden child <mesh>_high "
               "to bake from, at the finest level, highLevel levels below it, or level highLevel; the next sculptEnter gets "
               "every level back from it, less the levels lowLevel moved into the low poly. Returns {mesh, maxMove, high: "
               "{mesh, level, levels} or null}.",
        .flags = {{"method", "", T::String, "closest", "closest, normal or fit.", {"closest", "normal", "fit"}},
                  {"high", "", T::String, "highest", "Which level <mesh>_high keeps: highest, belowHighest, level, or none to keep nothing.",
                   {"highest", "belowHighest", "level", "none"}},
                  {"highLevel", "", T::Int, 1, "Levels below the highest (belowHighest), or the level (level).", {}, {}, 0.0, double(kMaxLevels)},
                  {"lowLevel", "", T::Int, 0, "The level the low poly comes out at: 0 keeps the mesh's topology, higher subdivides it "
                                               "that many times (its UVs with it).", {}, {}, 0.0, double(kMaxLevels)},
                  {"iterations", "", T::Int, 3, "Project-and-fit rounds for remeshed proxies.", {}, {}, 1.0, 20.0}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptExit");
            const std::shared_ptr<Session> open = s;
            if (s->stack && s->level < s->stack->top())
                s = atLevel(*s, s->stack->top(), s->stack);
            Graph& g = graph();
            Node& n = *g.node(s->node);
            std::string method = a.get<std::string>("method", "closest");
            if (method != "fit" && method != "closest" && method != "normal")
                throw CommandError("sculptExit: method must be fit, closest or normal");
            const std::string high = a.get<std::string>("high", "highest");
            if (high != "highest" && high != "belowHighest" && high != "level" && high != "none")
                throw CommandError("sculptExit: high must be highest, belowHighest, level or none");
            UndoChunk chunk("Exit Sculpt");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh mesh = storedMesh(base);
            if (mesh.pointCapacity() != s->cage.pointCapacity() || mesh.numFaces() != s->cage.numFaces())
                throw CommandError("sculptExit: " + n.path() + "'s topology changed since sculptEnter; undo that edit or run sculptCancel");

            const int iterations = std::clamp(a.get<int>("iterations"), 1, 20);
            const int lowLevel = s->stack ? std::clamp(a.get<int>("lowLevel"), 0, s->stack->top()) : 0;
            double maxMove = 0.0;
            if (lowLevel == 0)
            {
                std::vector<glm::dvec3> pos = exitPositions(*s, method, iterations);
                for (Index p = 0; p < mesh.pointCapacity() && p < pos.size(); ++p)
                    if (mesh.pointValid(p))
                    {
                        maxMove = std::max(maxMove, glm::length(pos[p] - s->cage.position(p)));
                        mesh.setPosition(p, pos[p]);
                    }
            }
            else
                mesh = lowPoly(*s, method, lowLevel, iterations);
            if (s->sculpt.painted())
                paintCage(*s, mesh);
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));

            Node* kept = s->proxyNode != scene::kNoNode ? g.node(s->proxyNode) : nullptr;
            if (!kept)
                kept = keptProxy(n);
            json keptOut = nullptr;
            if (high != "none" && s->mode != ProxyMode::Direct)
            {
                if (!kept)
                {
                    kept = g.createNode(*scene::types::find("mesh"), n.name() + kProxySuffix, n.id(), false);
                    g.setValue({kept->id(), Transform::kVisibility, -1}, false);
                }
                const int top = s->stack ? s->stack->top() : 0;
                const int level = s->stack ? highLevel(high, a.get<int>("highLevel"), top) : 0;
                g.setValue({kept->id(), MeshNode::kInMesh, -1}, proxyToKeep(level == top ? *s : *atLevel(*s, level, s->stack)));
                // A lower level keeps the finest beside it, so the next Enter Sculpt gets every level back.
                const bool lower = s->stack && level < top;
                if (lower || kept->type().attrIndex(kFinestAttr) >= 0)
                    g.setValue({kept->id(), keptAttr(*kept, kFinestAttr, scene::PlugType::Mesh, MeshPtr(), "The sculpt's finest level."), -1},
                               lower ? proxyToKeep(*s) : MeshPtr());
                if (s->stack)
                    g.setValue({kept->id(), keptAttr(*kept, kResumeLevelAttr, scene::PlugType::Int, int64_t(-1), "The level Enter Sculpt resumes at."), -1},
                               int64_t(std::max(open->level - lowLevel, 0)));
                keptOut = {{"mesh", kept->path()}, {"level", level}, {"levels", top}};
            }
            else if (kept)
                g.deleteNode(kept->id());

            UndoStack::get().record(std::make_unique<SwitchAction>(open, nullptr));
            setActive(nullptr);
            return {{"mesh", n.path()}, {"maxMove", maxMove}, {"high", keptOut}};
        },
    });

    ops::registerCommand({
        .name = "sculptDuplicateLowPoly",
        .doc = "Without leaving sculpt mode, duplicate the mesh as the low poly sculptExit would leave with this method at this "
               "level, next to it. The copy keeps the mesh's materials, transform and modifiers. Returns its name.",
        .flags = {{"method", "", T::String, "closest", "closest, normal or fit.", {"closest", "normal", "fit"}},
                  {"level", "", T::Int, 0, "0 keeps the mesh's topology; higher subdivides it that many times.", {}, {}, 0.0, double(kMaxLevels)},
                  {"name", "", T::String, "", "Name for the copy; default <mesh>_low<level>."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptDuplicateLowPoly");
            const std::string method = a.get<std::string>("method", "closest");
            if (method != "fit" && method != "closest" && method != "normal")
                throw CommandError("sculptDuplicateLowPoly: method must be fit, closest or normal");
            const int level = s->stack ? std::clamp(a.get<int>("level"), 0, s->stack->top()) : 0;
            Graph& g = graph();
            Node& n = *g.node(s->node);
            std::string name = a.get<std::string>("name", "");
            if (name.empty())
                name = n.name() + "_low" + std::to_string(level);
            UndoChunk chunk("Duplicate Low Poly");
            json made = ops::execute("duplicate", {{"args", {n.path()}}, {"name", name}});
            const std::string path = made.is_array() && !made.empty() ? made[0].get<std::string>() : std::string();
            Node* copy = g.find(path);
            if (!copy)
                throw CommandError("sculptDuplicateLowPoly: duplicating " + n.path() + " failed");
            // The kept high poly belongs to the mesh, not to its copies.
            const std::string keptName = n.name() + kProxySuffix;
            for (NodeId c : std::vector<NodeId>(g.children(copy->id())))
                if (Node* child = g.node(c); child && child->type().isA("mesh") && child->name().starts_with(keptName))
                    g.deleteNode(c);
            geo::Mesh low = lowPoly(*s, method, level);
            if (s->sculpt.painted())
                paintCage(*s, low);
            g.setValue(scene::modifiers::editPlug(g, copy->id()), MeshPtr(std::make_shared<geo::Mesh>(std::move(low))));
            return copy->path();
        },
    });

    ops::registerCommand({
        .name = "sculptCancel",
        .doc = "Leave sculpt mode without changing the mesh, discarding the sculpt (a kept <mesh>_high stays as it was).",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = activeRef();
            if (!s)
                return nullptr;
            UndoChunk chunk("Cancel Sculpt");
            UndoStack::get().record(std::make_unique<SwitchAction>(s, nullptr));
            setActive(nullptr);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sculptStroke",
        .doc = "One brush stroke on the sculpt: dabs is a list of {center, normal, radius, pressure, opacity, flow, view, "
               "delta, direction} in world space (only center is required; opacity defaults to pressure). Honours the mask, "
               "frozen points and the mesh's symmetry, mirrored exactly. Returns {points} changed.",
        .flags = {{"brush", "", T::String, "draw", "draw, clayStrips, inflate, grab, smooth, flatten, pinch, crease, scrape, damStandard, "
                                                     "trimDynamic, polish, fold, mask, relax, freeze, paint, or a brush an extension registered."},
                  {"paint", "", T::Bool, false, "Shape brushes also paint color where they move points."},
                  {"color", "", T::List, json::array({1.0, 1.0, 1.0}), "Paint color, linear [r, g, b]."},
                  {"falloff", "", T::String, "smooth", "Falloff: smooth, sphere, root, sharp, linear, constant, bell, or a soft select preset name."},
                  {"strength", "", T::Float, 0.5, "0 to 1.", {}, {}, 0.0, 1.0},
                  {"invert", "", T::Bool, false, "Dig, unmask, unfreeze or magnify instead."},
                  {"frontFacesOnly", "", T::Bool, false, "Leave points facing away from the view."},
                  {"radialCount", "", T::Int, 1, "Copies of every dab turned evenly about an object axis; 1 is off.", {}, {}, 1.0, 64.0},
                  {"radialAxis", "", T::String, "y", "The object axis radial copies turn about.", {"x", "y", "z"}},
                  {"dabs", "", T::List, json::array(), "The stamps, in order."},
                  {"live", "", T::Int, 0, "Internal: adopts the stroke the viewport tool already applied."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptStroke");
            StrokeRecord rec;
            uint64_t token = uint64_t(a.get<int64_t>("live"));
            if (token != 0 && token == s->liveToken)
            {
                rec = std::move(s->live);
                s->live = {};
                s->liveToken = 0;
            }
            else
            {
                syncSymmetry(*s);
                std::vector<Dab> dabs = dabsFrom(a.flags.value("dabs", json::array()), worldOf(*graph().node(s->node)));
                s->sculpt.beginStroke(settingsFrom(a));
                for (const Dab& d : dabs)
                    s->sculpt.dab(d);
                rec = s->sculpt.endStroke();
            }
            size_t count = rec.points.size();
            if (!rec.empty())
            {
                UndoChunk chunk("Sculpt Stroke");
                UndoStack::get().record(std::make_unique<StrokeAction>(s, std::move(rec)));
            }
            notify();
            return {{"points", count}};
        },
    });

    ops::registerCommand({
        .name = "sculptMask",
        .doc = "Change the sculpt mask (masked points resist every brush) or the freeze layer (frozen points never move): "
               "clear, fill, invert, or blur (mask only).",
        .flags = {{"op", "", T::String, "clear", "clear, fill, invert or blur.", {"clear", "fill", "invert", "blur"}},
                  {"layer", "", T::String, "mask", "mask or freeze.", {"mask", "freeze"}},
                  {"iterations", "", T::Int, 2, "Blur passes.", {}, {}, 1.0, 100.0}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptMask");
            std::string op = a.get<std::string>("op", "clear");
            MaskOp m = op == "fill" ? MaskOp::Fill : op == "invert" ? MaskOp::Invert : op == "blur" ? MaskOp::Blur : MaskOp::Clear;
            if (op != "clear" && op != "fill" && op != "invert" && op != "blur")
                throw CommandError("sculptMask: op must be clear, fill, invert or blur");
            Layer layer = a.get<std::string>("layer", "mask") == "freeze" ? Layer::Freeze : Layer::Mask;
            StrokeRecord rec = s->sculpt.maskOp(m, layer, std::clamp(a.get<int>("iterations"), 1, 100));
            size_t count = rec.points.size();
            if (!rec.empty())
            {
                UndoChunk chunk("Sculpt Mask");
                UndoStack::get().record(std::make_unique<StrokeAction>(s, std::move(rec)));
            }
            notify();
            return {{"points", count}};
        },
    });

    ops::registerCommand({
        .name = "sculptColor",
        .doc = "Fill the sculpt's polypaint with a color (masked points keep theirs) or clear it back to white.",
        .flags = {{"op", "", T::String, "fill", "fill or clear.", {"fill", "clear"}},
                  {"color", "", T::List, json::array({1.0, 1.0, 1.0}), "Fill color, linear [r, g, b]."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptColor");
            const bool clear = a.get<std::string>("op", "fill") == "clear";
            StrokeRecord rec = clear ? s->sculpt.clearColor() : s->sculpt.fillColor(colorArg(a, "sculptColor"));
            size_t count = rec.points.size();
            if (!rec.empty())
            {
                UndoChunk chunk(clear ? "Clear Polypaint" : "Fill Polypaint");
                UndoStack::get().record(std::make_unique<StrokeAction>(s, std::move(rec)));
            }
            notify();
            return {{"points", count}};
        },
    });

    ops::registerCommand({
        .name = "sculptLevel",
        .doc = "Sculpt another subdivision level: 0 is the cage, the highest the finest. Going down keeps the finer detail, "
               "and going back up lays it over whatever changed below. Returns {level, levels, points, faces}.",
        .flags = {{"level", "", T::Int, 0, "The level to sculpt.", {}, {}, 0.0, double(kMaxLevels)},
                  {"step", "", T::Int, 0, "Move this many levels from the current one instead (1 up, -1 down)."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptLevel");
            if (!s->stack)
                throw CommandError("sculptLevel: levels need a subdivide sculpt; this one is remeshed or direct");
            const int step = a.get<int>("step");
            const int target = std::clamp(step != 0 ? s->level + step : a.get<int>("level"), 0, s->stack->top());
            if (target != s->level)
            {
                UndoChunk chunk("Sculpt Level");
                std::shared_ptr<Session> to = atLevel(*s, target, s->stack);
                UndoStack::get().record(std::make_unique<SwitchAction>(s, to));
                setActive(to);
                s = to;
            }
            return levelJson(*s);
        },
    });

    ops::registerCommand({
        .name = "sculptAddLevel",
        .doc = "Subdivide the sculpt once more and sculpt the new level (from a lower level, the finest is subdivided). "
               "Returns {level, levels, points, faces}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptAddLevel");
            if (!s->stack)
                throw CommandError("sculptAddLevel: levels need a subdivide sculpt; this one is remeshed or direct");
            if (s->stack->top() >= kMaxLevels)
                throw CommandError("sculptAddLevel: already at the most levels, " + std::to_string(kMaxLevels));
            UndoChunk chunk("Add Sculpt Level");
            std::shared_ptr<const SubdStack> stack = addSubdLevel(*s->stack);
            std::shared_ptr<Session> to = atLevel(*s, stack->top(), stack);
            UndoStack::get().record(std::make_unique<SwitchAction>(s, to));
            setActive(to);
            return levelJson(*to);
        },
    });

    ops::registerCommand({
        .name = "sculptDeleteHigher",
        .doc = "Delete the levels above the one being sculpted, and their detail. Returns {level, levels, points, faces}.",
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = requireSession("sculptDeleteHigher");
            if (!s->stack)
                throw CommandError("sculptDeleteHigher: levels need a subdivide sculpt; this one is remeshed or direct");
            if (s->level == 0)
                throw CommandError("sculptDeleteHigher: the cage is level 0; go up a level first, a sculpt keeps at least one");
            if (s->level == s->stack->top())
                return levelJson(*s);
            UndoChunk chunk("Delete Higher Levels");
            std::shared_ptr<const SubdStack> stack = truncateSubdStack(*s->stack, s->level);
            std::shared_ptr<Session> to = atLevel(*s, s->level, stack);
            UndoStack::get().record(std::make_unique<SwitchAction>(s, to));
            setActive(to);
            return levelJson(*to);
        },
    });

    ops::registerCommand({
        .name = "sculptState",
        .doc = "The open sculpt session: {active, mesh, mode, level, levels, points, faces, symmetry, masked, frozen, painted}.",
        .undoable = false,
        .run = [](const Args&) -> json {
            std::shared_ptr<Session> s = activeRef();
            Node* n = s ? graph().node(s->node) : nullptr;
            if (!s || !n)
                return {{"active", false}};
            const char* modes[] = {"subdivide", "remesh", "direct"};
            size_t masked = 0, frozen = 0;
            for (float m : s->sculpt.mask())
                masked += m > 0.0f;
            for (uint8_t f : s->sculpt.frozen())
                frozen += f != 0;
            return {{"active", true},
                    {"mesh", n->path()},
                    {"mode", modes[int(s->mode)]},
                    {"level", s->stack ? s->level : 0},
                    {"levels", s->stack ? s->stack->top() : 0},
                    {"points", s->sculpt.mesh().numPoints()},
                    {"faces", s->sculpt.mesh().numFaces()},
                    {"symmetry", s->sculpt.symmetric()},
                    {"masked", masked},
                    {"frozen", frozen},
                    {"painted", s->sculpt.painted()}};
        },
    });

    ops::registerCommand({
        .name = "transferShape",
        .doc = "Move low-poly meshes onto a high-poly one (an imported sculpt): the same cage fit sculptExit uses. The first "
               "mesh is the high poly, the rest are the cages. Only positions change, so UVs and other attributes stay.",
        .positionalDoc = "source targets...",
        .flags = {{"method", "", T::String, "fit", "fit, closest or normal.", {"fit", "closest", "normal"}},
                  {"levels", "", T::Int, 2, "Subdivision levels the fit goes through.", {}, {}, 1.0, 5.0},
                  {"iterations", "", T::Int, 3, "Project-and-fit rounds.", {}, {}, 1.0, 20.0},
                  {"pinBoundary", "", T::Bool, false, "Keep border points where they are."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.size() < 2)
                throw CommandError("transferShape: give the high-poly source, then the low-poly targets");
            Graph& g = graph();
            Node& src = meshArg(objs.front(), "transferShape");
            std::string method = a.get<std::string>("method", "fit");
            if (method != "fit" && method != "closest" && method != "normal")
                throw CommandError("transferShape: method must be fit, closest or normal");
            UndoChunk chunk("Transfer Shape");
            json out = json::object();
            for (size_t i = 1; i < objs.size(); ++i)
            {
                Node& n = meshArg(objs[i], "transferShape");
                if (n.id() == src.id())
                    continue;
                geo::Mesh surface = meshInSpaceOf(src, n);
                if (surface.numFaces() == 0)
                    throw CommandError("transferShape: " + src.path() + " has no faces");
                geo::MeshBvh bvh(surface);
                PlugRef base = scene::modifiers::editPlug(g, n.id());
                geo::Mesh cage = storedMesh(base);
                FitOptions fo;
                fo.method = method == "closest" ? FitMethod::Closest : method == "normal" ? FitMethod::Normal : FitMethod::LeastSquares;
                fo.subdivide.levels = std::clamp(a.get<int>("levels"), 1, 5);
                fo.iterations = std::clamp(a.get<int>("iterations"), 1, 20);
                if (a.get<bool>("pinBoundary"))
                    fo.pinned = retopo::featurePoints(cage, {});
                std::optional<geo::SymmetryMap> map;
                if (map = scene::symmetryMapFor(n, cage); map)
                    fo.symmetry = &*map;
                geo::Mesh fitted = fitCageToSurface(cage, surface, bvh, fo);
                double maxMove = 0.0;
                for (Index p = 0; p < cage.pointCapacity(); ++p)
                    if (cage.pointValid(p))
                        maxMove = std::max(maxMove, glm::length(fitted.position(p) - cage.position(p)));
                g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(fitted))));
                out[n.path()] = maxMove;
            }
            return out;
        },
    });
}
} // namespace lila::sculpt
