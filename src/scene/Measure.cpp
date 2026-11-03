#include "scene/Measure.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/AttrBuilders.h"
#include "scene/Components.h"
#include "scene/MeasureMath.h"
#include "scene/MeshNodes.h"
#include "scene/RigUtil.h"

#include <charconv>
#include <sstream>

namespace lila::scene
{
using namespace rigutil;
using namespace attrdefs;

namespace
{
const uint32_t kOut = AttrOutput;

AttrDef floatOut(std::string name, std::string doc)
{
    return {.name = std::move(name), .type = PlugType::Float, .defaultValue = 0.0, .flags = kOut, .doc = std::move(doc)};
}

glm::dvec3 origin(const glm::dmat4& m) { return glm::dvec3(m[3]); }

std::vector<uint32_t> parseIds(const std::string& s)
{
    std::vector<uint32_t> out;
    const char* p = s.data();
    const char* end = p + s.size();
    while (p < end)
    {
        while (p < end && (*p == ' ' || *p == ','))
            ++p;
        uint32_t v = 0;
        auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc())
            break;
        out.push_back(v);
        p = r.ptr;
    }
    return out;
}

std::string joinIds(const std::vector<uint32_t>& ids)
{
    std::string out;
    for (uint32_t id : ids)
        out += (out.empty() ? "" : " ") + std::to_string(id);
    return out;
}
} // namespace

void DistanceMeasure::compute(const EvalContext& ctx)
{
    Transform::compute(ctx);
    glm::dvec3 a = origin(as<glm::dmat4>(kStartMatrix)), b = origin(as<glm::dmat4>(kEndMatrix));
    setOutput(kStartPoint, a);
    setOutput(kEndPoint, b);
    setOutput(kDistance, glm::distance(a, b));
    setOutput(kDelta, b - a);
}

void AngleMeasure::compute(const EvalContext& ctx)
{
    Transform::compute(ctx);
    double angle = 0.0;
    try
    {
        angle = measure::angleDegrees(origin(as<glm::dmat4>(kPoint1Matrix)), origin(as<glm::dmat4>(kVertexMatrix)),
                                      origin(as<glm::dmat4>(kPoint2Matrix)));
    }
    catch (const std::invalid_argument&)
    {
    }
    setOutput(kAngle, angle);
}

void RadiusMeasure::compute(const EvalContext& ctx)
{
    LILA_ZONE();

    Transform::compute(ctx);
    std::vector<glm::dvec3> pts;
    if (const MeshPtr& mesh = as<MeshPtr>(kInMesh))
    {
        glm::dmat4 world = as<glm::dmat4>(kMeshMatrix);
        for (uint32_t id : parseIds(as<std::string>(kVertices)))
            if (mesh->pointValid(id))
                pts.push_back(glm::dvec3(world * glm::dvec4(mesh->position(id), 1.0)));
    }
    else
    {
        json j = json::parse(as<std::string>(kPoints), nullptr, false);
        if (j.is_array())
            for (const json& p : j)
                if (p.is_array() && p.size() == 3 && p[0].is_number())
                    pts.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
    }
    measure::Circle c;
    bool valid = true;
    try
    {
        c = measure::fitCircle(pts);
    }
    catch (const std::invalid_argument&)
    {
        valid = false;
    }
    setOutput(kValid, valid);
    setOutput(kRadius, c.radius);
    setOutput(kDiameter, c.radius * 2.0);
    setOutput(kCircumference, c.radius * 2.0 * 3.14159265358979323846);
    setOutput(kCenter, c.center);
    setOutput(kNormal, c.normal);
    setOutput(kPlanarity, c.planarity);
    setOutput(kFitError, c.rms);
}

namespace measure
{
bool isMeasure(const Node& n) { return dynamic_cast<const MeasureNode*>(&n) != nullptr; }

bool shown() { return PrefStore::get().value<bool>("general", "/measure/show", true); }

std::string label(const Node& n)
{
    if (n.type().isA("distanceMeasure"))
        return formatLength(n.as<double>(DistanceMeasure::kDistance));
    if (n.type().isA("angleMeasure"))
        return formatAngle(n.as<double>(AngleMeasure::kAngle));
    if (n.type().isA("radiusMeasure"))
    {
        if (!n.as<bool>(RadiusMeasure::kValid))
            return "no circle";
        double r = n.as<double>(RadiusMeasure::kRadius);
        switch (n.as<int64_t>(RadiusMeasure::kDisplay))
        {
        case RadiusMeasure::kShowRadius:
            return "R " + formatLength(r);
        case RadiusMeasure::kShowDiameter:
            return "\xC3\x98 " + formatLength(2.0 * r);
        default:
            return "R " + formatLength(r) + "   \xC3\x98 " + formatLength(2.0 * r);
        }
    }
    return {};
}
} // namespace measure

