#include "scene/Measure.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/MeasureMath.h"
#include "scene/MeshNodes.h"
#include "scene/RigUtil.h"

#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <set>

namespace lila::scene
{
using namespace rigutil;

namespace
{
using measure::formatAngle;
using measure::formatArea;
using measure::formatLength;

enum class Quantity
{
    Length,
    Area,
    Angle,
};

/** What measureSelection hands back: raw fields in cm (and degrees), plus the headline in display units. */
struct Result
{
    json fields = json::object();
    std::vector<std::string> lines;
    std::vector<std::pair<glm::dvec3, glm::dvec3>> segments; // drawn by the viewport HUD
    std::optional<measure::Circle> circle;

    void main(const char* kind, const char* field, Quantity q)
    {
        fields["kind"] = kind;
        fields["main"] = field;
        double f = measure::unitFactor(measure::displayUnit());
        double v = fields[field].get<double>();
        fields["value"] = q == Quantity::Length ? v * f : q == Quantity::Area ? v * f * f : v;
        fields["text"] = q == Quantity::Length ? formatLength(v) : q == Quantity::Area ? formatArea(v) : formatAngle(v);
    }

    json finish()
    {
        if (!fields.contains("kind"))
            fields["kind"] = "none";
        if (!fields.contains("text"))
            fields["text"] = lines.empty() ? std::string() : lines.front();
        fields["unit"] = measure::displayUnit();
        std::string report;
        for (const std::string& l : lines)
            report += (report.empty() ? "" : "\n") + l;
        fields["report"] = report;
        json segs = json::array();
        for (const auto& [a, b] : segments)
            segs.push_back({toJson3(a), toJson3(b)});
        fields["segments"] = segs;
        if (circle)
            fields["circle"] = {{"center", toJson3(circle->center)}, {"normal", toJson3(circle->normal)}, {"radius", circle->radius}};
        return fields;
    }
};

void addDistance(Result& r, const glm::dvec3& a, const glm::dvec3& b)
{
    glm::dvec3 d = b - a;
    r.fields["distance"] = glm::length(d);
    r.fields["dx"] = d.x;
    r.fields["dy"] = d.y;
    r.fields["dz"] = d.z;
    r.fields["start"] = toJson3(a);
    r.fields["end"] = toJson3(b);
    r.lines.push_back("Distance " + formatLength(glm::length(d)));
    r.lines.push_back("dx " + formatLength(d.x, false) + "   dy " + formatLength(d.y, false) + "   dz " + formatLength(d.z));
    r.segments.push_back({a, b});
    r.main("distance", "distance", Quantity::Length);
}

void addCircle(Result& r, const measure::Circle& c, const char* kind)
{
    double d = 2.0 * c.radius, circ = 2.0 * std::numbers::pi * c.radius;
    r.fields["radius"] = c.radius;
    r.fields["diameter"] = d;
    r.fields["circumference"] = circ;
    r.fields["center"] = toJson3(c.center);
    r.fields["normal"] = toJson3(c.normal);
    r.fields["planarity"] = c.planarity;
    r.fields["fitError"] = c.rms;
    r.fields["arc"] = c.arcDegrees;
    r.lines.push_back("\xC3\x98 " + formatLength(d) + "   R " + formatLength(c.radius));
    r.lines.push_back("Circumference " + formatLength(circ));
    r.lines.push_back("Planarity " + formatLength(c.planarity) + "   fit error " + formatLength(c.rms));
    r.circle = c;
    r.main(kind, "diameter", Quantity::Length);
}

void addAngle(Result& r, const std::array<glm::dvec3, 3>& p, int vertex)
{
    const glm::dvec3& v = p[vertex];
    const glm::dvec3& a = p[(vertex + 1) % 3];
    const glm::dvec3& b = p[(vertex + 2) % 3];
    double angle = measure::angleDegrees(a, v, b);
    r.fields["angle"] = angle;
    r.fields["vertex"] = toJson3(v);
    r.lines.push_back("Angle " + formatAngle(angle));
    r.lines.push_back("Arms " + formatLength(glm::distance(v, a)) + ", " + formatLength(glm::distance(v, b)));
    r.segments.push_back({v, a});
    r.segments.push_back({v, b});
    r.main("angle", "angle", Quantity::Angle);
}

struct MeshRef
{
    NodeId id = kNoNode;
    const Node* node = nullptr;
    MeshPtr mesh;
    glm::dmat4 world{1.0};

