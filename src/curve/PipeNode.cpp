#include "curve/PipeNode.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "geo/Conform.h"
#include "geo/Ops.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
constexpr uint32_t kIn = AttrInput | AttrStorable;
constexpr uint32_t kKey = AttrInput | AttrStorable | AttrKeyable;

const std::vector<std::string> kCaps = {"none", "ngon", "grid", "round"};
const std::vector<std::string> kCouplings = {"none", "flange", "collar", "clamp", "elbowCollar", "mesh"};
const std::vector<std::string> kFasteners = {"none", "boltAndNut", "bolt", "nut", "mesh"};
const std::vector<std::string> kPresets = {"pipe", "hose", "cable", "conduit"};

AttrDef fAttr(std::string name, double def, std::string doc, std::string cat, std::optional<double> lo = std::nullopt, std::optional<double> hi = std::nullopt)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.maxValue = hi;
    a.category = std::move(cat);
    return a;
}

AttrDef iAttr(std::string name, int64_t def, std::string doc, std::string cat, double lo = 0.0)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Int, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.minValue = lo;
    a.category = std::move(cat);
    return a;
}

AttrDef bAttr(std::string name, bool def, std::string doc, std::string cat)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Bool, .defaultValue = def, .flags = kKey, .doc = std::move(doc)};
    a.category = std::move(cat);
    return a;
}

AttrDef eAttr(std::string name, std::vector<std::string> names, int64_t def, std::string doc, std::string cat)
{
    AttrDef a{.name = std::move(name), .type = PlugType::Enum, .defaultValue = def, .flags = kIn, .enumNames = std::move(names), .doc = std::move(doc)};
    a.category = std::move(cat);
    return a;
}

AttrDef sAttr(std::string name, std::string doc, std::string cat, uint32_t extra = 0)
{
    AttrDef a{.name = std::move(name), .type = PlugType::String, .defaultValue = std::string(), .flags = kIn | extra, .doc = std::move(doc)};
    a.category = std::move(cat);
    return a;
}

AttrDef meshIn(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = std::move(doc)};
}

int idx(const Node& n, const char* a) { return n.type().attrIndex(a); }
double f(const Node& n, const char* a) { return n.as<double>(idx(n, a)); }
int64_t i(const Node& n, const char* a) { return n.as<int64_t>(idx(n, a)); }
bool b(const Node& n, const char* a) { return n.as<bool>(idx(n, a)); }
const std::string& s(const Node& n, const char* a) { return n.as<std::string>(idx(n, a)); }

std::vector<D3> loopFromJson(const std::string& text)
{
    std::vector<D3> out;
    if (text.empty())
        return out;
    json j = json::parse(text, nullptr, false);
    if (!j.is_array())
        return out;
    for (const json& p : j)
        if (p.is_array() && p.size() == 3)
            out.emplace_back(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
    return out;
}

std::string loopToJson(const std::vector<D3>& pts)
{
    json a = json::array();
    for (const D3& p : pts)
        a.push_back({p.x, p.y, p.z});
    return a.dump();
}

class PipeNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        int out = idx(*this, "outMesh");
        try
        {
            PipeOptions o = pipeOptionsOf(*this);
            geo::Mesh mesh;
            std::string warning;
            int64_t parts = 0;
            for (const Spline& sp : splinesOf(value(idx(*this, "path"))))
            {
                PipeResult r = makePipe(sp, o);
                mesh.append(r.mesh);
                if (!r.mesh.materialNames().empty())
                    mesh.materialNames() = r.mesh.materialNames();
                parts += int64_t(r.placements.size());
                for (const std::string& w : r.warnings)
                    warning += (warning.empty() ? "" : "; ") + w;
                // Only the first path starts on the loop.
                o.startLoop.clear();
            }
            setOutput(out, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
            setOutput(idx(*this, "partCount"), parts);
            if (warning != as<std::string>(idx(*this, "warning")) && !warning.empty())
                log::warn("{}: {}", name(), warning);
            setOutput(idx(*this, "warning"), warning);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, MeshPtr());
        }
    }
};

