#include "curve/NodeHelpers.h"
#include "curve/EnvironmentNodes.h"
#include "curve/StrapNode.h"
#include "curve/StripUvAttrs.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Ops.h"
#include "ops/CommandOptions.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"

#include <algorithm>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace strapnode
{
using helpers::attrFlags;
using helpers::b;
using helpers::bAttr;
using helpers::bake;
using helpers::dataIn;
using helpers::dataOut;
using helpers::eAttr;
using helpers::f;
using helpers::fAttr;
using helpers::i;
using helpers::iAttr;
using helpers::idx;
using helpers::stringOut;
using helpers::v;
using helpers::vAttr;

namespace
{
const std::vector<std::string> kPresets = {"strap", "rope", "tape", "metalBand", "zipTie"};

AttrDef vOut(std::string name, std::string doc)
{
    AttrDef a{.name = name, .type = PlugType::Vec3, .defaultValue = D3(0.0), .flags = AttrOutput | AttrHidden, .doc = std::move(doc)};
    for (int k = 0; k < 3; ++k)
        a.childNames[size_t(k)] = name + char('X' + k);
    return a;
}

AttrDef hidden(AttrDef a)
{
    a.flags |= AttrHidden;
    return a;
}

void setWarning(Node& n, const std::string& warning)
{
    if (warning != n.as<std::string>(idx(n, "warning")) && !warning.empty())
        log::warn("{}: {}", n.name(), warning);
}

/** The cut around what a slash crosses, kept live: the strap's path. */
class StrapPathNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int out = idx(*this, "outCurve");
        try
        {
            std::vector<geo::SectionMesh> meshes;
            std::vector<MeshPtr> keep;
            const json faces = json::parse(as<std::string>(idx(*this, "faces")), nullptr, false);
            const auto& matrices = elements(idx(*this, "meshMatrices"));
            for (const auto& [k, val] : elements(idx(*this, "inMeshes")))
            {
                MeshPtr m = std::get<MeshPtr>(val);
                if (faces.is_array() && size_t(k) < faces.size() && faces[size_t(k)].is_array())
                    m = strapFaces(m, faces[size_t(k)].get<components::Ids>());
                auto mx = std::ranges::find(matrices, k, [](const auto& e) { return e.first; });
                if (m && m->numFaces() > 0)
                {
                    keep.push_back(m);
                    meshes.push_back({m.get(), mx != matrices.end() ? std::get<glm::dmat4>(mx->second) : glm::dmat4(1.0)});
                }
            }
            StrapSlash slash{v(*this, "start"), v(*this, "startDir"), v(*this, "end"), v(*this, "endDir")};
            StrapPathOptions o{.gap = std::max(0.0, f(*this, "gap")),
                               .bridge = std::max(0.0, f(*this, "bridge")),
                               .taut = b(*this, "taut"),
                               .tolerance = std::max(0.0, f(*this, "tolerance")),
                               .width = std::max(0.0, f(*this, "width"))};
            StrapPathResult r = meshes.empty() ? StrapPathResult{.warning = "no mesh to strap"} : strapPath(meshes, slash, o);
            Splines curves;
            if (r.path)
                curves.push_back(std::move(*r.path));
            setWarning(*this, r.warning);
            setOutput(idx(*this, "warning"), r.warning);
            setOutput(idx(*this, "outUp"), r.up);
            setOutput(idx(*this, "loops"), int64_t(r.loops));
            setOutput(out, makeCurveData(std::move(curves)));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, DataPtr());
        }
    }
};

