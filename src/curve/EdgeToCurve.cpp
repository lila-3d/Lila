#include "curve/EdgeToCurve.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/EdgeCurve.h"
#include "geo/Conform.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <charconv>

namespace lila::curve
{
using namespace scene;
using json = nlohmann::json;
using D3 = glm::dvec3;
using T = ArgType;

namespace
{
constexpr const char* kType = "polyEdgeToCurve";
constexpr uint32_t kIn = AttrInput | AttrStorable;
const std::vector<std::string> kKinds = {"poly", "bezier", "nurbs"};

std::string idsToString(const std::vector<geo::Index>& ids)
{
    std::string s;
    for (geo::Index i : ids)
        s += (s.empty() ? "" : " ") + std::to_string(i);
    return s;
}

std::vector<geo::Index> idsFromString(std::string_view s)
{
    std::vector<geo::Index> out;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end)
    {
        geo::Index v = 0;
        auto [next, ec] = std::from_chars(p, end, v);
        if (ec == std::errc())
            out.push_back(v);
        p = next == p ? p + 1 : next;
    }
    return out;
}

/** Live: rebuilds the curve from the mesh's current positions of the stored edge chain. */
class EdgeToCurveNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const NodeType& t = type();
        int out = t.attrIndex("outCurve");
        try
        {
            MeshPtr mesh = as<MeshPtr>(t.attrIndex("inMesh"));
            if (!mesh)
            {
                setOutput(out, makeCurveData({}));
                return;
            }
            EdgeCurveOptions o;
            o.kind = Kind(std::clamp<int64_t>(as<int64_t>(t.attrIndex("type")), 0, 2));
            o.degree = int(as<int64_t>(t.attrIndex("degree")));
            o.smooth = as<bool>(t.attrIndex("smooth"));
            o.tolerance = as<double>(t.attrIndex("tolerance"));
            o.cornerAngle = glm::radians(as<double>(t.attrIndex("angle")));
            std::vector<geo::Index> edges = idsFromString(as<std::string>(t.attrIndex("edges")));
            setOutput(out, makeCurveData(edgeCurves(*mesh, as<glm::dmat4>(t.attrIndex("meshMatrix")), edges, o, as<D3>(t.attrIndex("origin")))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(out, makeCurveData({}));
        }
    }
};

EdgeCurveOptions optionsFrom(const Args& a)
{
    EdgeCurveOptions o;
    if (!parseKind(a.get<std::string>("type", "bezier"), &o.kind))
        throw CommandError("polyEdgeToCurve: type is poly, bezier or nurbs");
    o.degree = std::clamp(a.get<int>("degree", 3), 1, 7);
    o.smooth = a.get<bool>("smooth", false);
    o.tolerance = std::max(1e-5, a.get<double>("tolerance", 0.05));
    o.cornerAngle = glm::radians(std::clamp(a.get<double>("angle", 40.0), 0.0, 180.0));
    return o;
}

/** The selected edge chains of one mesh, from its edge selection, a face selection's borders or a point selection. */
std::vector<geo::EdgePath> chainsOf(const geo::Mesh& m, ComponentMode kind, const components::Ids& ids)
{
    if (kind == ComponentMode::Face)
        return geo::shapeOfFaces(m, ids).chains;
    if (kind == ComponentMode::Vertex)
        return geo::shapeOfPoints(m, ids).chains;
    return geo::chainEdges(m, ids);
}

struct Source
{
    Node* mesh = nullptr;
    ComponentMode kind = ComponentMode::Edge;
    components::Ids ids;
};

std::vector<Source> sourcesOf(Graph& g, const Args& a)
{
    std::vector<Source> out;
    if (!a.positional.empty())
    {
        std::string name = a.positional[0].get<std::string>();
        Node* n = g.find(name);
        if (!n || !n->type().isA("mesh"))
            throw CommandError("polyEdgeToCurve: " + name + " is not a mesh");
        Source s{n, ComponentMode::Edge, {}};
        if (a.has("faces"))
        {
            s.kind = ComponentMode::Face;
            s.ids = a.flags["faces"].get<components::Ids>();
        }
        else if (a.has("edges"))
            s.ids = a.flags["edges"].get<components::Ids>();
        else
            throw CommandError("polyEdgeToCurve: give edges= or faces= with a mesh");
        out.push_back(std::move(s));
        return out;
    }
    ComponentMode mode = components::mode();
    std::vector<ComponentMode> kinds = mode == ComponentMode::Object ? std::vector{ComponentMode::Edge, ComponentMode::Face} : std::vector{mode};
    for (ComponentMode kind : kinds)
    {
        for (auto& [id, ids] : components::selected(kind))
            if (Node* n = g.node(id); n && n->type().isA("mesh") && !ids.empty())
                out.push_back({n, kind, ids});
        if (!out.empty())
            break;
    }
    if (out.empty())
        throw CommandError("polyEdgeToCurve: select mesh edges, or faces to use their borders");
    return out;
}

/** The plug whose mesh carries the ids components are picked by, to feed live history; nullopt when there is none to connect. */
std::optional<PlugRef> cagePlug(Graph& g, Node& mesh)
{
    if (modifiers::stack(g, mesh.id()).empty())
        return PlugRef{mesh.id(), MeshNode::kOutMesh, -1};
    if (Node* src = g.node(modifiers::stackSource(g, mesh.id())))
        if (int out = src->type().attrIndex("outMesh"); out >= 0)
            return PlugRef{src->id(), out, -1};
    return std::nullopt;
}

void setOp(Graph& g, Node& op, const char* attr, PlugValue v) { g.setValue({op.id(), op.type().attrIndex(attr), -1}, std::move(v)); }

json run(const Args& a)
{
    Graph& g = Graph::current();
    g.evaluate();
    EdgeCurveOptions o = optionsFrom(a);
    bool history = a.get<bool>("constructionHistory", false);
    std::string name = a.get<std::string>("name", "EdgeCurve");
    json names = json::array();
    std::vector<NodeId> made;
    for (Source& src : sourcesOf(g, a))
    {
        MeshPtr cage = modifiers::cage(g, src.mesh->id());
        if (!cage)
            continue;
        glm::dmat4 world = src.mesh->as<glm::dmat4>(Transform::kWorldMatrix);
        std::optional<PlugRef> live = history ? cagePlug(g, *src.mesh) : std::nullopt;
        for (const geo::EdgePath& chain : chainsOf(*cage, src.kind, src.ids))
        {
            if (chain.points.size() < 2)
                continue;
            D3 origin(0.0);
            for (geo::Index p : chain.points)
                origin += D3(world * glm::dvec4(cage->position(p), 1.0));
            origin /= double(chain.points.size());
            Splines s = edgeCurves(*cage, world, chain.edges, o, origin);
            if (s.empty())
                continue;
            Node* c = g.createNode(*types::find("curve"), name, kNoNode, true);
            g.setValue({c->id(), Transform::kTranslate, -1}, origin);
            PlugRef create{c->id(), CurveShape::kCreate, -1};
            if (live)
            {
                Node* op = g.createNode(*types::find(kType), {}, kNoNode, true);
                g.connect(*live, {op->id(), op->type().attrIndex("inMesh"), -1});
                g.connect({src.mesh->id(), Transform::kWorldMatrix, -1}, {op->id(), op->type().attrIndex("meshMatrix"), -1});
                setOp(g, *op, "edges", idsToString(chain.edges));
                setOp(g, *op, "type", int64_t(o.kind));
                setOp(g, *op, "degree", int64_t(o.degree));
                setOp(g, *op, "smooth", o.smooth);
                setOp(g, *op, "tolerance", o.tolerance);
                setOp(g, *op, "angle", glm::degrees(o.cornerAngle));
                setOp(g, *op, "origin", origin);
                g.connect({op->id(), op->type().attrIndex("outCurve"), -1}, create);
            }
            else
                g.setValue(create, makeCurveData(std::move(s)));
            names.push_back(c->path());
            made.push_back(c->id());
        }
    }
    if (made.empty())
        throw CommandError("polyEdgeToCurve: the selection has no edge chains");
    components::setModeUndoable(ComponentMode::Object);
    g.select(made);
    return names;
}
} // namespace