/** Option flags for every editable attr of the pipe node (enums by name), with no defaults so only given ones apply. */
std::vector<FlagDef> attrFlags()
{
    std::vector<FlagDef> out;
    for (const AttrDef& d : types::find("curvePipe")->ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden))
            continue;
        FlagDef fd{d.name, "", T::Float, nullptr, d.doc};
        switch (d.type)
        {
        case PlugType::Bool: fd.type = T::Bool; break;
        case PlugType::Int: fd.type = T::Int; break;
        case PlugType::String: fd.type = T::String; break;
        case PlugType::Enum:
            fd.type = T::String;
            fd.choices = d.enumNames;
            break;
        default: break;
        }
        out.push_back(std::move(fd));
    }
    return out;
}

void applyValues(Graph& g, Node& n, const json& values, const char* cmd)
{
    for (const AttrDef& d : n.type().ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden) || !values.contains(d.name) || values[d.name].is_null())
            continue;
        int a = n.type().attrIndex(d.name);
        const json& v = values[d.name];
        if (d.type == PlugType::Enum && v.is_string())
        {
            auto it = std::ranges::find(d.enumNames, v.get<std::string>());
            if (it == d.enumNames.end())
                throw CommandError(std::string(cmd) + ": " + d.name + ": unknown value '" + v.get<std::string>() + "'");
            g.setValue({n.id(), a, -1}, int64_t(it - d.enumNames.begin()));
            continue;
        }
        try
        {
            g.setValue({n.id(), a, -1}, fromJson(d, v));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + d.name + ": " + e.what());
        }
    }
}

/** Every attr a preset touches, at its node default, so switching presets leaves nothing of the last one. */
json presetBase()
{
    json j = json::object();
    const std::vector<std::string> keys = {"radius",      "sides",      "wallThickness", "startCap",      "endCap",         "bendRadius",
                                           "splitEvery",  "splitAtBends", "partsAtEnds", "coupling",      "couplingScale",  "couplingLength",
                                           "fasteners",   "boltCount",  "corrugationDepth", "corrugationPeriod", "radiusRamp"};
    for (const AttrDef& d : types::find("curvePipe")->ownAttrs)
        if (std::ranges::find(keys, d.name) != keys.end())
            j[d.name] = d.type == PlugType::Enum ? json(d.enumNames[size_t(std::get<int64_t>(d.defaultValue))]) : toJson(d.defaultValue);
    return j;
}

Node& curveNamed(Graph& g, const std::string& name)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("curve"))
        throw CommandError("pipe: " + name + " is not a curve");
    return *n;
}

Node& meshNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** A selected or given loop on a mesh: its points (ordered), the faces inside it, and the mesh. */
struct StartLoop
{
    Node* mesh = nullptr;
    std::vector<geo::Index> points;
    std::vector<geo::Index> faces;
    std::vector<D3> world;
};