StrapOptions strapOptionsOf(const Node& n)
{
    StrapOptions o;
    o.style = StrapStyle(std::clamp<int64_t>(i(n, "style"), 0, 1));
    o.width = std::max(1e-3, f(n, "width"));
    o.thickness = std::max(1e-4, f(n, "thickness"));
    o.bevel = std::max(0.0, f(n, "bevel"));
    o.strands = int(std::clamp<int64_t>(i(n, "strands"), 1, 12));
    o.strandSides = int(std::clamp<int64_t>(i(n, "strandSides"), 3, 64));
    o.lay = std::max(0.1, f(n, "lay"));
    o.buckle = b(n, "buckle");
    o.buckleAt = f(n, "buckleAt");
    o.buckleLength = std::max(0.0, f(n, "buckleLength"));
    o.buckleScale = std::max(1.0, f(n, "buckleScale"));
    o.up = v(n, "up");
    o.flip = b(n, "flip");
    o.uv = stripUvOf(n, {});
    return o;
}

/** A band or rope swept along path curves that lie on a surface. */
class StrapNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int out = idx(*this, "outMesh");
        try
        {
            StrapOptions o = strapOptionsOf(*this);
            geo::Mesh mesh;
            std::string warning;
            for (const Spline& sp : splinesOf(value(idx(*this, "path"))))
            {
                StrapResult r = makeStrap(sp, o);
                mesh.append(r.mesh);
                for (const std::string& w : r.warnings)
                    warning += (warning.empty() ? "" : "; ") + w;
            }
            setWarning(*this, warning);
            setOutput(idx(*this, "warning"), warning);
            setOutput(out, MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, MeshPtr());
        }
    }
};

/** Every attr a preset sets, at its default first, so switching presets leaves nothing of the last one. */
json presetValues(std::string_view name)
{
    json j = json::object();
    for (const AttrDef& d : types::find("curveStrap")->ownAttrs)
        if ((d.flags & AttrInput) && !(d.flags & AttrHidden) && d.name != "flip" && !isUvAttr(d.name) && d.name != "buckleAt")
            j[d.name] = d.type == PlugType::Enum ? json(d.enumNames[size_t(std::get<int64_t>(d.defaultValue))]) : toJson(d.defaultValue);
    j["gap"] = 0.0;
    if (name == "strap")
        j.update({{"style", "band"}, {"width", 4.0}, {"thickness", 0.35}, {"bevel", 0.08}, {"buckle", true}, {"buckleLength", 5.0}, {"buckleScale", 1.3}});
    else if (name == "rope")
        j.update({{"style", "rope"}, {"width", 2.5}, {"strands", 3}, {"strandSides", 8}, {"lay", 7.0}});
    else if (name == "tape")
        j.update({{"style", "band"}, {"width", 5.0}, {"thickness", 0.04}, {"gap", 0.02}});
    else if (name == "metalBand")
        j.update({{"style", "band"}, {"width", 2.0}, {"thickness", 0.12}, {"buckle", true}, {"buckleLength", 2.5}, {"buckleScale", 1.15}});
    else if (name == "zipTie")
        j.update({{"style", "band"}, {"width", 0.8}, {"thickness", 0.15}, {"bevel", 0.03}, {"buckle", true}, {"buckleLength", 1.2}, {"buckleScale", 1.6}});
    else
        return json();
    return j;
}

/** The path node a strap's path curve comes from, if it is still live. */
Node* pathNodeOf(Graph& g, Node& strap)
{
    std::optional<PlugRef> curve = g.source({strap.id(), idx(strap, "path"), -1});
    std::optional<PlugRef> cut = curve ? g.source({curve->node, CurveShape::kCreate, -1}) : std::nullopt;
    Node* n = cut ? g.node(cut->node) : nullptr;
    return n && n->type().name == "curveStrapPath" ? n : nullptr;
}

/** The strap node feeding a mesh (through anything between), or the node itself. */
Node* strapNodeOf(Graph& g, Node* n)
{
    for (int depth = 0; n && depth < 16; ++depth)
    {
        if (n->type().name == "curveStrap")
            return n;
        if (!n->type().isA("mesh"))
            return nullptr;
        std::optional<PlugRef> src = g.source({n->id(), MeshNode::kInMesh, -1});
        n = src ? g.node(src->node) : nullptr;
    }
    return nullptr;
}