void registerEdgeToCurveType()
{
    if (types::find(kType))
        return;
    NodeType t;
    t.name = kType;
    t.icon = "vector-spline";
    t.doc = "Builds a curve along a chain of mesh edges, following the mesh as it changes while the edge ids stay valid.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The mesh the edges belong to."},
        {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The mesh's world matrix."},
        {.name = "edges", .type = PlugType::String, .defaultValue = std::string(), .flags = kIn | AttrHidden, .doc = "Edge ids, space separated."},
        {.name = "type", .type = PlugType::Enum, .defaultValue = int64_t(1), .flags = kIn, .enumNames = kKinds, .doc = "Curve kind."},
        {.name = "degree", .type = PlugType::Int, .defaultValue = int64_t(3), .flags = kIn, .doc = "NURBS degree.", .minValue = 1.0, .maxValue = 7.0},
        {.name = "smooth", .type = PlugType::Bool, .defaultValue = false, .flags = kIn, .doc = "Fit within tolerance rather than one point per vertex."},
        {.name = "tolerance", .type = PlugType::Float, .defaultValue = 0.05, .flags = kIn, .doc = "Fitting tolerance, cm.", .minValue = 1e-5},
        {.name = "angle", .type = PlugType::Float, .defaultValue = 40.0, .flags = kIn, .doc = "Points turning more than this stay corners, degrees.",
         .minValue = 0.0, .maxValue = 180.0},
        {.name = "origin", .type = PlugType::Vec3, .defaultValue = D3(0.0), .flags = kIn | AttrHidden,
         .childNames = {"originX", "originY", "originZ"}, .doc = "World position the curve sits at."},
        {.name = "outCurve", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden, .doc = "The curves it makes."},
    };
    t.create = [] { return std::make_unique<EdgeToCurveNode>(); };
    types::registerType(std::move(t));
}