std::optional<StartLoop> startLoopOf(Graph& g, const Args& a)
{
    Node* node = nullptr;
    ComponentMode kind = ComponentMode::Face;
    components::Ids ids;
    if (a.has("loopMesh"))
    {
        node = &meshNamed(g, a.get<std::string>("loopMesh"), "pipe");
        if (a.has("loopFaces"))
            ids = a.flags["loopFaces"].get<components::Ids>();
        else if (a.has("loopEdges"))
        {
            ids = a.flags["loopEdges"].get<components::Ids>();
            kind = ComponentMode::Edge;
        }
    }
    else if (a.get<bool>("fromLoop"))
        for (ComponentMode m : {ComponentMode::Face, ComponentMode::Edge, ComponentMode::Vertex})
        {
            for (auto& [id, sel] : components::selected(m))
                if (Node* n = g.node(id); n && n->type().isA("mesh") && !sel.empty())
                {
                    node = n;
                    ids = sel;
                    kind = m;
                    break;
                }
            if (node)
                break;
        }
    if (!node)
    {
        if (a.get<bool>("fromLoop") || a.has("loopMesh"))
            throw CommandError("pipe: select a face, or a closed edge loop, to start from");
        return std::nullopt;
    }
    MeshPtr cage = modifiers::cage(g, node->id());
    if (!cage || ids.empty())
        throw CommandError("pipe: nothing selected on " + node->path() + " to start from");
    geo::ShapeSelection sel = kind == ComponentMode::Face   ? geo::shapeOfFaces(*cage, ids)
                              : kind == ComponentMode::Edge ? geo::shapeOfEdges(*cage, ids)
                                                            : geo::shapeOfPoints(*cage, ids);
    auto loop = std::ranges::find_if(sel.chains, [](const geo::EdgePath& p) { return p.closed && p.points.size() >= 3; });
    if (loop == sel.chains.end())
        throw CommandError("pipe: the selection on " + node->path() + " has no closed loop to start from");
    StartLoop out;
    out.mesh = node;
    out.points = loop->points;
    if (kind == ComponentMode::Face)
        out.faces = ids;
    glm::dmat4 w = node->as<glm::dmat4>(Transform::kWorldMatrix);
    for (geo::Index p : out.points)
        out.world.push_back(D3(w * glm::dvec4(cage->position(p), 1.0)));
    return out;
}

/** The pipe node feeding a mesh (through anything between), or the node itself. */
Node* pipeNodeOf(Graph& g, Node* n)
{
    for (int depth = 0; n && depth < 16; ++depth)
    {
        if (n->type().name == "curvePipe")
            return n;
        if (!n->type().isA("mesh") && depth > 0)
            return nullptr;
        int in = n->type().isA("mesh") ? MeshNode::kInMesh : n->type().attrIndex("inMesh");
        if (in < 0)
            return nullptr;
        std::optional<PlugRef> src = g.source({n->id(), in, -1});
        n = src ? g.node(src->node) : nullptr;
    }
    return nullptr;
}

/** Bakes the pipe into the loop's mesh: the loop's faces go, the first ring welds onto the loop. Returns the mesh. */
void attachToMesh(Graph& g, const StartLoop& loop, const Spline& worldPath, const PipeOptions& o)
{
    LILA_ZONE();

    PipeResult r = makePipe(worldPath, o);
    if (r.startRing.size() != loop.points.size())
        throw CommandError("pipe: the pipe could not start on the loop");
    Node& n = *loop.mesh;
    PlugRef base = modifiers::editPlug(g, n.id());
    g.evaluate();
    const MeshPtr& stored = std::get<MeshPtr>(g.storedValue(base));
    geo::Mesh mesh = stored ? *stored : geo::Mesh();
    glm::dmat4 toLocal = glm::inverse(n.as<glm::dmat4>(Transform::kWorldMatrix));
    for (D3& p : r.mesh.positions())
        p = D3(toLocal * glm::dvec4(p, 1.0));
    if (!loop.faces.empty())
        geo::deleteFaces(mesh, loop.faces);
    geo::Index off = mesh.append(r.mesh);
    std::vector<geo::WeldPair> pairs;
    for (size_t j = 0; j < loop.points.size(); ++j)
        pairs.push_back({r.startRing[j] + off, loop.points[j]});
    geo::targetWeld(mesh, pairs);
    mesh.compact();
    for (ComponentMode m : {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face})
        components::select(n.id(), m, {}, components::Op::Replace);
    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
}

void bake(Graph& g, Node& op, const PlugRef& dst)
{
    g.evaluate();
    PlugValue v = g.value(dst);
    g.disconnect(dst);
    g.setValue(dst, v);
    g.deleteNode(op.id());
}

