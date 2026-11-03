#include "curve/PipeToolsNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/Carve.h"
#include "curve/Channel.h"
#include "curve/EnvAttrs.h"
#include "curve/PipeRoute.h"
#include "curve/PipeSupport.h"
#include "curve/StripUvAttrs.h"
#include "curve/WorldMeshes.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <memory>

namespace lila::curve
{
using namespace scene;
using namespace env;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
const std::vector<std::string> kShapes = {"uChannel", "vGroove", "halfRound", "stepped"};
const std::vector<std::string> kSupportStyles = {"auto", "hanger", "bracket", "stand", "clamp", "mesh"};
const std::vector<std::string> kSupportTargets = {"nearest", "floor", "ceiling", "wall"};

D3 worldUp() { return math::upAxis() == math::UpAxis::Z ? D3(0, 0, 1) : D3(0, 1, 0); }

AttrDef meshArray(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrArray, .doc = std::move(doc)};
}

AttrDef matrixArray(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden | AttrArray,
            .doc = std::move(doc)};
}

AttrDef sAttr(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = kIn, .doc = std::move(doc)};
}

AttrDef out(std::string name, PlugType type, PlugValue def, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrOutput, .doc = std::move(doc)};
}

/** The meshes wired into an array attr with their matrices (same element numbers), placed in the world. */
WorldMeshes worldMeshesOf(const Node& n, const char* meshes, const char* matrices)
{
    WorldMeshes w;
    const auto& mats = n.elements(idx(n, matrices));
    for (const auto& [k, v] : n.elements(idx(n, meshes)))
    {
        const MeshPtr* m = std::get_if<MeshPtr>(&v);
        if (!m || !*m)
            continue;
        glm::dmat4 world(1.0);
        for (const auto& [mk, mv] : mats)
            if (mk == k)
                if (const glm::dmat4* mm = std::get_if<glm::dmat4>(&mv))
                    world = *mm;
        w.add(*m, world);
    }
    return w;
}

/** Wires meshes into an array attr pair, element by element from 0. */
void connectMeshes(Graph& g, Node& n, const std::vector<Node*>& meshes, const char* meshAttr, const char* matrixAttr)
{
    for (size_t k = 0; k < meshes.size(); ++k)
    {
        g.connect({meshes[k]->id(), MeshNode::kOutMesh, -1}, {n.id(), idx(n, meshAttr), -1, int(k)});
        g.connect({meshes[k]->id(), Transform::kWorldMatrix, -1}, {n.id(), idx(n, matrixAttr), -1, int(k)});
    }
}

Node& meshNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

Node& curveNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a curve");
    return *n;
}

std::vector<Node*> meshList(Graph& g, const Args& a, const char* flag, const char* cmd)
{
    std::vector<Node*> out;
    if (a.has(flag))
        for (const json& m : a.flags[flag])
            out.push_back(&meshNamed(g, m.get<std::string>(), cmd));
    return out;
}

/** A new poly curve from points=[[x, y, z], ...], or null without the flag. */
Node* curveFromPoints(Graph& g, const Args& a, const char* cmd, const std::string& name)
{
    if (!a.has("points"))
        return nullptr;
    Spline s;
    s.kind = Kind::Poly;
    for (const json& p : a.flags["points"])
    {
        if (!p.is_array() || p.size() != 3)
            throw CommandError(std::string(cmd) + ": points: expected [[x, y, z], ...]");
        s.points.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    }
    if (s.size() < 2)
        throw CommandError(std::string(cmd) + ": give at least two points");
    s.normalize();
    Node* c = g.createNode(*types::find("curve"), name, kNoNode, true);
    g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData({s}));
    return c;
}

std::vector<Node*> curvesOf(Graph& g, const Args& a, const char* cmd)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
        out.push_back(&curveNamed(g, p.get<std::string>(), cmd));
    if (out.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
    return out;
}

// Channel.