void registerMeasureTypes()
{
    if (types::find("distanceMeasure"))
        return;
    const NodeType* transform = types::find("transform");

    NodeType d;
    d.name = "distanceMeasure";
    d.icon = "ruler-measure";
    d.base = transform;
    d.dag = true;
    d.ownAttrs = {
        matrixAttr("startMatrix", kStoredFlags | AttrHidden, "World matrix of the start point, usually a locator's worldMatrix."),
        matrixAttr("endMatrix", kStoredFlags | AttrHidden, "World matrix of the end point."),
        vec3Attr("startPoint", glm::dvec3(0.0), kOut, "World position of the start, cm."),
        vec3Attr("endPoint", glm::dvec3(0.0), kOut, "World position of the end, cm."),
        floatOut("distance", "Distance between the points, cm."),
        vec3Attr("delta", glm::dvec3(0.0), kOut, "End minus start, cm."),
    };
    d.create = [] { return std::make_unique<DistanceMeasure>(); };
    types::registerType(std::move(d));

    NodeType a;
    a.name = "angleMeasure";
    a.icon = "angle";
    a.base = transform;
    a.dag = true;
    a.ownAttrs = {
        matrixAttr("point1Matrix", kStoredFlags | AttrHidden, "World matrix of the first arm's end."),
        matrixAttr("vertexMatrix", kStoredFlags | AttrHidden, "World matrix of the angle's vertex."),
        matrixAttr("point2Matrix", kStoredFlags | AttrHidden, "World matrix of the second arm's end."),
        floatOut("angle", "Angle at the vertex, degrees."),
    };
    a.create = [] { return std::make_unique<AngleMeasure>(); };
    types::registerType(std::move(a));

    NodeType r;
    r.name = "radiusMeasure";
    r.icon = "circle-dashed";
    r.base = transform;
    r.dag = true;
    r.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
         .doc = "Mesh whose vertices are fitted, connected from a mesh's outMesh."},
        matrixAttr("meshMatrix", kStoredFlags | AttrHidden, "World matrix of the mesh."),
        {.name = "vertices", .type = PlugType::String, .defaultValue = std::string(), .flags = kStoredFlags,
         .doc = "Vertex ids on inMesh, space separated."},
        {.name = "points", .type = PlugType::String, .defaultValue = std::string("[]"), .flags = kStoredFlags,
         .doc = "Fixed world points as a JSON list of [x, y, z], used when no mesh is connected."},
        {.name = "display", .type = PlugType::Enum, .defaultValue = int64_t(RadiusMeasure::kShowBoth), .flags = kStoredFlags,
         .enumNames = {"radius", "diameter", "both"}, .doc = "What the viewport label shows."},
        {.name = "valid", .type = PlugType::Bool, .defaultValue = false, .flags = kOut, .doc = "False when the points don't define a circle."},
        floatOut("radius", "Best-fit radius, cm."),
        floatOut("diameter", "Best-fit diameter, cm."),
        floatOut("circumference", "Circumference of the fitted circle, cm."),
        vec3Attr("center", glm::dvec3(0.0), kOut, "World centre of the circle."),
        vec3Attr("normal", glm::dvec3(0.0), kOut, "Normal of the circle's plane."),
        floatOut("planarity", "Largest distance of a point from the circle's plane, cm."),
        floatOut("fitError", "RMS radial distance of the points from the circle, cm."),
    };
    r.create = [] { return std::make_unique<RadiusMeasure>(); };
    types::registerType(std::move(r));
}