void applyValues(Graph& g, Node& n, const json& values, const char* cmd)
{
    for (const AttrDef& d : n.type().ownAttrs)
    {
        if (!(d.flags & AttrInput) || (d.flags & AttrHidden) || !values.contains(d.name) || values[d.name].is_null())
            continue;
        const int at = n.type().attrIndex(d.name);
        const json& val = values[d.name];
        if (d.type == PlugType::Enum && val.is_string())
        {
            auto it = std::ranges::find(d.enumNames, val.get<std::string>());
            if (it == d.enumNames.end())
                throw CommandError(std::string(cmd) + ": " + d.name + ": unknown value '" + val.get<std::string>() + "'");
            g.setValue({n.id(), at, -1}, int64_t(it - d.enumNames.begin()));
            continue;
        }
        try
        {
            // The strap's width is driven by its path node's, which clears the surface across it.
            const PlugRef plug{n.id(), at, -1};
            g.setValue(g.source(plug).value_or(plug), fromJson(d, val));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + d.name + ": " + e.what());
        }
    }
}

std::vector<Node*> targetMeshes(Graph& g, const Args& a)
{
    std::vector<Node*> out;
    for (const json& p : a.positional)
    {
        Node* n = g.find(p.get<std::string>());
        if (!n || !n->type().isA("mesh"))
            throw CommandError("strap: " + p.get<std::string>() + " is not a mesh");
        out.push_back(n);
    }
    if (a.positional.empty())
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                out.push_back(n);
    if (out.empty())
        throw CommandError("strap: select the meshes to strap");
    return out;
}

D3 vecFlag(const Args& a, const char* flag)
{
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("strap: ") + flag + ": expected [x, y, z]");
    return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}

void registerOptionLayout()
{
    namespace opt = ops::options;
    opt::Override o;
    o.order = {"preset", "style", "width", "thickness", "bevel", "strands", "lay", "buckle", "gap", "bridge", "taut"};
    o.hidden = {"start", "end", "view", "startDir", "endDir", "name"};
    o.choices["preset"] = {{"", "None"}, {"strap", "Strap"}, {"rope", "Rope"}, {"tape", "Tape"}, {"metalBand", "Metal Band"}, {"zipTie", "Zip Tie"}};
    o.ranges["width"] = {0.01, 1e4, 0.1, " cm"};
    o.ranges["thickness"] = {0.001, 1e3, 0.05, " cm"};
    o.ranges["bevel"] = {0.0, 1e3, 0.01, " cm"};
    o.ranges["lay"] = {0.1, 1e4, 0.5, " cm"};
    o.ranges["gap"] = {0.0, 1e3, 0.05, " cm"};
    o.ranges["bridge"] = {0.0, 1e5, 1.0, " cm"};
    o.ranges["buckleLength"] = {0.0, 1e4, 0.5, " cm"};
    o.labels = {{"width", "Width (rope: diameter)"}, {"bridge", "Bridge Hollows (cm)"}, {"taut", "Pulled Taut"}, {"lay", "Lay (cm per turn)"}};
    opt::setOverride("strap", std::move(o));
}
} // namespace