ChannelOptions channelOptionsOf(const Node& n)
{
    ChannelOptions o;
    o.shape = ChannelShape(std::clamp<int64_t>(i(n, "shape"), 0, 3));
    o.width = std::max(1e-3, f(n, "width"));
    o.depth = std::max(1e-3, f(n, "depth"));
    o.cornerRound = std::clamp(f(n, "cornerRound"), 0.0, 1.0);
    o.steps = int(std::clamp<int64_t>(i(n, "steps"), 1, 64));
    o.floorWidth = std::clamp(f(n, "floorWidth"), 0.0, 1.0);
    o.segments = int(std::clamp<int64_t>(i(n, "segments"), 1, 64));
    o.lift = std::max(0.0, f(n, "lift"));
    o.bendRadius = std::max(0.0, f(n, "bendRadius"));
    o.up = v(n, "up");
    o.uv = stripUvOf(n, StripUv{.layout = UvLayout::Along, .perCm = 0.01});
    return o;
}

/** The path dropped onto the surface along -up, every step cm, so a curve drawn in plan follows the ground. */
Spline dropOnto(const Spline& path, const WorldMeshes& surface, const D3& up, double step)
{
    TessOptions tess;
    tess.mode = TessMode::Adaptive;
    Polyline pl = tessellate(path, tess);
    if (pl.size() < 2)
        return path;
    std::vector<D3> pts;
    const int n = std::max(1, int(std::ceil(pl.length / std::max(step, 1e-3))));
    const geo::Bounds b = surface.bounds();
    const double reach = glm::length(b.size()) + 1e3;
    for (int k = 0; k <= n; ++k)
    {
        if (pl.closed && k == n)
            break;
        const D3 p = pl.pointAtLength(pl.length * k / n);
        const double h = std::max(glm::dot(b.max - p, up), 0.0) + 1.0;
        std::optional<SurfacePoint> hit = surface.raycast(p + up * h, -up, h + reach);
        pts.push_back(hit ? hit->position : p);
    }
    Spline s;
    s.kind = Kind::Poly;
    s.points = std::move(pts);
    s.closed = pl.closed;
    s.normalize();
    return s;
}

geo::Mesh channelRun(const geo::Mesh& in, const Node& self)
{
    LILA_ZONE();

    ChannelOptions o = channelOptionsOf(self);
    const glm::dmat4 world = self.as<glm::dmat4>(idx(self, "targetMatrix"));
    CarveOptions co;
    co.up = glm::length(o.up) > 1e-9 ? glm::normalize(o.up) : D3(0, 1, 0);
    co.material = self.as<std::string>(idx(self, "material"));
    co.hardAngle = f(self, "hardAngle");
    WorldMeshes ground;
    const bool follow = b(self, "followSurface");
    if (follow)
        ground.add(std::shared_ptr<const geo::Mesh>(&in, [](const geo::Mesh*) {}), world);
    geo::Mesh mesh = in;
    std::string problems;
    for (const Spline& s : splinesOf(self.value(idx(self, "path"))))
    {
        const Spline path = follow ? dropOnto(s, ground, co.up, std::max(o.width * 0.5, 1.0)) : s;
        geo::Mesh cutter = channelCutter(path, o);
        if (cutter.numFaces() == 0)
            continue;
        CarveResult r = carve(mesh, world, cutter, co);
        if (!r.ok())
            throw std::runtime_error(r.error);
        if (r.unwelded > 0)
            problems = std::to_string(r.unwelded) + " border points of the cut didn't weld back";
        mesh = std::move(r.mesh);
    }
    if (!problems.empty())
        log::warn("{}: {}", self.name(), problems);
    return mesh;
}

