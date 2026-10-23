#include "sculpt/SculptSession.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Symmetry.h"
#include "retopo/Retopo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "sculpt/Reproject.h"

#include <algorithm>
#include <atomic>
#include <map>

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
    size_t n = sizeof(Session) + s.cage.memoryBytes() + s.sculpt.mesh().memoryBytes() * 2;
    for (const geo::PointStencil& st : s.stencils)
        n += (st.offsets.capacity() + st.cols.capacity()) * sizeof(Index) + st.weights.capacity() * sizeof(double);
    return n;
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
                shared.add(s.get(), sessionBytes(*s));
    }
};

struct StrokeAction : UndoAction
{
    std::shared_ptr<Session> session;
    StrokeRecord record;
    StrokeAction(std::shared_ptr<Session> s, StrokeRecord r) : session(std::move(s)), record(std::move(r)) {}
    void undo() override
    {
        session->sculpt.apply(record, true);
        notify();
    }
    void redo() override
    {
        session->sculpt.apply(record, false);
        notify();
    }
    size_t bytes() const override
    {
        const StrokeRecord& r = record;
        return sizeof(*this) + r.points.capacity() * sizeof(Index)
            + (r.before.capacity() + r.after.capacity()) * sizeof(glm::dvec3)
            + (r.maskBefore.capacity() + r.maskAfter.capacity()) * sizeof(float) + r.frozenBefore.capacity()
            + r.frozenAfter.capacity();
    }
    void sharedBytes(UndoShared& shared) const override { shared.add(session.get(), sessionBytes(*session)); }
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

int symmetryAxis(const Node& n)
{
    int64_t axis = n.as<int64_t>(MeshNode::kSymmetry);
    return axis >= 1 && axis <= 3 ? int(axis - 1) : -1;
}

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

glm::dvec3 vec3(const json& j, const glm::dvec3& fallback = glm::dvec3(0.0))
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

BrushSettings settingsFrom(const Args& a)
{
    BrushSettings s;
    std::string brush = a.get<std::string>("brush", "draw");
    auto b = brushFromName(brush);
    if (!b)
        throw CommandError("sculptStroke: unknown brush " + brush);
    s.brush = *b;
    std::string falloff = a.get<std::string>("falloff", "smooth");
    auto f = falloffFromName(falloff);
    if (!f)
        throw CommandError("sculptStroke: unknown falloff " + falloff);
    s.falloff = *f;
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
        d.viewDir = glm::dvec3(inv * glm::dvec4(vec3(j.value("view", json())), 0.0));
        d.delta = glm::dvec3(inv * glm::dvec4(vec3(j.value("delta", json())), 0.0));
        d.direction = glm::dvec3(inv * glm::dvec4(vec3(j.value("direction", json())), 0.0));
        out.push_back(d);
    }
    return out;
}

std::atomic<uint64_t> gLiveToken{1};

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
            proxy = storedMesh({kept->id(), MeshNode::kInMesh, -1});
            // A kept subdivision proxy is recognised by its point count, provided the cage kept its topology.
            s.mode = ProxyMode::Remesh;
            for (int l = 1; l <= 6; ++l)
            {
                geo::SubdivideOptions o = s.subdivide;
                o.levels = l;
                geo::Mesh sub;
                std::vector<geo::PointStencil> st = geo::subdivideStencils(s.cage, o, &sub);
                if (sub.pointCapacity() == proxy.pointCapacity() && sub.numFaces() == proxy.numFaces())
                {
                    s.mode = ProxyMode::Subdivide;
                    s.subdivide = o;
                    s.stencils = std::move(st);
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
            s.stencils = geo::subdivideStencils(s.cage, s.subdivide, &proxy);
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

    std::vector<float> mask, freeze;
    if (auto m = std::as_const(proxy).pointAttrs().get<float>(kMaskLayer); !m.empty())
        mask.assign(m.begin(), m.end());
    if (auto f = std::as_const(proxy).pointAttrs().get<float>(kFreezeLayer); !f.empty())
        freeze.assign(f.begin(), f.end());
    s.sculpt = SculptMesh(std::move(proxy));
    if (!mask.empty())
        s.sculpt.setMask(mask);
    if (!freeze.empty())
    {
        StrokeRecord r;
        for (Index p = 0; p < freeze.size(); ++p)
            if (freeze[p] > 0.5f)
            {
                r.points.push_back(p);
                r.frozenAfter.push_back(1);
            }
        s.sculpt.apply(r, false);
    }
    (void)g;
}

/** New cage positions from the sculpt, per point capacity of the cage. */
std::vector<glm::dvec3> exitPositions(Session& s, const std::string& method, int iterations, const geo::SymmetryMap* symmetry)
{
    LILA_ZONE();

    const geo::Mesh& proxy = s.sculpt.mesh();
    if (s.mode == ProxyMode::Direct)
        return {proxy.positions().begin(), proxy.positions().end()};
    FitOptions fo;
    fo.method = method == "closest" ? FitMethod::Closest : method == "normal" ? FitMethod::Normal : FitMethod::LeastSquares;
    fo.subdivide = s.subdivide;
    fo.iterations = iterations;
    fo.symmetry = symmetry;
    if (fo.method == FitMethod::LeastSquares && s.mode == ProxyMode::Subdivide)
        return fitCage(s.cage, s.stencils, proxy.positions(), fo);
    geo::Mesh fitted = fitCageToSurface(s.cage, proxy, s.sculpt.bvh(), fo);
    return {fitted.positions().begin(), fitted.positions().end()};
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
    return m;
}
} // namespace