void registerTypes()
{
    if (types::find("curveStrap"))
        return;
    {
        NodeType t;
        t.name = "curveStrapPath";
        t.icon = "slice";
        t.doc = "The path of a strap: the cut through meshes in the plane of a slash, around the loops it crosses (bundled into "
                "one when it crosses several), lifted off the surface by gap, with hollows bridged or pulled taut.";
        t.ownAttrs.push_back({.name = "inMeshes", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden | AttrArray,
                              .doc = "The meshes to strap.", .fileAlias = "inMesh"});
        t.ownAttrs.push_back({.name = "meshMatrices", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0),
                              .flags = AttrInput | AttrHidden | AttrArray, .doc = "Each mesh's world matrix, by the same element.",
                              .fileAlias = "meshMatrix"});
        t.ownAttrs.push_back({.name = "faces", .type = PlugType::String, .defaultValue = std::string(), .flags = helpers::kIn | AttrHidden,
                              .doc = "Faces that count per inMeshes element, as JSON: [[ids] or null, ...]; empty is every face."});
        for (AttrDef a : {hidden(vAttr("start", D3(0.0), "A point on the ray through the slash's start.")),
                          hidden(vAttr("startDir", D3(0, 0, -1), "That ray's direction.")),
                          hidden(vAttr("end", D3(0.0), "A point on the ray through the slash's end.")),
                          hidden(vAttr("endDir", D3(0, 0, -1), "That ray's direction.")),
                          fAttr("width", 4.0, "Width the strap clears the surface across, cm; drives the strap's width.", 0.0),
                          fAttr("gap", 0.0, "cm between the surface and the strap.", 0.0),
                          fAttr("bridge", 0.0, "Span hollows narrower than twice this many cm, as a stiff strap would; 0 follows every dip.", 0.0),
                          bAttr("taut", false, "Pulled tight: span every hollow (the convex hull of what the strap goes around)."),
                          fAttr("tolerance", 0.01, "Simplify the path within this many cm; 0 keeps every crossing.", 0.0)})
            t.ownAttrs.push_back(std::move(a));
        t.ownAttrs.push_back(dataOut("outCurve", "The strap's path, world space."));
        t.ownAttrs.push_back(vOut("outUp", "The cutting plane's normal: an open path's outside is tangent x up."));
        t.ownAttrs.push_back({.name = "loops", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = AttrOutput, .doc = "Loops the slash crossed."});
        t.ownAttrs.push_back(stringOut("warning", "Why the last cut made no path, if it didn't."));
        t.create = [] { return std::make_unique<StrapPathNode>(); };
        types::registerType(std::move(t));
    }
    NodeType t;
    t.name = "curveStrap";
    t.icon = "route";
    t.doc = "A strap along path curves lying on a surface: a flat band (with a bevel) or twisted rope, its underside on the path, "
            "with an optional buckle.";
    t.ownAttrs = {
        dataIn("path", "Path curves, world space.", false),
        hidden(vAttr("up", D3(0.0), "Open paths: tangent x up is the outside. Zero takes the path's plane.")),
        eAttr("style", {"band", "rope"}, 0, "A flat band, or rope of twisted strands."),
        fAttr("width", 4.0, "Width across, cm (rope: its diameter).", 0.0),
        fAttr("thickness", 0.35, "Band thickness off the surface, cm.", 0.0),
        fAttr("bevel", 0.0, "Band corners cut back this far, cm.", 0.0),
        iAttr("strands", 3, "Rope strands.", 1, 12),
        iAttr("strandSides", 8, "Sides around each strand.", 3, 64),
        fAttr("lay", 7.0, "Rope length per turn of the strands, cm; loops round it to whole turns.", 0.1),
        bAttr("buckle", false, "A buckle on a band, a whipping on rope."),
        fAttr("buckleAt", 0.0, "Where the buckle sits, as a fraction of the length.", 0.0, 1.0),
        fAttr("buckleLength", 4.0, "Buckle length along the strap, cm.", 0.0),
        fAttr("buckleScale", 1.3, "Buckle width over the strap's.", 1.0),
        bAttr("flip", false, "Put the strap on the other side of the path."),
        {.name = "outMesh", .shortName = "o", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "The strap mesh."},
        stringOut("warning", "Problems with the last build."),
    };
    for (AttrDef& a : stripUvAttrs({}, true, "UVs"))
        t.ownAttrs.push_back(std::move(a));
    t.create = [] { return std::make_unique<StrapNode>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    std::vector<FlagDef> flags = attrFlags("curveStrap");
    for (FlagDef& fd : attrFlags("curveStrapPath"))
        if (std::ranges::none_of(flags, [&](const FlagDef& have) { return have.name == fd.name; }))
            flags.push_back(std::move(fd));
    for (FlagDef fd : std::vector<FlagDef>{
             {"preset", "", T::String, nullptr, "Starting settings: strap, rope, tape, metalBand or zipTie (flags given as well win).", kPresets},
             {"faces", "", T::List, nullptr, "Per mesh given, the face ids that count, or null for all of it."},
             {"start", "", T::Vec3, nullptr, "World point where the slash starts."},
             {"end", "", T::Vec3, nullptr, "World point where the slash ends."},
             {"view", "", T::Vec3, nullptr, "Direction the slash is seen along (both rays), default -Y."},
             {"startDir", "", T::Vec3, nullptr, "Direction of the ray through start (a perspective slash), instead of view."},
             {"endDir", "", T::Vec3, nullptr, "Direction of the ray through end, instead of view."},
             {"constructionHistory", "ch", T::Bool, true, "Keep the strap nodes so the strap follows the meshes and settings."},
             {"name", "n", T::String, nullptr, "Mesh name; made unique."}})
        flags.push_back(std::move(fd));

    ops::registerCommand({
        .name = "strap",
        .doc = "Strap meshes: the slash from start to end, seen along view, cuts them in its plane, and a band or rope goes round "
               "the loops it crosses (several are bundled into one), sitting on the surface. The path curve and the strap stay "
               "live while history is on. Returns [mesh, curve, curveStrap, curveStrapPath].",
        .positionalDoc = "meshes (default: the selected meshes or components, else every visible mesh)",
        .flags = flags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            std::vector<Node*> meshes;
            json faces = json::array();
            if (a.positional.empty())
            {
                for (const StrapTarget& t : strapTargets(g))
                {
                    meshes.push_back(g.node(t.node));
                    faces.push_back(t.faces ? json(*t.faces) : json());
                }
                if (meshes.empty())
                    throw CommandError("strap: select the meshes to strap, or show some");
            }
            else
            {
                meshes = targetMeshes(g, a);
                if (a.has("faces"))
                    faces = a.flags["faces"];
                if (!faces.is_array() || (!faces.empty() && faces.size() != meshes.size()))
                    throw CommandError("strap: faces needs one entry (a list of face ids, or null) per mesh");
            }
            if (!a.has("start") || !a.has("end"))
                throw CommandError("strap: give the slash's start and end");
            const D3 start = vecFlag(a, "start"), end = vecFlag(a, "end");
            D3 view = a.has("view") ? vecFlag(a, "view") : D3(0, -1, 0);
            const D3 startDir = a.has("startDir") ? vecFlag(a, "startDir") : view;
            const D3 endDir = a.has("endDir") ? vecFlag(a, "endDir") : view;
            if (glm::length(startDir) < 1e-12 || glm::length(endDir) < 1e-12)
                throw CommandError("strap: the view direction has no length");
            if (glm::length(StrapSlash{start, startDir, end, endDir}.normal()) < 0.5)
                throw CommandError("strap: the slash has no length across the view");

            json values = json::object();
            if (a.has("preset"))
            {
                values = presetValues(a.get<std::string>("preset"));
                if (values.is_null())
                    throw CommandError("strap: preset is strap, rope, tape, metalBand or zipTie");
            }
            for (auto& [k, val] : a.flags.items())
                if (!val.is_null())
                    values[k] = val;

            Node* cut = g.createNode(*types::find("curveStrapPath"), {}, kNoNode, true);
            applyValues(g, *cut, values, "strap");
            g.setValue({cut->id(), idx(*cut, "start"), -1}, start);
            g.setValue({cut->id(), idx(*cut, "end"), -1}, end);
            g.setValue({cut->id(), idx(*cut, "startDir"), -1}, glm::normalize(startDir));
            g.setValue({cut->id(), idx(*cut, "endDir"), -1}, glm::normalize(endDir));
            if (std::ranges::any_of(faces, [](const json& f) { return f.is_array(); }))
                g.setValue({cut->id(), idx(*cut, "faces"), -1}, faces.dump());
            for (size_t k = 0; k < meshes.size(); ++k)
            {
                g.connect({meshes[k]->id(), MeshNode::kOutMesh, -1}, {cut->id(), idx(*cut, "inMeshes"), -1, int(k)});
                g.connect({meshes[k]->id(), Transform::kWorldMatrix, -1}, {cut->id(), idx(*cut, "meshMatrices"), -1, int(k)});
            }
            Node* curve = g.createNode(*types::find("curve"), "StrapPath", kNoNode, true);
            g.connect({cut->id(), idx(*cut, "outCurve"), -1}, {curve->id(), CurveShape::kCreate, -1});

            Node* op = g.createNode(*types::find("curveStrap"), {}, kNoNode, true);
            applyValues(g, *op, values, "strap");
            g.connect({curve->id(), CurveShape::kWorldSpace, -1}, {op->id(), idx(*op, "path"), -1});
            g.connect({cut->id(), idx(*cut, "outUp"), -1}, {op->id(), idx(*op, "up"), -1});
            g.connect({cut->id(), idx(*cut, "width"), -1}, {op->id(), idx(*op, "width"), -1});
            Node* mesh = g.createNode(*types::find("mesh"), a.get<std::string>("name", "Strap"), kNoNode, true);
            g.connect({op->id(), idx(*op, "outMesh"), -1}, {mesh->id(), MeshNode::kInMesh, -1});

            g.evaluate();
            const std::string warning = cut->as<std::string>(idx(*cut, "warning"));
            if (splinesOf(g.value({curve->id(), CurveShape::kLocal, -1})).empty())
                throw CommandError("strap: no strap made" + (warning.empty() ? std::string() : ": " + warning));
            json opName = op->path(), cutName = cut->path();
            if (!a.get<bool>("constructionHistory", true))
            {
                const D3 up = cut->as<D3>(idx(*cut, "outUp"));
                const double width = cut->as<double>(idx(*cut, "width"));
                for (const char* attr : {"up", "width"})
                    g.disconnect({op->id(), idx(*op, attr), -1});
                g.setValue({op->id(), idx(*op, "up"), -1}, up);
                g.setValue({op->id(), idx(*op, "width"), -1}, width);
                bake(g, *op, {mesh->id(), MeshNode::kInMesh, -1});
                bake(g, *cut, {curve->id(), CurveShape::kCreate, -1});
                opName = cutName = nullptr;
            }
            g.select({mesh->id()});
            return json::array({mesh->path(), curve->path(), opName, cutName});
        },
    });

    ops::registerCommand({
        .name = "strapPreset",
        .doc = "Apply a strap preset (strap, rope, tape, metalBand or zipTie) to strap nodes, or the straps feeding the given or "
               "selected meshes.",
        .positionalDoc = "straps",
        .flags = {{"preset", "", T::String, "strap", "strap, rope, tape, metalBand or zipTie.", kPresets}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json values = presetValues(a.get<std::string>("preset", "strap"));
            if (values.is_null())
                throw CommandError("strapPreset: preset is strap, rope, tape, metalBand or zipTie");
            std::vector<Node*> from, nodes;
            for (const json& p : a.positional)
                if (Node* n = g.find(p.get<std::string>()))
                    from.push_back(n);
                else
                    throw CommandError("strapPreset: no node named '" + p.get<std::string>() + "'");
            if (from.empty())
                for (NodeId id : g.selection())
                    from.push_back(g.node(id));
            for (Node* n : from)
                if (Node* s = strapNodeOf(g, n); s && std::ranges::find(nodes, s) == nodes.end())
                    nodes.push_back(s);
            if (nodes.empty())
                throw CommandError("strapPreset: select a strap");
            json names = json::array();
            for (Node* n : nodes)
            {
                applyValues(g, *n, values, "strapPreset");
                if (Node* cut = pathNodeOf(g, *n))
                    applyValues(g, *cut, values, "strapPreset");
                names.push_back(n->path());
            }
            return names;
        },
    });
    registerOptionLayout();
}
} // namespace strapnode