void registerChannelModifier()
{
    if (types::find("channelModifier"))
        return;
    NodeType t;
    t.name = "channelModifier";
    t.icon = "route";
    t.doc = "A channel carved along curves, kept live: a U channel, V groove, half round or stepped profile swept along the path "
            "curves and cut into this mesh, open surfaces such as terrain included. Moving the curve or editing the profile recuts it.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        bAttr("enabled", true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = kIn, .doc = "Apply the modifier when exporting."},
        dataIn("path", "Path curves, world space."),
        {.name = "targetMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = kIn | AttrHidden,
         .doc = "This mesh's world matrix when the channel was made."},
        eAttr("shape", kShapes, 0, "Cross-section: U channel, V groove, half round or stepped terraces."),
        fAttr("width", 100.0, "Width at the surface, cm.", 0.0),
        fAttr("depth", 50.0, "Depth below the path, cm.", 0.0),
        fAttr("cornerRound", 0.25, "U channel: bottom corner radius, as a fraction of half the width.", 0.0, 1.0),
        iAttr("steps", 3, "Stepped: terraces from the rim to the floor.", 1, 64.0),
        fAttr("floorWidth", 0.3, "Stepped: floor width, as a fraction of the width.", 0.0, 1.0),
        iAttr("segments", 8, "Points on each rounded part of the profile.", 1, 64.0),
        fAttr("lift", 0.0, "How far the cutter rises above the path, cm; 0 is the larger of the depth and half the width.", 0.0),
        fAttr("bendRadius", 0.0, "Rounds the path's sharp corners, cm; 0 mitres them.", 0.0),
        bAttr("followSurface", true, "Drop the path onto this mesh along up, so a curve drawn in plan follows the ground."),
        vAttr("up", D3(0, 1, 0), "Up at the path: the channel goes down from it, and only faces turned this way are cut."),
        sAttr("material", "Material slot for the channel's walls and floor; empty takes the mesh's first."),
        fAttr("hardAngle", 30.0, "New edges sharper than this many degrees are marked hard.", 0.0, 180.0),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    const AttrDef outMesh = t.ownAttrs.back();
    t.ownAttrs.pop_back();
    for (AttrDef& a : stripUvAttrs(StripUv{.layout = UvLayout::Along, .perCm = 0.01}, true, "UVs"))
        t.ownAttrs.push_back(std::move(a));
    t.ownAttrs.push_back(outMesh);
    const int outAttr = int(t.ownAttrs.size()) - 1;
    t.create = [outAttr] {
        auto n = std::make_unique<ModifierNode>();
        n->outAttr = outAttr;
        n->apply = channelRun;
        return n;
    };
    types::registerType(std::move(t));
}

// Routing.

class PipeRouteNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int outCurve = idx(*this, "outCurve");
        try
        {
            PipeRouteOptions o;
            o.clearance = std::max(0.0, f(*this, "clearance"));
            o.cellSize = std::max(0.0, f(*this, "cellSize"));
            o.axisCost = glm::max(v(*this, "axisCost"), D3(1e-3));
            o.bendCost = std::max(0.0, f(*this, "bendCost"));
            o.startDir = v(*this, "startDir");
            o.endDir = v(*this, "endDir");
            o.margin = std::max(0.0, f(*this, "margin"));
            WorldMeshes obstacles = worldMeshesOf(*this, "obstacles", "obstacleMatrices");
            ObstacleDistance dist;
            if (!obstacles.empty())
                dist = [&obstacles](const D3& p, double maxDistance) { return obstacles.signedDistance(p, maxDistance); };
            PipeRouteResult r = routePipe(v(*this, "start"), v(*this, "end"), dist, o);
            if (!r.warning.empty() && r.warning != as<std::string>(idx(*this, "warning")))
                log::warn("{}: {}", name(), r.warning);
            setOutput(idx(*this, "warning"), r.warning);
            setOutput(idx(*this, "found"), r.found);
            setOutput(idx(*this, "bends"), int64_t(r.bends));
            setOutput(idx(*this, "length"), r.length);
            setOutput(outCurve, makeCurveData({routeSpline(r)}));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(outCurve, DataPtr());
        }
    }
};