Node* createPathCurve(Graph& g, const Args& a)
{
    Splines given;
    try
    {
        if (a.has("splines"))
            for (const json& j : a.flags["splines"])
                given.push_back(splineFromJson(j));
        else if (a.has("points"))
        {
            Spline sp;
            sp.kind = Kind::Poly;
            for (const json& p : a.flags["points"])
                sp.points.emplace_back(p.at(0).get<double>(), p.at(1).get<double>(), p.at(2).get<double>());
            sp.normalize();
            validate(sp);
            given.push_back(std::move(sp));
        }
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string("pipe: ") + e.what());
    }
    if (given.empty())
        return nullptr;
    Node* c = g.createNode(*types::find("curve"), "PipePath", kNoNode, true);
    g.setValue({c->id(), CurveShape::kCreate, -1}, makeCurveData(std::move(given)));
    return c;
}

std::vector<Node*> pathCurves(Graph& g, const Args& a)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
        out.push_back(&curveNamed(g, p.get<std::string>()));
    if (out.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("curve"))
                out.push_back(n);
    return out;
}

/** A pipe node on one path curve, its settings and mesh inputs set. */
Node* makeNode(Graph& g, const Args& a, const json& values, Node* path, const StartLoop* loop)
{
    Node* op = g.createNode(*types::find("curvePipe"), {}, kNoNode, true);
    applyValues(g, *op, values, "pipe");
    g.connect({path->id(), CurveShape::kWorldSpace, -1}, {op->id(), op->type().attrIndex("path"), -1});
    if (a.has("couplingMeshNode"))
        g.connect({meshNamed(g, a.get<std::string>("couplingMeshNode"), "pipe").id(), MeshNode::kOutMesh, -1},
                  {op->id(), op->type().attrIndex("couplingMesh"), -1});
    if (a.has("boltMeshNode"))
        g.connect({meshNamed(g, a.get<std::string>("boltMeshNode"), "pipe").id(), MeshNode::kOutMesh, -1}, {op->id(), op->type().attrIndex("boltMesh"), -1});
    if (loop)
        g.setValue({op->id(), op->type().attrIndex("startLoop"), -1}, loopToJson(loop->world));
    return op;
}

void registerOptionLayout()
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = {"preset", "radius", "sides", "wallThickness", "bendRadius", "coupling", "fasteners", "splitEvery"};
    o.hidden = {"points", "splines", "loopMesh", "loopFaces", "loopEdges", "name", "couplingMesh", "boltMesh"};
    for (const AttrDef& d : types::find("curvePipe")->ownAttrs)
        if (!d.category.empty())
            o.sections[d.name] = d.category;
    o.sections["fromLoop"] = o.sections["attach"] = "Start";
    o.choices["preset"] = {{"", "None"}, {"pipe", "Pipe"}, {"hose", "Hose"}, {"cable", "Cable"}, {"conduit", "Conduit"}};
    o.ranges["radius"] = {0.01, 1e5, 0.1, " cm"};
    o.ranges["wallThickness"] = {0.0, 1e4, 0.05, " cm"};
    o.ranges["bendRadius"] = {0.0, 1e5, 0.5, " cm"};
    o.ranges["splitEvery"] = {0.0, 1e6, 10.0, " cm"};
    o.ranges["couplingLength"] = {0.0, 1e4, 0.5, " cm"};
    o.ranges["boltPhase"] = {-360.0, 360.0, 1.0, " deg"};
    o.labels = {{"bendRadius", "Bend Radius (0 = 2x Radius)"}, {"minBendRadius", "Min Bend Radius (0 = Radius)"}, {"radiusRamp", "Radius Ramp (t:scale ...)"},
                {"splitAt", "Split At (fractions)"},             {"fromLoop", "Start from Selected Loop"},       {"attach", "Attach to Mesh (bakes)"}};
    opt::setOverride("pipe", std::move(o));
}
} // namespace