std::shared_ptr<Session> active() { return activeRef(); }

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
    int axis = n ? symmetryAxis(*n) : -1;
    if (axis == s.symmetryAxis)
        return;
    s.symmetryAxis = axis;
    if (axis < 0)
        s.sculpt.setSymmetry(std::nullopt, 0.0);
    else
        s.sculpt.setSymmetry(geo::axisPlane(axis), symmetryTolerance(s.sculpt.mesh()));
}

uint64_t nextLiveToken() { return gLiveToken.fetch_add(1); }

void registerCommands()
{
    using T = ArgType;

    graph().addListener([](scene::Change c, NodeId) {
        if (c == scene::Change::Cleared && activeRef())
            setActive(nullptr);
    }, true);

    ops::registerCommand({
        .name = "sculptEnter",
        .doc = "Enter sculpt mode on a mesh: builds a temporary high-resolution proxy of its cage to sculpt on. mode='subdivide' "
               "(Catmull-Clark, levels deep) reprojects exactly on exit; 'remesh' sculpts a uniform voxel remesh; 'direct' "
               "sculpts the mesh itself. Resumes from a kept proxy when there is one. Returns {mesh, mode, levels, points, faces}.",
        .positionalDoc = "mesh",
        .flags = {{"mode", "", T::String, "subdivide", "subdivide, remesh or direct.", {"subdivide", "remesh", "direct"}},
                  {"levels", "", T::Int, 2, "Subdivision levels of the proxy, 1 to 6."},
                  {"resolution", "", T::Int, 192, "Voxels along the longest side, for mode='remesh'."},
                  {"resume", "", T::Bool, true, "Continue from the mesh's kept proxy when it has one."}},
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
            UndoStack::get().record(std::make_unique<SwitchAction>(activeRef(), s));
            setActive(s);
            const char* modes[] = {"subdivide", "remesh", "direct"};
            log::info("sculptEnter {}: {} proxy, {} points, {} faces", n.path(), modes[int(s->mode)], s->sculpt.mesh().numPoints(),
                      s->sculpt.mesh().numFaces());
            return {{"mesh", n.path()},
                    {"mode", modes[int(s->mode)]},
                    {"levels", s->mode == ProxyMode::Subdivide ? s->subdivide.levels : 0},
                    {"points", s->sculpt.mesh().numPoints()},
                    {"faces", s->sculpt.mesh().numFaces()}};
        },
    });

    ops::registerCommand({
        .name = "sculptExit",
        .doc = "Leave sculpt mode, moving the low-poly cage onto the sculpt. method='fit' solves for the cage whose "
               "subdivision best matches the sculpt (least squares through the subdivision stencils); 'closest' and 'normal' "
               "project each cage point onto the sculpted surface instead. Only positions change: UVs and other attributes "
               "stay. keepProxy stores the proxy as a hidden child mesh to resume from. Returns {mesh, maxMove}.",
        .flags = {{"method", "", T::String, "fit", "fit, closest or normal.", {"fit", "closest", "normal"}},
                  {"keepProxy", "", T::Bool, false, "Keep the sculpt as a hidden child mesh."},
                  {"iterations", "", T::Int, 3, "Project-and-fit rounds for remeshed proxies."}},
        .run = [](const Args& a) -> json {
            std::shared_ptr<Session> s = requireSession("sculptExit");
            Graph& g = graph();
            Node& n = *g.node(s->node);
            std::string method = a.get<std::string>("method", "fit");
            if (method != "fit" && method != "closest" && method != "normal")
                throw CommandError("sculptExit: method must be fit, closest or normal");
            UndoChunk chunk("Exit Sculpt");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh mesh = storedMesh(base);
            if (mesh.pointCapacity() != s->cage.pointCapacity() || mesh.numFaces() != s->cage.numFaces())
                throw CommandError("sculptExit: " + n.path() + "'s topology changed since sculptEnter; undo that edit or run sculptCancel");

            std::optional<geo::SymmetryMap> map;
            geo::Mesh cageCopy = s->cage;
            if (int axis = symmetryAxis(n); axis >= 0)
                map = geo::SymmetryMap::persistent(cageCopy, axis, symmetryTolerance(cageCopy));
            std::vector<glm::dvec3> pos = exitPositions(*s, method, std::clamp(a.get<int>("iterations"), 1, 20), map ? &*map : nullptr);
            double maxMove = 0.0;
            for (Index p = 0; p < mesh.pointCapacity() && p < pos.size(); ++p)
                if (mesh.pointValid(p))
                {
                    maxMove = std::max(maxMove, glm::length(pos[p] - s->cage.position(p)));
                    mesh.setPosition(p, pos[p]);
                }
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));

            Node* kept = s->proxyNode != scene::kNoNode ? g.node(s->proxyNode) : nullptr;
            if (a.get<bool>("keepProxy"))
            {
                if (!kept)
                {
                    kept = g.createNode(*scene::types::find("mesh"), n.name() + kProxySuffix, n.id(), false);
                    g.setValue({kept->id(), Transform::kVisibility, -1}, false);
                }
                g.setValue({kept->id(), MeshNode::kInMesh, -1}, proxyToKeep(*s));
            }
            else if (kept)
                g.deleteNode(kept->id());

            UndoStack::get().record(std::make_unique<SwitchAction>(s, nullptr));
            setActive(nullptr);
            return {{"mesh", n.path()}, {"maxMove", maxMove}};
        },
    });

    ops::registerCommand({
        .name = "sculptCancel",
        .doc = "Leave sculpt mode without changing the mesh, discarding the proxy (a kept proxy stays as it was).",
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
        .doc = "One brush stroke on the sculpt proxy: dabs is a list of {center, normal, radius, pressure, view, delta, direction} "
               "in world space (only center is required). Honours the mask, frozen points and the mesh's symmetry, mirrored "
               "exactly. Returns {points} changed.",
        .flags = {{"brush", "", T::String, "draw", "Brush.", {"draw", "clayStrips", "inflate", "grab", "smooth", "flatten", "pinch", "crease", "scrape", "mask", "relax", "freeze"}},
                  {"falloff", "", T::String, "smooth", "Falloff curve.", {"smooth", "sphere", "root", "sharp", "linear", "constant"}},
                  {"strength", "", T::Float, 0.5, "0 to 1."},
                  {"invert", "", T::Bool, false, "Dig, unmask, unfreeze or magnify instead."},
                  {"frontFacesOnly", "", T::Bool, false, "Leave points facing away from the view."},
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
                  {"iterations", "", T::Int, 2, "Blur passes."}},
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
        .name = "sculptState",
        .doc = "The open sculpt session: {active, mesh, mode, levels, points, faces, symmetry, masked, frozen}.",
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
                    {"levels", s->mode == ProxyMode::Subdivide ? s->subdivide.levels : 0},
                    {"points", s->sculpt.mesh().numPoints()},
                    {"faces", s->sculpt.mesh().numFaces()},
                    {"symmetry", s->sculpt.symmetric()},
                    {"masked", masked},
                    {"frozen", frozen}};
        },
    });

    ops::registerCommand({
        .name = "transferShape",
        .doc = "Move low-poly meshes onto a high-poly one (an imported sculpt): the same cage fit sculptExit uses. The first "
               "mesh is the high poly, the rest are the cages. Only positions change, so UVs and other attributes stay.",
        .positionalDoc = "source targets...",
        .flags = {{"method", "", T::String, "fit", "fit, closest or normal.", {"fit", "closest", "normal"}},
                  {"levels", "", T::Int, 2, "Subdivision levels the fit goes through."},
                  {"iterations", "", T::Int, 3, "Project-and-fit rounds."},
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
                if (int axis = symmetryAxis(n); axis >= 0)
                {
                    map = geo::SymmetryMap::persistent(cage, axis, symmetryTolerance(cage));
                    fo.symmetry = &*map;
                }
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