namespace
{
PlugRef worldPlug(NodeId id) { return {id, Transform::kWorldMatrix, -1}; }

/** A world-space point picked from components, remembering the mesh it sits on so its locator can follow it. */
struct Picked
{
    glm::dvec3 world{0.0};
    NodeId mesh = kNoNode;
};

std::vector<Picked> pickedComponentPoints()
{
    std::vector<Picked> out;
    if (components::mode() == ComponentMode::Object)
        return out;
    Graph& g = graph();
    g.evaluate();
    for (const auto& [id, ids] : components::selected(components::mode()))
    {
        (void)ids;
        Node* n = g.node(id);
        if (!n || !n->type().isA("mesh") || !g.isSelected(id))
            continue;
        MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        glm::dmat4 w = worldOf(*n);
        for (uint32_t p : components::pointsOf(id))
            if (m->pointValid(p))
                out.push_back({glm::dvec3(w * glm::dvec4(m->position(p), 1.0)), id});
    }
    return out;
}

NodeId measureLocator(const std::string& name, const glm::dvec3& world, NodeId parent)
{
    Graph& g = graph();
    Node* loc = guarded([&] { return g.createNode(*types::find("locator"), name, parent); });
    g.evaluate();
    glm::dmat4 parentInv = glm::inverse(loc->as<glm::dmat4>(Transform::kParentMatrix));
    g.setValue({loc->id(), Transform::kTranslate}, clean(glm::dvec3(parentInv * glm::dvec4(world, 1.0))));
    g.setValue({loc->id(), Locator::kLocalScale}, glm::dvec3(0.5));
    return loc->id();
}

/**
 * An endpoint given as a command argument: an object name (measured from its origin, no locator) or [x, y, z] (a
 * new locator there, under `parent` when given).
 */
NodeId endpoint(const json& spec, const json& parent, const std::string& locName, const char* cmd)
{
    if (spec.is_string())
    {
        Node& n = nodeNamed(spec.get<std::string>(), cmd);
        if (!n.type().isA("transform"))
            throw CommandError(std::string(cmd) + ": " + n.path() + " has no position");
        return n.id();
    }
    if (!spec.is_array() || spec.size() != 3)
        throw CommandError(std::string(cmd) + ": a point is an object name or [x, y, z], got " + spec.dump());
    NodeId p = kNoNode;
    if (parent.is_string() && !parent.get<std::string>().empty())
        p = nodeNamed(parent.get<std::string>(), cmd).id();
    return measureLocator(locName, vec3Of(spec), p);
}

/** Endpoints from the selection: picked components (locators attached to their meshes) or the selected objects. */
std::vector<NodeId> endpointsFromSelection(const Args& a, size_t want, const std::string& prefix, const char* cmd)
{
    std::vector<NodeId> out;
    std::vector<Picked> picked = pickedComponentPoints();
    bool attach = a.get<bool>("attach", true);
    if (!picked.empty())
    {
        if (picked.size() != want)
            throw CommandError(std::string(cmd) + ": select " + std::to_string(want) + " vertices, not " + std::to_string(picked.size()));
        for (size_t i = 0; i < want; ++i)
            out.push_back(measureLocator(prefix + std::to_string(i + 1), picked[i].world, attach ? picked[i].mesh : kNoNode));
        return out;
    }
    std::vector<std::string> objs = a.objects();
    if (objs.size() != want)
        throw CommandError(std::string(cmd) + ": give " + std::to_string(want) + " points or select " + std::to_string(want) +
                           " objects or vertices");
    for (const std::string& name : objs)
        out.push_back(endpoint(json(name), nullptr, "", cmd));
    return out;
}

Node& createMeasure(const char* type, const Args& a, const char* fallbackName)
{
    Graph& g = graph();
    return *guarded([&] { return g.createNode(*types::find(type), a.get<std::string>("name", fallbackName)); });
}

void connectPoint(NodeId source, NodeId measure, int attr)
{
    guarded([&] {
        graph().connect(worldPlug(source), {measure, attr, -1});
        return 0;
    });
}

json finish(Node& n, const Args& a)
{
    if (!a.get<bool>("skipSelect", false))
        graph().select({n.id()});
    return n.path();
}

std::vector<glm::dvec3> positionsArg(const json& j, const char* cmd)
{
    if (!j.is_array())
        throw CommandError(std::string(cmd) + ": points must be a list of [x, y, z]");
    std::vector<glm::dvec3> out;
    for (const json& p : j)
        out.push_back(vec3Of(p));
    return out;
}

void registerCreateCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "distanceMeasure",
        .doc = "Live distance between two points. Each point is an object name or [x, y, z] (a locator is made there); with "
               "neither given, measures between two selected objects or vertices (vertex locators are parented to their "
               "mesh). Returns the measure node.",
        .positionalDoc = "objects",
        .flags = {
            {"start", "sp", T::Any, nullptr, "Start: object name or [x, y, z]."},
            {"end", "ep", T::Any, nullptr, "End: object name or [x, y, z]."},
            {"startParent", "spp", T::String, nullptr, "Parent for the start locator, so it follows that object."},
            {"endParent", "epp", T::String, nullptr, "Parent for the end locator."},
            {"attach", "at", T::Bool, true, "From selected vertices: parent the locators to their meshes."},
            {"name", "n", T::String, nullptr, "Name for the measure node."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new node."},
        },
        .run = [](const Args& a) -> json {
            const char* cmd = "distanceMeasure";
            std::vector<NodeId> ends;
            if (a.has("start") || a.has("end"))
            {
                if (!a.has("start") || !a.has("end"))
                    throw CommandError("distanceMeasure: give both start and end");
                ends.push_back(endpoint(a.flags["start"], a.flags.value("startParent", json()), "distanceStart", cmd));
                ends.push_back(endpoint(a.flags["end"], a.flags.value("endParent", json()), "distanceEnd", cmd));
            }
            else
                ends = endpointsFromSelection(a, 2, "distancePoint", cmd);
            Node& n = createMeasure("distanceMeasure", a, "distanceMeasure");
            connectPoint(ends[0], n.id(), DistanceMeasure::kStartMatrix);
            connectPoint(ends[1], n.id(), DistanceMeasure::kEndMatrix);
            return finish(n, a);
        },
    });

    ops::registerCommand({
        .name = "angleMeasure",
        .doc = "Live angle at a vertex. points is [arm1, vertex, arm2], each an object name or [x, y, z]; with none given, "
               "uses three selected objects (the second is the vertex) or vertices (the one joined to both others by edges, "
               "else the corner opposite the longest side). Returns the measure node.",
        .positionalDoc = "objects",
        .flags = {
            {"points", "pts", T::List, nullptr, "[arm1, vertex, arm2]: object names or [x, y, z]."},
            {"parents", "pa", T::List, nullptr, "Parents for the locators made for [x, y, z] points (null to skip one)."},
            {"attach", "at", T::Bool, true, "From selected vertices: parent the locators to their meshes."},
            {"name", "n", T::String, nullptr, "Name for the measure node."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new node."},
        },
        .run = [](const Args& a) -> json {
            const char* cmd = "angleMeasure";
            std::vector<NodeId> pts;
            if (a.has("points"))
            {
                const json& p = a.flags["points"];
                if (p.size() != 3)
                    throw CommandError("angleMeasure: points needs three entries");
                json parents = a.flags.value("parents", json::array());
                const char* names[] = {"anglePoint1", "angleVertex", "anglePoint2"};
                for (int i = 0; i < 3; ++i)
                    pts.push_back(endpoint(p[i], parents.is_array() && parents.size() > size_t(i) ? parents[i] : json(), names[i], cmd));
            }
            else
            {
                std::vector<Picked> picked = pickedComponentPoints();
                pts = endpointsFromSelection(a, 3, "anglePoint", cmd);
                if (!picked.empty())
                {
                    std::array<glm::dvec3, 3> w = {picked[0].world, picked[1].world, picked[2].world};
                    int v = measure::cornerOf(w);
                    // Topology wins over geometry: a vertex joined to both others is the corner.
                    if (picked[0].mesh == picked[1].mesh && picked[1].mesh == picked[2].mesh)
                    {
                        Node* mn = graph().node(picked[0].mesh);
                        MeshPtr m = mn->as<MeshPtr>(MeshNode::kOutMesh);
                        components::Ids ids = components::pointsOf(picked[0].mesh);
                        for (int i = 0; i < 3 && ids.size() == 3; ++i)
                            if (m->findEdge(ids[i], ids[(i + 1) % 3]) != geo::kInvalid &&
                                m->findEdge(ids[i], ids[(i + 2) % 3]) != geo::kInvalid &&
                                m->findEdge(ids[(i + 1) % 3], ids[(i + 2) % 3]) == geo::kInvalid)
                                v = i;
                    }
                    std::swap(pts[1], pts[v]);
                }
            }
            Node& n = createMeasure("angleMeasure", a, "angleMeasure");
            connectPoint(pts[0], n.id(), AngleMeasure::kPoint1Matrix);
            connectPoint(pts[1], n.id(), AngleMeasure::kVertexMatrix);
            connectPoint(pts[2], n.id(), AngleMeasure::kPoint2Matrix);
            return finish(n, a);
        },
    });

    ops::registerCommand({
        .name = "radiusMeasure",
        .doc = "Best-fit circle (radius and diameter). Either fixed world points, a mesh plus vertex ids (live under edits), "
               "or with neither, the selected components of one mesh (an edge loop or ring of vertices). Returns the node.",
        .flags = {
            {"points", "pts", T::List, nullptr, "Fixed world points, [[x, y, z], ...], at least 3."},
            {"mesh", "m", T::String, nullptr, "Mesh whose vertices are fitted."},
            {"vertices", "vtx", T::List, nullptr, "Vertex ids on mesh."},
            {"display", "d", T::String, "both", "Label shows radius, diameter or both."},
            {"name", "n", T::String, nullptr, "Name for the measure node."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new node."},
        },
        .run = [](const Args& a) -> json {
            const char* cmd = "radiusMeasure";
            Graph& g = graph();
            std::string display = a.get<std::string>("display", "both");
            int64_t displayIndex = display == "radius" ? RadiusMeasure::kShowRadius
                                   : display == "diameter" ? RadiusMeasure::kShowDiameter
                                   : display == "both" ? RadiusMeasure::kShowBoth
                                                       : -1;
            if (displayIndex < 0)
                throw CommandError("radiusMeasure: display is radius, diameter or both");

            NodeId mesh = kNoNode;
            std::vector<uint32_t> verts;
            std::vector<glm::dvec3> fixed;
            if (a.has("points"))
                fixed = positionsArg(a.flags["points"], cmd);
            else if (a.has("mesh"))
            {
                mesh = nodeNamed(a.get<std::string>("mesh"), cmd).id();
                for (const json& v : a.flags.value("vertices", json::array()))
                    verts.push_back(v.get<uint32_t>());
            }
            else
            {
                std::map<NodeId, components::Ids> sel;
                if (components::mode() != ComponentMode::Object)
                    for (const auto& [id, ids] : components::selected(components::mode()))
                        if (!ids.empty() && g.isSelected(id))
                            sel[id] = ids;
                if (sel.size() != 1)
                    throw CommandError("radiusMeasure: select an edge loop or vertices on one mesh, or give points");
                mesh = sel.begin()->first;
                verts = components::pointsOf(mesh);
            }

            json pts = json::array();
            for (const glm::dvec3& p : fixed)
                pts.push_back(toJson3(p));
            if (mesh != kNoNode)
            {
                if (!g.node(mesh)->type().isA("mesh"))
                    throw CommandError(std::string("radiusMeasure: ") + g.node(mesh)->path() + " is not a mesh");
                if (verts.size() < 3)
                    throw CommandError("radiusMeasure: needs at least 3 vertices");
            }
            else if (fixed.size() < 3)
                throw CommandError("radiusMeasure: needs at least 3 points");

            Node& n = createMeasure("radiusMeasure", a, "radiusMeasure");
            g.setValue({n.id(), RadiusMeasure::kDisplay}, displayIndex);
            if (mesh != kNoNode)
            {
                g.setValue({n.id(), RadiusMeasure::kVertices}, joinIds(verts));
                guarded([&] {
                    g.connect({mesh, MeshNode::kOutMesh, -1}, {n.id(), RadiusMeasure::kInMesh, -1});
                    g.connect(worldPlug(mesh), {n.id(), RadiusMeasure::kMeshMatrix, -1});
                    return 0;
                });
            }
            else
                g.setValue({n.id(), RadiusMeasure::kPoints}, pts.dump());
            g.evaluate();
            if (!n.as<bool>(RadiusMeasure::kValid))
                log::warn("radiusMeasure: {} points don't define a circle (collinear?)", n.path());
            return finish(n, a);
        },
    });
}
} // namespace

void registerMeasureSelectionCommand();

void registerMeasureCommands()
{
    if (ops::find("distanceMeasure"))
        return;
    registerCreateCommands();
    registerMeasureSelectionCommand();
}
} // namespace lila::scene