void registerRouteType()
{
    if (types::find("curvePipeRoute"))
        return;
    NodeType t;
    t.name = "curvePipeRoute";
    t.icon = "route";
    t.doc = "An orthogonal pipe run from start to end around the obstacle meshes, with clearance, preferred axes and few bends. "
            "Its curve feeds a pipe, which rounds the corners into bends.";
    t.ownAttrs = {
        vAttr("start", D3(0.0), "Where the run starts, world cm."),
        vAttr("end", D3(100.0, 0.0, 0.0), "Where the run ends, world cm."),
        vAttr("startDir", D3(0.0), "The run leaves the start along this axis (zero: any)."),
        vAttr("endDir", D3(0.0), "The run arrives at the end moving along this axis (zero: any)."),
        fAttr("clearance", 10.0, "Least distance from the centreline to any obstacle, cm: the pipe radius plus a gap.", 0.0),
        fAttr("cellSize", 0.0, "Grid step, cm; 0 picks one from the clearance and the distance.", 0.0),
        vAttr("axisCost", D3(1.0), "Cost per cm along x, y and z: raise one to keep runs off that axis."),
        fAttr("bendCost", 100.0, "Cost of each bend, in cm of run.", 0.0),
        fAttr("margin", 0.0, "How far the search reaches past the box around the ends, cm; 0 is a quarter of the distance.", 0.0),
        meshArray("obstacles", "Meshes the run keeps clear of."),
        matrixArray("obstacleMatrices", "The obstacles' world matrices."),
        dataOut("outCurve", "The run: a poly curve through its corners."),
        out("found", PlugType::Bool, false, "A route was found; otherwise the curve is a straight line."),
        out("bends", PlugType::Int, int64_t(0), "Corners along the run."),
        out("length", PlugType::Float, 0.0, "Run length, cm."),
        stringOut("warning", "Why no route was found."),
    };
    t.create = [] { return std::make_unique<PipeRouteNode>(); };
    types::registerType(std::move(t));
}

// Supports.

class PipeSupportsNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int outMesh = idx(*this, "outMesh");
        try
        {
            SupportOptions o;
            o.style = SupportStyle(std::clamp<int64_t>(i(*this, "style"), 0, 5));
            o.target = SupportTarget(std::clamp<int64_t>(i(*this, "target"), 0, 3));
            o.spacing = std::max(1.0, f(*this, "spacing"));
            o.startOffset = f(*this, "startOffset");
            o.endClearance = std::max(0.0, f(*this, "endClearance"));
            o.pipeRadius = std::max(1e-3, f(*this, "pipeRadius"));
            o.bendRadius = std::max(0.0, f(*this, "bendRadius"));
            o.maxReach = std::max(0.0, f(*this, "maxReach"));
            o.ringThickness = std::max(0.0, f(*this, "ringThickness"));
            o.ringWidth = std::max(0.0, f(*this, "ringWidth"));
            o.rodSize = std::max(0.0, f(*this, "rodSize"));
            o.sides = int(std::clamp<int64_t>(i(*this, "sides"), 3, 64));
            o.up = v(*this, "up");
            o.meshScale = f(*this, "meshScale");
            o.stretch = b(*this, "stretch");
            const MeshPtr& custom = as<MeshPtr>(idx(*this, "supportMesh"));
            o.mesh = custom.get();
            WorldMeshes surfaces = worldMeshesOf(*this, "surfaces", "surfaceMatrices");
            o.cast = [&surfaces](const D3& origin, const D3& dir, double maxDistance) { return surfaces.raycast(origin, dir, maxDistance); };
            geo::Mesh mesh;
            int64_t count = 0;
            std::string warning;
            for (const Spline& s : splinesOf(value(idx(*this, "path"))))
            {
                SupportResult r = makeSupports(s, o);
                mesh.append(r.mesh);
                count += int64_t(r.placements.size());
                for (const std::string& w : r.warnings)
                    warning += (warning.empty() ? "" : "; ") + w;
            }
            if (surfaces.empty())
                warning = "no surfaces connected to reach to";
            if (!warning.empty() && warning != as<std::string>(idx(*this, "warning")))
                log::warn("{}: {}", name(), warning);
            setOutput(idx(*this, "warning"), warning);
            setOutput(idx(*this, "count"), count);
            setOutput(outMesh, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(outMesh, MeshPtr());
        }
    }
};