PipeOptions pipeOptionsOf(const Node& n)
{
    PipeOptions o;
    o.radius = std::max(1e-4, f(n, "radius"));
    o.sides = int(std::max<int64_t>(3, i(n, "sides")));
    o.radiusRamp = parseRamp(s(n, "radiusRamp"));
    o.wall = std::max(0.0, f(n, "wallThickness"));
    CapMode caps[4] = {CapMode::None, CapMode::Ngon, CapMode::Grid, CapMode::Round};
    o.startCap = caps[std::clamp<int64_t>(i(n, "startCap"), 0, 3)];
    o.endCap = caps[std::clamp<int64_t>(i(n, "endCap"), 0, 3)];
    o.roundSegments = int(std::max<int64_t>(1, i(n, "roundSegments")));
    o.flip = b(n, "flip");
    o.vPerCm = std::max(0.0, f(n, "texelDensity"));
    o.autoFillet = b(n, "autoFillet");
    o.bendRadius = std::max(0.0, f(n, "bendRadius"));
    o.minBendRadius = std::max(0.0, f(n, "minBendRadius"));
    o.tess.mode = TessMode(std::clamp<int64_t>(i(n, "tessellation"), 0, 2));
    o.tess.perSpan = int(std::max<int64_t>(1, i(n, "segments")));
    o.tess.maxAngle = glm::radians(std::clamp(f(n, "maxAngle"), 0.1, 90.0));
    o.splitEvery = std::max(0.0, f(n, "splitEvery"));
    o.splitAtBends = b(n, "splitAtBends");
    o.splitAt = parseNumbers(s(n, "splitAt"));
    o.partsAtEnds = b(n, "partsAtEnds");
    o.segmentMaterials = b(n, "segmentMaterials");
    o.coupling = CouplingKind(std::clamp<int64_t>(i(n, "coupling"), 0, 5));
    o.couplingScale = std::max(1.0, f(n, "couplingScale"));
    o.couplingLength = std::max(0.01, f(n, "couplingLength"));
    o.couplingSides = int(i(n, "couplingSides"));
    o.scaleToRadius = b(n, "scaleToRadius");
    o.meshAxis = int(std::clamp<int64_t>(i(n, "meshAxis"), 0, 2));
    o.meshScale = f(n, "meshScale");
    const MeshPtr& cm = n.as<MeshPtr>(idx(n, "couplingMesh"));
    const MeshPtr& bm = n.as<MeshPtr>(idx(n, "boltMesh"));
    o.couplingMesh = cm.get();
    o.boltMesh = bm.get();
    o.fasteners = FastenerKind(std::clamp<int64_t>(i(n, "fasteners"), 0, 4));
    o.boltCount = int(std::max<int64_t>(1, i(n, "boltCount")));
    o.boltCircle = std::max(0.0, f(n, "boltCircle"));
    o.boltPhase = glm::radians(f(n, "boltPhase"));
    o.boltSize = std::max(0.0, f(n, "boltSize"));
    o.corrugationDepth = std::clamp(f(n, "corrugationDepth"), 0.0, 0.9);
    o.corrugationPeriod = std::max(0.01, f(n, "corrugationPeriod"));
    o.corrugationRings = int(std::max<int64_t>(2, i(n, "corrugationRings")));
    o.startLoop = loopFromJson(s(n, "startLoop"));
    return o;
}