void registerEdgeToCurveCommand()
{
    ops::registerCommand({
        .name = "polyEdgeToCurve",
        .doc = "Make a curve from each chain of the selected mesh edges (or the borders of selected faces), in world space with "
               "the curve at the chain's centre. Closed loops make closed curves; branching points split chains. Selects the new "
               "curves. Returns their names.",
        .positionalDoc = "[mesh]",
        .flags = {{"type", "t", T::String, "bezier", "Curve kind: Bezier points keep corners sharper than angle and are smooth elsewhere.",
                   {"poly", "bezier", "nurbs"}},
                  {"degree", "d", T::Int, 3, "NURBS degree (1 to 7)."},
                  {"smooth", "sm", T::Bool, false, "Fit within tolerance with fewer points rather than one point per vertex."},
                  {"tolerance", "tol", T::Float, 0.05, "Fitting tolerance with smooth, cm."},
                  {"angle", "a", T::Float, 40.0, "Points where the chain turns more than this stay corners, degrees."},
                  {"constructionHistory", "ch", T::Bool, false, "Keep the curve following the mesh while its edge ids stay valid."},
                  {"edges", "", T::List, nullptr, "Edge ids of the given mesh, instead of the selection."},
                  {"faces", "", T::List, nullptr, "Face ids of the given mesh: their borders."},
                  {"name", "n", T::String, nullptr, "Curve name; made unique."}},
        .run = run,
        .adjustable = true,
    });

    namespace opt = ops::options;
    opt::Override o;
    o.order = {"type", "degree", "smooth", "tolerance", "angle", "constructionHistory"};
    o.hidden = {"edges", "faces", "name"};
    o.choices["type"] = {{"poly", "Poly"}, {"bezier", "Bezier"}, {"nurbs", "NURBS"}};
    o.ranges["tolerance"] = {1e-4, 1e4, 0.01, " cm"};
    o.ranges["angle"] = {0.0, 180.0, 5.0, " deg"};
    o.labels = {{"smooth", "Fit Smooth"}, {"angle", "Corner Angle"}, {"constructionHistory", "Keep History"}};
    opt::setOverride("polyEdgeToCurve", std::move(o));
}
} // namespace lila::curve