void registerSupportsType()
{
    if (types::find("curvePipeSupports"))
        return;
    NodeType t;
    t.name = "curvePipeSupports";
    t.icon = "route";
    t.doc = "Supports along a pipe run: hangers to a ceiling, brackets to a wall, stands to a floor, clamps or your own mesh, "
            "spaced along the run and each reaching to the nearest surface found by casting rays.";
    t.ownAttrs = {
        dataIn("path", "The run's path curves, world space (the pipe's own path)."),
        meshArray("surfaces", "Meshes supports reach to: walls, floors, ceilings."),
        matrixArray("surfaceMatrices", "The surfaces' world matrices."),
        meshIn("supportMesh", "A mesh to place as each support (style mesh): +y toward the surface, +z along the pipe."),
        eAttr("style", kSupportStyles, 0, "auto picks a hanger, bracket or stand by the direction it reaches; or one style for all."),
        eAttr("target", kSupportTargets, 0, "Which surface to reach for: the nearest, or only floors, ceilings or walls."),
        fAttr("spacing", 200.0, "Distance between supports along the run, cm.", 1.0),
        fAttr("startOffset", -1.0, "First support's distance from the start, cm; negative is half the spacing."),
        fAttr("endClearance", 0.0, "Keep supports this far from the ends and off bends, cm; 0 is twice the pipe radius.", 0.0),
        fAttr("pipeRadius", 5.0, "Radius of the pipe held, cm.", 0.0),
        fAttr("bendRadius", 0.0, "Bend radius the pipe rounds corners with, cm; 0 is twice the radius.", 0.0),
        fAttr("maxReach", 500.0, "Farthest a support reaches, cm; stations farther from any surface get none.", 0.0),
        fAttr("ringThickness", 0.0, "Clamp ring thickness, cm; 0 sizes it from the pipe.", 0.0),
        fAttr("ringWidth", 0.0, "Clamp ring width along the pipe, cm; 0 sizes it from the pipe.", 0.0),
        fAttr("rodSize", 0.0, "Rod, arm or post thickness, cm; 0 sizes it from the pipe.", 0.0),
        iAttr("sides", 12, "Sides around the rings.", 3, 64.0),
        vAttr("up", D3(0, 1, 0), "World up: floors are below, ceilings above."),
        fAttr("meshScale", 1.0, "Scale of the support mesh.", 0.0),
        bAttr("stretch", true, "Stretch the support mesh along its +y to reach the surface."),
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "The supports."},
        out("count", PlugType::Int, int64_t(0), "Supports placed."),
        stringOut("warning", "Stations that found no surface."),
    };
    t.create = [] { return std::make_unique<PipeSupportsNode>(); };
    types::registerType(std::move(t));
}

/** The pipe node building a mesh (through any modifiers), or null. */
Node* pipeFeeding(Graph& g, Node* n)
{
    for (int depth = 0; n && depth < 16; ++depth)
    {
        if (n->type().name == "curvePipe")
            return n;
        const ModifierNode* mod = modifiers::asModifier(n);
        const int in = n->type().isA("mesh") ? MeshNode::kInMesh : mod ? mod->inAttr : -1;
        if (in < 0)
            return nullptr;
        std::optional<PlugRef> src = g.source({n->id(), in, -1});
        n = src ? g.node(src->node) : nullptr;
    }
    return nullptr;
}

json withArgs(const json& options, const std::string& arg)
{
    json call = options.is_object() ? options : json::object();
    call["args"] = json::array({arg});
    return call;
}
} // namespace

void registerPipeToolTypes()
{
    registerChannelModifier();
    registerRouteType();
    registerSupportsType();
}