json pipePreset(std::string_view name)
{
    json j = presetBase();
    if (name == "pipe")
        j.update({{"radius", 5.0}, {"sides", 12}, {"wallThickness", 0.4}, {"coupling", "flange"}, {"couplingScale", 1.6}, {"couplingLength", 2.0},
                  {"splitEvery", 200.0}, {"partsAtEnds", true}, {"fasteners", "boltAndNut"}, {"boltCount", 6}});
    else if (name == "hose")
        j.update({{"radius", 2.0}, {"sides", 10}, {"coupling", "collar"}, {"couplingScale", 1.35}, {"couplingLength", 4.0}, {"partsAtEnds", true},
                  {"corrugationDepth", 0.15}, {"corrugationPeriod", 1.5}, {"bendRadius", 8.0}, {"startCap", "grid"}, {"endCap", "grid"}});
    else if (name == "cable")
        j.update({{"radius", 0.6}, {"sides", 6}, {"startCap", "grid"}, {"endCap", "grid"}, {"bendRadius", 3.0}});
    else if (name == "conduit")
        j.update({{"radius", 2.5}, {"sides", 8}, {"wallThickness", 0.25}, {"coupling", "collar"}, {"couplingScale", 1.25}, {"couplingLength", 5.0},
                  {"splitEvery", 300.0}, {"bendRadius", 15.0}});
    else
        return json();
    return j;
}

void registerPipeType()
{
    if (types::find("curvePipe"))
        return;
    const std::string body = "Body", seg = "Segments", cpl = "Couplings", bolts = "Bolts", corr = "Corrugation";
    NodeType t;
    t.name = "curvePipe";
    t.icon = "route";
    t.doc = "A pipe or hose along path curves: round body with rounded bends and an optional wall, split into segments with "
            "couplings, flanges, clamps or elbow collars and bolt rings placed along the rotation-minimising frame.";
    t.ownAttrs = {
        {.name = "path", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = kIn | AttrHidden, .doc = "Path curves, world space."},
        meshIn("couplingMesh", "A mesh to place as the coupling (coupling = mesh)."),
        meshIn("boltMesh", "A mesh to place as the bolts (fasteners = mesh)."),
        sAttr("startLoop", "First ring, world points as JSON: the pipe starts exactly on this loop.", "", AttrHidden),
        fAttr("radius", 5.0, "Radius, cm.", body, 0.0),
        sAttr("radiusRamp", "Radius scale along the length: keys \"t:scale\" with t from 0 to 1, e.g. \"0:1 0.5:1.5 1:1\".", body),
        iAttr("sides", 12, "Sides around.", body, 3),
        fAttr("wallThickness", 0.0, "0 is solid; otherwise a tube with an inner surface and rims, this thick (cm).", body, 0.0),
        eAttr("startCap", kCaps, 0, "Close a solid pipe's start: one face, a quad grid or a dome.", body),
        eAttr("endCap", kCaps, 0, "Close a solid pipe's end: one face, a quad grid or a dome.", body),
        iAttr("roundSegments", 3, "Rings in a dome cap.", body, 1),
        bAttr("autoFillet", true, "Round sharp path corners into bends of bendRadius.", body),
        fAttr("bendRadius", 0.0, "Centreline radius of rounded corners, cm; 0 is twice the radius.", body, 0.0),
        fAttr("minBendRadius", 0.0, "Warn about bends tighter than this, cm; 0 is the radius.", body, 0.0),
        eAttr("tessellation", {"fixed", "adaptive", "game"}, 2,
              "How the path becomes rings: fixed per span, adaptive, or game (straight spans one ring, bends by angle).", body),
        iAttr("segments", 8, "Rings per curved span (fixed).", body, 1),
        fAttr("maxAngle", 10.0, "Largest turn between rings, degrees (adaptive and game).", body, 0.1, 90.0),
        fAttr("texelDensity", 0.0, "V per cm along the path; 0 fits each segment to 0..1.", body, 0.0),
        bAttr("flip", false, "Flip the faces.", body),
        fAttr("splitEvery", 0.0, "Split into segments every this many cm (0: none); couplings go at the splits.", seg, 0.0),
        bAttr("splitAtBends", false, "Also split at each bend.", seg),
        sAttr("splitAt", "Also split at these fractions of the length, e.g. \"0.25 0.5\".", seg),
        bAttr("partsAtEnds", false, "Couplings at both ends as well.", seg),
        bAttr("segmentMaterials", false, "Segments alternate materials 0 and 1, parts take 2.", seg),
        eAttr("coupling", kCouplings, 0, "Part at each split: flange pair, collar, clamp, a collar following the bend, or the coupling mesh.", cpl),
        fAttr("couplingScale", 1.4, "Coupling outer radius, in pipe radii.", cpl, 1.0),
        fAttr("couplingLength", 4.0, "Coupling length along the pipe, cm.", cpl, 0.0),
        iAttr("couplingSides", 0, "Coupling sides; 0 matches the pipe.", cpl),
        bAttr("scaleToRadius", true, "Parts follow the local radius (ramps, tapers); off sizes them by the base radius.", cpl),
        eAttr("meshAxis", {"x", "y", "z"}, 1, "Axis of the coupling and bolt meshes laid along the pipe.", cpl),
        fAttr("meshScale", 1.0, "Scale of the coupling and bolt meshes.", cpl, 0.0),
        eAttr("fasteners", kFasteners, 0, "Bolt ring through each flange (or coupling mesh): bolts with nuts, bolts, nuts, or the bolt mesh.", bolts),
        iAttr("boltCount", 6, "Bolts around each ring.", bolts, 1),
        fAttr("boltCircle", 0.0, "Bolt circle radius, cm; 0 sits them between the hub and the rim.", bolts, 0.0),
        fAttr("boltPhase", 0.0, "Turn of the first bolt from the frame normal, degrees.", bolts),
        fAttr("boltSize", 0.0, "Bolt diameter, cm; 0 sizes them to the flange.", bolts, 0.0),
        fAttr("corrugationDepth", 0.0, "Hose ribs: how far the radius dips, as a fraction of it; 0 is smooth.", corr, 0.0, 0.9),
        fAttr("corrugationPeriod", 2.0, "Rib spacing, cm.", corr, 0.01),
        iAttr("corrugationRings", 4, "Rings per rib.", corr, 2),
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "The pipe mesh."},
        {.name = "partCount", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Parts placed (couplings and bolts)."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last build (tight bends)."},
    };
    t.create = [] { return std::make_unique<PipeNode>(); };
    types::registerType(std::move(t));
}