    glm::dvec3 at(geo::Index p) const { return glm::dvec3(world * glm::dvec4(mesh->position(p), 1.0)); }
};

std::optional<MeshRef> meshRef(NodeId id)
{
    Node* n = graph().node(id);
    if (!n || !n->type().isA("mesh"))
        return std::nullopt;
    MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m)
        return std::nullopt;
    return MeshRef{id, n, m, worldOf(*n)};
}

json measureVertices(const std::map<NodeId, components::Ids>& sel)
{
    Result r;
    std::vector<glm::dvec3> pts;
    std::vector<std::pair<const MeshRef*, uint32_t>> owners;
    std::vector<MeshRef> meshes;
    meshes.reserve(sel.size());
    for (const auto& [id, ids] : sel)
        if (auto m = meshRef(id))
        {
            meshes.push_back(*m);
            for (uint32_t p : ids)
                if (m->mesh->pointValid(p))
                {
                    pts.push_back(meshes.back().at(p));
                    owners.push_back({&meshes.back(), p});
                }
        }
    r.fields["count"] = pts.size();
    if (pts.empty())
        throw CommandError("measureSelection: nothing selected");
    if (pts.size() == 1)
    {
        r.fields["position"] = toJson3(pts[0]);
        r.lines.push_back("Point " + formatLength(pts[0].x, false) + ", " + formatLength(pts[0].y, false) + ", " + formatLength(pts[0].z));
        r.fields["kind"] = "point";
    }
    else if (pts.size() == 2)
        addDistance(r, pts[0], pts[1]);
    else if (pts.size() == 3)
    {
        std::array<glm::dvec3, 3> p = {pts[0], pts[1], pts[2]};
        int v = measure::cornerOf(p);
        if (owners[0].first == owners[1].first && owners[1].first == owners[2].first)
        {
            const geo::Mesh& m = *owners[0].first->mesh;
            for (int i = 0; i < 3; ++i)
            {
                auto edge = [&](int x, int y) { return m.findEdge(owners[x].second, owners[y].second) != geo::kInvalid; };
                if (edge(i, (i + 1) % 3) && edge(i, (i + 2) % 3) && !edge((i + 1) % 3, (i + 2) % 3))
                    v = i;
            }
        }
        try
        {
            addAngle(r, p, v);
            measure::Circle c = measure::fitCircle(pts);
            r.fields["radius"] = c.radius;
            r.fields["diameter"] = 2.0 * c.radius;
            r.lines.push_back("Circle through them: \xC3\x98 " + formatLength(2.0 * c.radius));
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(std::string("measureSelection: ") + e.what());
        }
    }
    else
    {
        try
        {
            addCircle(r, measure::fitCircle(pts), "circle");
        }
        catch (const std::invalid_argument&)
        {
            // Collinear: the span between the two extremes along the line.
            size_t a = 0, b = 0;
            double best = -1.0;
            for (size_t i = 0; i < pts.size(); ++i)
                for (size_t j = i + 1; j < pts.size(); ++j)
                    if (double d = glm::distance(pts[i], pts[j]); d > best)
                    {
                        best = d;
                        a = i;
                        b = j;
                    }
            addDistance(r, pts[a], pts[b]);
            r.lines.push_back("The points are collinear: span between the farthest two");
        }
    }
    return r.finish();
}

json measureEdges(const std::map<NodeId, components::Ids>& sel)
{
    Result r;
    double total = 0.0;
    json perEdge = json::array();
    std::map<std::pair<NodeId, uint32_t>, int> degree;
    std::map<std::pair<NodeId, uint32_t>, std::vector<std::pair<NodeId, uint32_t>>> links;
    std::map<std::pair<NodeId, uint32_t>, glm::dvec3> where;
    int count = 0;
    for (const auto& [id, ids] : sel)
    {
        auto m = meshRef(id);
        if (!m)
            continue;
        for (uint32_t e : ids)
        {
            if (!m->mesh->edgeValid(e))
                continue;
            geo::Index p0 = m->mesh->edgePoint(e, 0), p1 = m->mesh->edgePoint(e, 1);
            glm::dvec3 a = m->at(p0), b = m->at(p1);
            double len = glm::distance(a, b);
            total += len;
            ++count;
            perEdge.push_back({{"node", m->node->path()}, {"edge", e}, {"length", len}});
            r.segments.push_back({a, b});
            std::pair<NodeId, uint32_t> k0{id, p0}, k1{id, p1};
            ++degree[k0];
            ++degree[k1];
            links[k0].push_back(k1);
            links[k1].push_back(k0);
            where[k0] = a;
            where[k1] = b;
        }
    }
    if (count == 0)
        throw CommandError("measureSelection: nothing selected");
    r.fields["length"] = total;
    r.fields["count"] = count;
    r.fields["edges"] = perEdge;

    // One closed loop: every point has two selected edges and they all connect.
    bool loop = count >= 3 && std::all_of(degree.begin(), degree.end(), [](const auto& d) { return d.second == 2; });
    if (loop)
    {
        std::set<std::pair<NodeId, uint32_t>> seen;
        std::vector<std::pair<NodeId, uint32_t>> stack = {degree.begin()->first};
        while (!stack.empty())
        {
            auto k = stack.back();
            stack.pop_back();
            if (!seen.insert(k).second)
                continue;
            for (const auto& n : links[k])
                stack.push_back(n);
        }
        loop = seen.size() == degree.size();
    }
    r.fields["closed"] = loop;
    if (loop)
    {
        std::vector<glm::dvec3> pts;
        for (const auto& [k, p] : where)
            pts.push_back(p);
        try
        {
            addCircle(r, measure::fitCircle(pts), "loop");
            r.lines.insert(r.lines.begin() + 2, "Loop length " + formatLength(total) + " (" + std::to_string(count) + " edges)");
            return r.finish();
        }
        catch (const std::invalid_argument&)
        {
        }
    }
    r.lines.push_back("Length " + formatLength(total) + (count > 1 ? " (" + std::to_string(count) + " edges)" : ""));
    if (count == 1)
    {
        glm::dvec3 d = r.segments[0].second - r.segments[0].first;
        r.fields["distance"] = total;
        r.lines.push_back("dx " + formatLength(d.x, false) + "   dy " + formatLength(d.y, false) + "   dz " + formatLength(d.z));
    }
    constexpr int kListed = 12;
    for (int i = 0; i < count && count > 1 && i < kListed; ++i)
        r.lines.push_back("  " + perEdge[i]["node"].get<std::string>() + ".e[" + std::to_string(perEdge[i]["edge"].get<uint32_t>()) +
                          "]  " + formatLength(perEdge[i]["length"].get<double>()));
    if (count > kListed)
        r.lines.push_back("  ... " + std::to_string(count - kListed) + " more");
    r.main("length", "length", Quantity::Length);
    return r.finish();
}

json measureFaces(const std::map<NodeId, components::Ids>& sel)
{
    Result r;
    double area = 0.0, perimeter = 0.0;
    int count = 0;
    for (const auto& [id, ids] : sel)
    {
        auto m = meshRef(id);
        if (!m)
            continue;
        std::map<uint32_t, int> edgeUse;
        for (uint32_t f : ids)
        {
            if (!m->mesh->faceValid(f))
                continue;
            ++count;
            std::vector<glm::dvec3> poly;
            for (geo::Index p : m->mesh->facePoints(f))
                poly.push_back(m->at(p));
            area += measure::polygonArea(poly);
            m->mesh->forEachFaceCorner(f, [&](geo::Index c) { ++edgeUse[m->mesh->cornerEdge(c)]; });
        }
        for (const auto& [e, uses] : edgeUse)
            if (uses == 1)
            {
                glm::dvec3 a = m->at(m->mesh->edgePoint(e, 0)), b = m->at(m->mesh->edgePoint(e, 1));
                perimeter += glm::distance(a, b);
                r.segments.push_back({a, b});
            }
    }
    if (count == 0)
        throw CommandError("measureSelection: nothing selected");
    r.fields["area"] = area;
    r.fields["perimeter"] = perimeter;
    r.fields["length"] = perimeter;
    r.fields["count"] = count;
    r.lines.push_back("Area " + formatArea(area) + (count > 1 ? " (" + std::to_string(count) + " faces)" : ""));
    r.lines.push_back("Perimeter " + formatLength(perimeter));
    r.main("area", "area", Quantity::Area);
    return r.finish();
}

/** World position of an object's rotate pivot, where Maya measures objects from. */
glm::dvec3 pivotOf(const Node& n)
{
    glm::dvec3 local = n.as<glm::dvec3>(Transform::kTranslate) + n.as<glm::dvec3>(Transform::kRotatePivotTranslate) +
                       n.as<glm::dvec3>(Transform::kRotatePivot);
    return glm::dvec3(n.as<glm::dmat4>(Transform::kParentMatrix) * glm::dvec4(local, 1.0));
}

struct Cylinder
{
    double radius = 0.0;
    double height = 0.0;
    int axis = 1;
};

/**
 * Whether a mesh reads as a cylinder (or a sphere) about one of its own axes: square across the other two, with the
 * outermost points on a circle as wide as that square. Boxes and coarse prisms fail on the second test.
 */
std::optional<Cylinder> cylinderOf(const std::vector<glm::dvec3>& pts, const glm::dmat4& world)
{
    std::optional<Cylinder> best;
    double bestScore = 1e300;
    for (int k = 0; k < 3; ++k)
    {
        glm::dvec3 axis(world[k]);
        if (glm::length(axis) < 1e-12)
            continue;
        axis = glm::normalize(axis);
        glm::dvec3 u = glm::normalize(glm::dvec3(world[(k + 1) % 3])), v = glm::normalize(glm::dvec3(world[(k + 2) % 3]));
        glm::dvec2 lo(1e300), hi(-1e300);
        double hlo = 1e300, hhi = -1e300;
        for (const glm::dvec3& p : pts)
        {
            glm::dvec2 q(glm::dot(p, u), glm::dot(p, v));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
            hlo = std::min(hlo, glm::dot(p, axis));
            hhi = std::max(hhi, glm::dot(p, axis));
        }
        glm::dvec2 ext = hi - lo;
        double side = std::max(ext.x, ext.y);
        if (side < 1e-9 || std::abs(ext.x - ext.y) > 0.02 * side)
            continue;
        glm::dvec2 c = (lo + hi) * 0.5;
        double rmax = 0.0;
        for (const glm::dvec3& p : pts)
            rmax = std::max(rmax, glm::length(glm::dvec2(glm::dot(p, u), glm::dot(p, v)) - c));
        if (std::abs(2.0 * rmax - side) > 0.03 * side)
            continue;
        std::vector<glm::dvec3> outer;
        for (const glm::dvec3& p : pts)
        {
            glm::dvec2 q(glm::dot(p, u), glm::dot(p, v));
            if (glm::length(q - c) > 0.97 * rmax)
                outer.push_back(u * q.x + v * q.y);
        }
        if (outer.size() < 8)
            continue;
        try
        {
            measure::Circle circle = measure::fitCircle(outer);
            if (circle.arcDegrees < 300.0)
                continue;
            double score = circle.rms / circle.radius;
            if (score < bestScore)
            {
                bestScore = score;
                best = Cylinder{circle.radius, hhi - hlo, k};
            }
        }
        catch (const std::invalid_argument&)
        {
        }
    }
    return best;
}

json measureMeasureNode(const Node& n)
{
    Result r;
    r.fields["node"] = n.path();
    if (n.type().isA("distanceMeasure"))
        addDistance(r, n.as<glm::dvec3>(DistanceMeasure::kStartPoint), n.as<glm::dvec3>(DistanceMeasure::kEndPoint));
    else if (n.type().isA("angleMeasure"))
        addAngle(r,
                 {glm::dvec3(n.as<glm::dmat4>(AngleMeasure::kVertexMatrix)[3]), glm::dvec3(n.as<glm::dmat4>(AngleMeasure::kPoint1Matrix)[3]),
                  glm::dvec3(n.as<glm::dmat4>(AngleMeasure::kPoint2Matrix)[3])},
                 0);
    else if (n.type().isA("radiusMeasure"))
    {
        if (!n.as<bool>(RadiusMeasure::kValid))
            throw CommandError("measureSelection: " + n.path() + " has no valid circle");
        measure::Circle c;
        c.center = n.as<glm::dvec3>(RadiusMeasure::kCenter);
        c.normal = n.as<glm::dvec3>(RadiusMeasure::kNormal);
        c.radius = n.as<double>(RadiusMeasure::kRadius);
        c.planarity = n.as<double>(RadiusMeasure::kPlanarity);
        c.rms = n.as<double>(RadiusMeasure::kFitError);
        c.arcDegrees = 360.0;
        addCircle(r, c, "circle");
    }
    return r.finish();
}

json measureObjects(const std::vector<NodeId>& ids)
{
    Graph& g = graph();
    if (ids.empty())
        throw CommandError("measureSelection: nothing selected");
    if (ids.size() == 1 && measure::isMeasure(*g.node(ids[0])))
        return measureMeasureNode(*g.node(ids[0]));

    Result r;
    std::vector<glm::dvec3> pivots;
    for (NodeId id : ids)
        if (Node* n = g.node(id); n && n->type().isA("transform"))
            pivots.push_back(pivotOf(*n));
    if (ids.size() == 2 && pivots.size() == 2)
    {
        addDistance(r, pivots[0], pivots[1]);
        return r.finish();
    }
    if (ids.size() == 3 && pivots.size() == 3)
    {
        try
        {
            addAngle(r, {pivots[1], pivots[2], pivots[0]}, 0);
        }
        catch (const std::invalid_argument& e)
        {
            throw CommandError(std::string("measureSelection: ") + e.what());
        }
        return r.finish();
    }

    geo::Bounds b;
    std::vector<glm::dvec3> single;
    for (NodeId id : ids)
    {
        if (auto m = meshRef(id))
        {
            for (geo::Index p = 0; p < m->mesh->pointCapacity(); ++p)
                if (m->mesh->pointValid(p))
                {
                    glm::dvec3 w = m->at(p);
                    b.extend(w);
                    if (ids.size() == 1)
                        single.push_back(w);
                }
        }
        else if (Node* n = g.node(id); n && n->type().isA("transform"))
            b.extend(glm::dvec3(worldOf(*n)[3]));
    }
    if (!b.valid())
        throw CommandError("measureSelection: nothing measurable selected");
    glm::dvec3 size = b.size();
    if (ids.size() == 1 && single.empty())
    {
        r.fields["position"] = toJson3(b.min);
        r.fields["kind"] = "point";
        r.lines.push_back("Position " + formatLength(b.min.x, false) + ", " + formatLength(b.min.y, false) + ", " + formatLength(b.min.z));
        return r.finish();
    }
    r.fields["width"] = size.x;
    r.fields["height"] = size.y;
    r.fields["depth"] = size.z;
    r.fields["min"] = toJson3(b.min);
    r.fields["max"] = toJson3(b.max);
    r.fields["center"] = toJson3(b.center());
    std::string dims = formatLength(size.x, false) + " x " + formatLength(size.y, false) + " x " + formatLength(size.z, false);
    r.lines.push_back("Size " + dims + " " + measure::displayUnit());
    r.lines.push_back("W " + formatLength(size.x) + "   H " + formatLength(size.y) + "   D " + formatLength(size.z));
    r.fields["kind"] = "bounds";
    r.fields["main"] = "size";
    r.fields["text"] = dims + " " + measure::displayUnit();
    r.fields["value"] = dims;
    // The box's twelve edges, for the HUD.
    for (int axis = 0; axis < 3; ++axis)
        for (int s = 0; s < 4; ++s)
        {
            glm::dvec3 a = b.min, c;
            a[(axis + 1) % 3] = (s & 1) ? b.max[(axis + 1) % 3] : b.min[(axis + 1) % 3];
            a[(axis + 2) % 3] = (s & 2) ? b.max[(axis + 2) % 3] : b.min[(axis + 2) % 3];
            c = a;
            c[axis] = b.max[axis];
            r.segments.push_back({a, c});
        }
    if (ids.size() == 1)
        if (auto cyl = cylinderOf(single, worldOf(*g.node(ids[0]))))
        {
            const char* axisName[] = {"x", "y", "z"};
            r.fields["radius"] = cyl->radius;
            r.fields["diameter"] = 2.0 * cyl->radius;
            r.fields["cylinderHeight"] = cyl->height;
            r.fields["cylinderAxis"] = axisName[cyl->axis];
            r.lines.insert(r.lines.begin(), "Cylinder \xC3\x98 " + formatLength(2.0 * cyl->radius) + "   height " + formatLength(cyl->height) +
                                                " (local " + axisName[cyl->axis] + ")");
            r.main("cylinder", "diameter", Quantity::Length);
        }
    return r.finish();
}
} // namespace