void registerStrapTypes() { strapnode::registerTypes(); }
void registerStrapCommands() { strapnode::registerCommands(); }

json strapPreset(std::string_view name) { return strapnode::presetValues(name); }

std::vector<StrapTarget> strapTargets(Graph& g)
{
    g.evaluate();
    const isolation::Set* iso = isolation::active();
    auto shown = [&](NodeId id) { return effectivelyVisible(g, id) && (!iso || iso->shows(id)); };
    std::vector<StrapTarget> out;
    auto has = [&](NodeId id) { return std::ranges::any_of(out, [&](const StrapTarget& t) { return t.node == id; }); };
    if (const ComponentMode mode = components::mode(); mode != ComponentMode::Object)
        for (const auto& [id, ids] : components::selected(mode))
            if (Node* n = g.node(id); n && n->type().isA("mesh") && !ids.empty() && shown(id))
                if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                    out.push_back({id, isolation::facesOf(*m, mode, ids)});
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && shown(id) && !has(id))
            out.push_back({id, std::nullopt});
    // With nothing selected, straps already made are not things to strap.
    if (out.empty())
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && n->type().isA("mesh") && shown(id) && !strapnode::strapNodeOf(g, n))
                out.push_back({id, std::nullopt});

    // Faces isolation hides drop out, and a mesh left with none goes.
    std::erase_if(out, [&](StrapTarget& t) {
        const MeshPtr& m = g.node(t.node)->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            return true;
        if (!iso || !iso->isPartial(t.node))
            return false;
        const std::vector<uint8_t> mask = iso->shownFaces(t.node, *m);
        components::Ids kept;
        if (t.faces)
        {
            for (uint32_t f : *t.faces)
                if (f < mask.size() && mask[f])
                    kept.push_back(f);
        }
        else
            for (uint32_t f = 0; f < mask.size(); ++f)
                if (mask[f] && m->faceValid(f))
                    kept.push_back(f);
        t.faces = std::move(kept);
        return t.faces->empty();
    });
    return out;
}