void registerPipeCommands()
{
    std::vector<FlagDef> flags = attrFlags();
    for (FlagDef fd : std::vector<FlagDef>{
             {"preset", "", T::String, nullptr, "Starting settings: pipe, hose, cable or conduit (flags given as well win).", kPresets},
             {"points", "p", T::List, nullptr, "[[x, y, z], ...] world points of a new poly path curve, instead of path curves."},
             {"splines", "", T::List, nullptr, "Whole path splines (curveData format), instead of path curves."},
             {"fromLoop", "", T::Bool, false, "Start on the selected face (its border) or closed edge loop: it becomes the first ring."},
             {"loopMesh", "", T::String, nullptr, "Mesh holding the start loop (instead of the selection)."},
             {"loopFaces", "", T::List, nullptr, "Faces whose border is the start loop."},
             {"loopEdges", "", T::List, nullptr, "Edges of the start loop."},
             {"attach", "", T::Bool, false, "Build the pipe into the start loop's mesh: its faces go and the first ring welds on (no history)."},
             {"couplingMeshNode", "", T::String, nullptr, "Mesh to use as the coupling (sets coupling = mesh)."},
             {"boltMeshNode", "", T::String, nullptr, "Mesh to use as the bolts (sets fasteners = mesh)."},
             {"constructionHistory", "ch", T::Bool, true, "Keep the pipe node so its settings stay live."},
             {"name", "n", T::String, nullptr, "Mesh name; made unique."}})
        flags.push_back(std::move(fd));

    ops::registerCommand({
        .name = "pipe",
        .doc = "Build a pipe or hose mesh along path curves with live history: radius (and a ramp), sides, wall thickness, rounded "
               "bends, caps, segments with couplings (flange, collar, clamp, elbow collar or any mesh), bolt rings and hose "
               "corrugation. fromLoop starts it on a selected face's border; attach builds it into that mesh. Returns [mesh, curvePipe].",
        .positionalDoc = "paths",
        .flags = flags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            std::optional<StartLoop> loop = startLoopOf(g, a);
            std::vector<Node*> paths;
            if (Node* made = createPathCurve(g, a))
                paths.push_back(made);
            else
                paths = pathCurves(g, a);
            if (paths.empty())
                throw CommandError("pipe: select path curves, or give points=");
            json values = json::object();
            if (a.has("preset"))
            {
                values = pipePreset(a.get<std::string>("preset"));
                if (values.is_null())
                    throw CommandError("pipe: preset is pipe, hose, cable or conduit");
            }
            for (auto& [k, v] : a.flags.items())
                if (!v.is_null())
                    values[k] = v;
            if (a.has("couplingMeshNode"))
                values["coupling"] = "mesh";
            if (a.has("boltMeshNode"))
                values["fasteners"] = "mesh";

            if (a.get<bool>("attach"))
            {
                if (!loop)
                    throw CommandError("pipe: attach needs a start loop (fromLoop or loopMesh)");
                Node* op = makeNode(g, a, values, paths[0], &*loop);
                g.evaluate();
                const Splines& ps = splinesOf(g.value({paths[0]->id(), CurveShape::kWorldSpace, -1}));
                if (ps.empty())
                    throw CommandError("pipe: the path curve is empty");
                PipeOptions o = pipeOptionsOf(*op);
                if (o.wall <= 0.0)
                    o.startCap = CapMode::None;
                attachToMesh(g, *loop, ps.front(), o);
                g.deleteNode(op->id());
                g.select({loop->mesh->id()});
                return json::array({loop->mesh->path(), nullptr});
            }

            json out = json::array();
            std::vector<NodeId> made;
            for (size_t k = 0; k < paths.size(); ++k)
            {
                Node* op = makeNode(g, a, values, paths[k], k == 0 && loop ? &*loop : nullptr);
                Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "Pipe"), kNoNode, true);
                g.connect({op->id(), op->type().attrIndex("outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});
                json opName = op->path();
                if (!a.get<bool>("constructionHistory", true))
                {
                    bake(g, *op, {mesh->id(), MeshNode::kInMesh, -1});
                    opName = nullptr;
                }
                out.push_back(mesh->path());
                out.push_back(opName);
                made.push_back(mesh->id());
            }
            g.select(made);
            return out;
        },
    });

    ops::registerCommand({
        .name = "pipePreset",
        .doc = "Apply a pipe preset (pipe, hose, cable or conduit) to pipe nodes, or the pipes feeding the given or selected meshes.",
        .positionalDoc = "pipes",
        .flags = {{"preset", "", T::String, "pipe", "pipe, hose, cable or conduit.", kPresets}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json values = pipePreset(a.get<std::string>("preset", "pipe"));
            if (values.is_null())
                throw CommandError("pipePreset: preset is pipe, hose, cable or conduit");
            std::vector<Node*> nodes;
            std::vector<Node*> from;
            for (const json& p : a.positional)
                if (Node* n = g.find(p.get<std::string>()))
                    from.push_back(n);
                else
                    throw CommandError("pipePreset: no node named '" + p.get<std::string>() + "'");
            if (from.empty())
                for (NodeId id : g.selection())
                    from.push_back(g.node(id));
            for (Node* n : from)
                if (Node* p = pipeNodeOf(g, n); p && std::ranges::find(nodes, p) == nodes.end())
                    nodes.push_back(p);
            if (nodes.empty())
                throw CommandError("pipePreset: select a pipe");
            json names = json::array();
            for (Node* n : nodes)
            {
                applyValues(g, *n, values, "pipePreset");
                names.push_back(n->path());
            }
            return names;
        },
    });
    registerOptionLayout();
}
} // namespace lila::curve