void registerMeasureSelectionCommand()
{
    ops::registerCommand({
        .name = "measureSelection",
        .doc = "Measure the selection without making nodes: 2 vertices or objects give distance and dx/dy/dz, 3 an angle, "
               "edges total and per-edge length, a closed edge loop or 4+ vertices a best-fit circle, faces area and "
               "perimeter, one object its bounding box (and diameter when it is cylinder-like), a measure node its value. "
               "Returns a dict: lengths in cm; 'value' and 'text' are the headline in the display unit (/units/linear).",
        .undoable = false,
        .run = [](const Args&) -> json {
            LILA_ZONE();

            Graph& g = graph();
            g.evaluate();
            ComponentMode mode = components::mode();
            if (mode != ComponentMode::Object)
            {
                // Only meshes still in the object selection are live; components left on others are stale.
                std::map<NodeId, components::Ids> sel;
                for (const auto& [id, ids] : components::selected(mode))
                    if (!ids.empty() && g.isSelected(id))
                        sel[id] = ids;
                if (!sel.empty())
                {
                    if (mode == ComponentMode::Vertex)
                        return measureVertices(sel);
                    if (mode == ComponentMode::Edge)
                        return measureEdges(sel);
                    return measureFaces(sel);
                }
            }
            return measureObjects(g.selection());
        },
    });
}
} // namespace lila::scene