MeshPtr strapFaces(const MeshPtr& mesh, const std::optional<components::Ids>& faces)
{
    if (!mesh || !faces)
        return mesh;
    std::vector<uint8_t> keep(mesh->faceCapacity(), 0);
    for (uint32_t f : *faces)
        if (f < keep.size())
            keep[f] = 1;
    std::vector<geo::Index> drop;
    for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
        if (mesh->faceValid(f) && !keep[f])
            drop.push_back(f);
    if (drop.empty())
        return mesh;
    auto trimmed = std::make_shared<geo::Mesh>(*mesh);
    geo::deleteFaces(*trimmed, drop);
    return trimmed;
}

StrapOptions strapOptionsFrom(const json& values)
{
    StrapOptions o;
    auto num = [&](const char* k, double def) { return values.contains(k) && values[k].is_number() ? values[k].get<double>() : def; };
    auto flag = [&](const char* k, bool def) { return values.contains(k) && values[k].is_boolean() ? values[k].get<bool>() : def; };
    o.style = values.value("style", std::string("band")) == "rope" ? StrapStyle::Rope : StrapStyle::Band;
    o.width = std::max(1e-3, num("width", 4.0));
    o.thickness = std::max(1e-4, num("thickness", 0.35));
    o.bevel = std::max(0.0, num("bevel", 0.0));
    o.strands = int(std::clamp(num("strands", 3.0), 1.0, 12.0));
    o.strandSides = int(std::clamp(num("strandSides", 8.0), 3.0, 64.0));
    o.lay = std::max(0.1, num("lay", 7.0));
    o.buckle = flag("buckle", false);
    o.buckleAt = num("buckleAt", 0.0);
    o.buckleLength = std::max(0.0, num("buckleLength", 4.0));
    o.buckleScale = std::max(1.0, num("buckleScale", 1.3));
    o.flip = flag("flip", false);
    o.uv = stripUvFrom(values, {});
    return o;
}
} // namespace lila::curve