void registerPipeToolCommands()
{
    {
        std::vector<FlagDef> flags = attrFlags("channelModifier");
        std::erase_if(flags, [](const FlagDef& fd) { return fd.name == "enabled" || fd.name == "exportEnabled"; });
        for (FlagDef fd : std::vector<FlagDef>{
                 {"surface", "", T::String, nullptr, "Mesh to carve (default: the selected mesh)."},
                 {"points", "p", T::List, nullptr, "[[x, y, z], ...] world points of a new path curve, instead of path curves."},
                 {"constructionHistory", "ch", T::Bool, true, "Keep the channel live; off applies it."}})
            flags.push_back(std::move(fd));
        ops::registerCommand({
            .name = "carveChannel",
            .doc = "Carve a channel into a mesh along path curves: a U channel, V groove, half round or stepped profile swept along "
                   "the path and cut out of the mesh, terrain and other open surfaces included, at any scale. Live: a "
                   "channelModifier on the mesh's stack recuts as the curve or settings change. The walls take the strip UVs "
                   "(uvLayout along by default, tiling every 1 / texelDensity cm). Returns [mesh, channelModifier, path curve].",
            .positionalDoc = "path curves (default: the selected curves)",
            .flags = flags,
            .run = [](const Args& a) -> json {
                Graph& g = Graph::current();
                g.evaluate();
                Node* surface = nullptr;
                if (a.has("surface"))
                    surface = &meshNamed(g, a.get<std::string>("surface"), "carveChannel");
                else
                    for (NodeId id : g.selection())
                        if (Node* n = g.node(id); n && n->type().isA("mesh"))
                        {
                            surface = n;
                            break;
                        }
                if (!surface)
                    throw CommandError("carveChannel: select a mesh to carve, or give surface=");
                std::vector<Node*> paths;
                if (Node* made = curveFromPoints(g, a, "carveChannel", "ChannelPath"))
                    paths.push_back(made);
                else
                    paths = curvesOf(g, a, "carveChannel");
                if (paths.empty())
                    throw CommandError("carveChannel: select path curves, or give points=");
                json out = json::array();
                for (Node* path : paths)
                {
                    json mod = ops::execute("addModifier", {{"args", {surface->path()}}, {"kind", "channel"}});
                    Node* op = g.find(mod.get<std::string>());
                    if (!a.has("up"))
                        g.setValue({op->id(), idx(*op, "up"), -1}, worldUp());
                    applyFlags(g, *op, a, "carveChannel");
                    g.setValue({op->id(), idx(*op, "targetMatrix"), -1}, surface->as<glm::dmat4>(Transform::kWorldMatrix));
                    g.connect({path->id(), CurveShape::kWorldSpace, -1}, {op->id(), idx(*op, "path"), -1});
                    g.evaluate();
                    if (const auto* m = modifiers::asModifier(op); m && !m->error().empty())
                        throw CommandError("carveChannel: " + m->error());
                    json opName = op->path();
                    if (!a.get<bool>("constructionHistory", true))
                    {
                        ops::execute("applyModifier", {{"args", {op->path()}}});
                        opName = nullptr;
                    }
                    out.push_back(json::array({surface->path(), opName, path->path()}));
                }
                g.select({surface->id()});
                return out.size() == 1 ? out[0] : out;
            },
        });
    }

    {
        std::vector<FlagDef> flags = attrFlags("curvePipeRoute");
        for (FlagDef fd : std::vector<FlagDef>{
                 {"obstacles", "", T::List, nullptr, "Meshes to keep clear of (default: the selected meshes)."},
                 {"pipe", "", T::Bool, false, "Also build a live pipe along the run."},
                 {"pipeOptions", "", T::Any, nullptr, "{flag: value} for the pipe command (radius, preset, uvLayout ...)."},
                 {"name", "n", T::String, nullptr, "Route curve name; made unique."}})
            flags.push_back(std::move(fd));
        ops::registerCommand({
            .name = "pipeRoute",
            .doc = "Route a pipe from start to end around obstacle meshes: orthogonal runs on a grid with clearance, preferred axes "
                   "(axisCost) and few bends (bendCost). Live: moving the obstacles or the ends reroutes, and a pipe on the route "
                   "follows. Returns [route curve, curvePipeRoute, pipe mesh or None].",
            .flags = flags,
            .run = [](const Args& a) -> json {
                Graph& g = Graph::current();
                if (!a.has("start") || !a.has("end"))
                    throw CommandError("pipeRoute: give start= and end=");
                std::vector<Node*> obstacles = meshList(g, a, "obstacles", "pipeRoute");
                if (!a.has("obstacles"))
                    for (NodeId id : g.selection())
                        if (Node* n = g.node(id); n && n->type().isA("mesh"))
                            obstacles.push_back(n);
                Node* op = g.createNode(*types::find("curvePipeRoute"), {}, kNoNode, true);
                if (!a.has("clearance") && a.has("pipeOptions") && a.flags["pipeOptions"].contains("radius"))
                    g.setValue({op->id(), idx(*op, "clearance"), -1}, a.flags["pipeOptions"]["radius"].get<double>() * 1.5);
                applyFlags(g, *op, a, "pipeRoute");
                connectMeshes(g, *op, obstacles, "obstacles", "obstacleMatrices");
                Node* curve = g.createNode(*types::find("curve"), a.get<std::string>("name", "PipeRoute"), kNoNode, true);
                g.connect({op->id(), idx(*op, "outCurve"), -1}, {curve->id(), CurveShape::kCreate, -1});
                g.evaluate();
                if (!op->as<bool>(idx(*op, "found")))
                    log::warn("pipeRoute: {}", op->as<std::string>(idx(*op, "warning")));
                json pipeMesh = nullptr;
                if (a.get<bool>("pipe", false))
                {
                    json r = ops::execute("pipe", withArgs(a.has("pipeOptions") ? a.flags["pipeOptions"] : json(), curve->path()));
                    if (r.is_array() && !r.empty())
                        pipeMesh = r[0];
                }
                g.select({curve->id()});
                return json::array({curve->path(), op->path(), pipeMesh});
            },
        });
    }

    {
        std::vector<FlagDef> flags = attrFlags("curvePipeSupports");
        for (FlagDef fd : std::vector<FlagDef>{
                 {"surfaces", "", T::List, nullptr, "Meshes supports reach to (default: the selected meshes that aren't the pipe)."},
                 {"supportMeshNode", "", T::String, nullptr, "Mesh to place as each support (sets style = mesh)."},
                 {"name", "n", T::String, nullptr, "Supports mesh name; made unique."}})
            flags.push_back(std::move(fd));
        ops::registerCommand({
            .name = "pipeSupports",
            .doc = "Place supports along pipes: hangers, wall brackets, floor stands, clamps or your own mesh, every spacing cm, "
                   "each reaching to the nearest wall, floor or ceiling by ray cast. Given a pipe mesh, the supports follow its "
                   "path, radius and bends live. Returns [supports mesh, curvePipeSupports] per pipe or path curve.",
            .positionalDoc = "pipe meshes or path curves (default: the selection)",
            .flags = flags,
            .run = [](const Args& a) -> json {
                Graph& g = Graph::current();
                std::vector<Node*> given;
                for (const json& p : a.positional)
                {
                    Node* n = g.find(p.get<std::string>());
                    if (!n)
                        throw CommandError("pipeSupports: no node named '" + p.get<std::string>() + "'");
                    given.push_back(n);
                }
                const bool fromSelection = given.empty();
                if (fromSelection)
                    for (NodeId id : g.selection())
                        if (Node* n = g.node(id); n && (n->type().isA("curve") || pipeFeeding(g, n)))
                            given.push_back(n);
                std::vector<Node*> surfaces = meshList(g, a, "surfaces", "pipeSupports");
                if (!a.has("surfaces"))
                    for (NodeId id : g.selection())
                        if (Node* n = g.node(id); n && n->type().isA("mesh") && !pipeFeeding(g, n))
                            surfaces.push_back(n);
                if (given.empty())
                    throw CommandError("pipeSupports: select pipe meshes or path curves");
                if (surfaces.empty())
                    throw CommandError("pipeSupports: give surfaces= (the walls, floors and ceilings to reach to)");
                json out = json::array();
                std::vector<NodeId> made;
                for (Node* n : given)
                {
                    Node* op = g.createNode(*types::find("curvePipeSupports"), {}, kNoNode, true);
                    if (!a.has("up"))
                        g.setValue({op->id(), idx(*op, "up"), -1}, worldUp());
                    if (a.has("supportMeshNode"))
                    {
                        g.connect({meshNamed(g, a.get<std::string>("supportMeshNode"), "pipeSupports").id(), MeshNode::kOutMesh, -1},
                                  {op->id(), idx(*op, "supportMesh"), -1});
                        g.setValue({op->id(), idx(*op, "style"), -1}, int64_t(5));
                    }
                    applyFlags(g, *op, a, "pipeSupports");
                    if (Node* pipe = pipeFeeding(g, n))
                    {
                        std::optional<PlugRef> path = g.source({pipe->id(), idx(*pipe, "path"), -1});
                        if (!path)
                            throw CommandError("pipeSupports: " + n->path() + " has no path curve");
                        g.connect(*path, {op->id(), idx(*op, "path"), -1});
                        if (!a.has("pipeRadius"))
                            g.connect({pipe->id(), idx(*pipe, "radius"), -1}, {op->id(), idx(*op, "pipeRadius"), -1});
                        if (!a.has("bendRadius"))
                            g.connect({pipe->id(), idx(*pipe, "bendRadius"), -1}, {op->id(), idx(*op, "bendRadius"), -1});
                    }
                    else if (n->type().isA("curve"))
                        g.connect({n->id(), CurveShape::kWorldSpace, -1}, {op->id(), idx(*op, "path"), -1});
                    else
                        throw CommandError("pipeSupports: " + n->path() + " is neither a pipe nor a curve");
                    connectMeshes(g, *op, surfaces, "surfaces", "surfaceMatrices");
                    Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "PipeSupports"), kNoNode, true);
                    g.connect({op->id(), idx(*op, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
                    out.push_back(json::array({mesh->path(), op->path()}));
                    made.push_back(mesh->id());
                }
                g.select(made);
                return out.size() == 1 ? out[0] : out;
            },
        });
    }

    namespace opt = ops::options;
    opt::Override ch;
    ch.order = {"shape", "width", "depth", "cornerRound", "steps", "floorWidth", "followSurface"};
    ch.hidden = {"points", "surface"};
    ch.ranges["width"] = {0.0, 1e7, 10.0, " cm"};
    ch.ranges["depth"] = {0.0, 1e7, 5.0, " cm"};
    opt::setOverride("carveChannel", std::move(ch));

    opt::Override rt;
    rt.order = {"clearance", "cellSize", "bendCost", "axisCost", "pipe"};
    rt.hidden = {"start", "end", "startDir", "endDir", "obstacles", "pipeOptions", "name"};
    rt.ranges["clearance"] = {0.0, 1e5, 1.0, " cm"};
    rt.ranges["cellSize"] = {0.0, 1e5, 1.0, " cm"};
    rt.ranges["margin"] = {0.0, 1e7, 10.0, " cm"};
    opt::setOverride("pipeRoute", std::move(rt));

    opt::Override sp;
    sp.order = {"style", "target", "spacing", "maxReach"};
    sp.hidden = {"surfaces", "supportMeshNode", "name", "pipeRadius", "bendRadius"};
    sp.ranges["spacing"] = {1.0, 1e6, 10.0, " cm"};
    sp.ranges["maxReach"] = {0.0, 1e6, 10.0, " cm"};
    opt::setOverride("pipeSupports", std::move(sp));
}
} // namespace lila::curve
