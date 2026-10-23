#include "ops/Command.h"
#include "core/Profile.h"
#include "geo/BoundingShapes.h"
#include "geo/Primitives.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/TransformOps.h"

#include <glm/gtc/constants.hpp>

#include <cstdio>
#include <limits>
#include <set>

namespace lila::scene
{
namespace
{
using T = ArgType;

struct Job
{
    NodeId owner = kNoNode;
    std::vector<glm::dvec3> points;
};

bool isCollisionName(const std::string& name)
{
    for (const char* prefix : {"UCX_", "UBX_", "USP_", "UCP_"})
        if (name.starts_with(prefix))
            return true;
    return false;
}

void addWorldPoints(const Node& n, const geo::Mesh& mesh, std::vector<glm::dvec3>& out)
{
    glm::dmat4 w = n.as<glm::dmat4>(Transform::kWorldMatrix);
    for (geo::Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            out.push_back(glm::dvec3(w * glm::dvec4(mesh.position(p), 1.0)));
}

/** One job per mesh: whole meshes at or under the selection, or the selected components in a component mode. */
std::vector<Job> gather(Graph& g, const std::vector<std::string>& names)
{
    g.evaluate();
    std::vector<Job> jobs;
    ComponentMode mode = components::mode();
    if (mode != ComponentMode::Object)
    {
        for (auto& [id, ids] : components::selected(mode))
        {
            Node* n = g.node(id);
            MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, id) : nullptr;
            if (!cage || ids.empty())
                continue;
            Job job{id, {}};
            glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            for (geo::Index p : components::convert(*cage, mode, ids, ComponentMode::Vertex))
                if (cage->pointValid(p))
                    job.points.push_back(glm::dvec3(w * glm::dvec4(cage->position(p), 1.0)));
            jobs.push_back(std::move(job));
        }
        return jobs;
    }
    std::set<NodeId> seen;
    for (const std::string& name : names)
    {
        Node* top = g.find(name);
        if (!top)
            throw CommandError("boundingMesh: no node named '" + name + "'");
        std::vector<NodeId> ids{top->id()};
        for (NodeId d : g.descendants(top->id()))
            ids.push_back(d);
        for (NodeId id : ids)
        {
            Node* n = g.node(id);
            if (!n->type().isA("mesh") || isCollisionName(n->name()) || !seen.insert(id).second)
                continue;
            if (MeshPtr mesh = n->as<MeshPtr>(MeshNode::kOutMesh))
            {
                Job job{id, {}};
                addWorldPoints(*n, *mesh, job.points);
                if (!job.points.empty())
                    jobs.push_back(std::move(job));
            }
        }
    }
    return jobs;
}

std::string autoPrefix(const std::string& shape)
{
    if (shape == "box")
        return "UBX_";
    if (shape == "sphere")
        return "USP_";
    if (shape == "capsule")
        return "UCP_";
    return "UCX_";
}

/** UE pairs UCX_Name, UCX_Name_01, UCX_Name_02... with the mesh Name, so extra shapes get a two-digit suffix. */
std::string freeName(const Graph& g, const std::string& base)
{
    if (!g.find(base))
        return base;
    for (int i = 1;; ++i)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof suffix, "_%02d", i);
        if (!g.find(base + suffix))
            return base + suffix;
    }
}

glm::dmat3 rotationOf(const Node& n)
{
    glm::dmat3 m(n.as<glm::dmat4>(Transform::kWorldMatrix));
    for (int i = 0; i < 3; ++i)
        m[i] = glm::length(m[i]) > 1e-12 ? glm::normalize(m[i]) : glm::dvec3(0.0);
    m[2] = glm::normalize(glm::cross(m[0], m[1]));
    m[1] = glm::cross(m[2], m[0]);
    return m;
}

struct Placed
{
    geo::Mesh mesh;
    glm::dmat3 axes{1.0};
    glm::dvec3 center{0.0};
    /** Generator node type and its settings, for history. Empty for the hull. */
    std::string generator;
    json settings;
};

Placed solve(Graph& g, const Job& job, const Args& a)
{
    const std::string shape = a.get<std::string>("shape", "box");
    const std::string orient = a.get<std::string>("orient", "fit");
    const double offset = a.get<double>("offset", 0.0);
    glm::dvec3 axisOffset(0.0);
    if (a.has("axisOffset"))
        axisOffset = {a.flags["axisOffset"][0], a.flags["axisOffset"][1], a.flags["axisOffset"][2]};
    const int segments = std::max(3, a.get<int>("segments", 16));

    if (shape == "hull")
    {
        Placed p;
        p.mesh = geo::convexHullMesh(job.points, a.get<int>("maxHullPoints", 32), offset);
        if (p.mesh.numFaces() == 0)
            throw CommandError("boundingMesh: the points are flat, so they have no convex hull");
        glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-lo);
        for (geo::Index i = 0; i < p.mesh.pointCapacity(); ++i)
            if (p.mesh.pointValid(i))
                lo = glm::min(lo, p.mesh.position(i)), hi = glm::max(hi, p.mesh.position(i));
        p.center = (lo + hi) * 0.5;
        for (geo::Index i = 0; i < p.mesh.pointCapacity(); ++i)
            if (p.mesh.pointValid(i))
                p.mesh.setPosition(i, p.mesh.position(i) - p.center);
        return p;
    }

    geo::OrientedBox box;
    if (orient == "world")
        box = geo::boxInFrame(job.points, glm::dmat3(1.0));
    else if (orient == "object")
        box = geo::boxInFrame(job.points, rotationOf(*g.node(job.owner)));
    else if (orient == "fit")
    {
        std::optional<geo::OrientedBox> fit = geo::minimumVolumeBox(job.points);
        if (!fit)
            throw CommandError("boundingMesh: need at least two points");
        box = *fit;
    }
    else
        throw CommandError("boundingMesh: orient must be fit, world or object, got '" + orient + "'");

    if (shape == "box")
    {
        glm::dvec3 size = glm::max(box.size + 2.0 * (glm::dvec3(offset) + axisOffset), glm::dvec3(1e-4));
        return {geo::makeCube({size.x, size.y, size.z}), box.axes, box.center, "polyCube",
                {{"width", size.x}, {"height", size.y}, {"depth", size.z}}};
    }
    if (shape == "sphere")
    {
        geo::RoundShape s = geo::minimumSphere(job.points);
        double r = std::max(s.radius + offset, 1e-4);
        int rings = std::max(2, segments / 2);
        return {geo::makeSphere({r, segments, rings}), orient == "fit" ? glm::dmat3(1.0) : box.axes, s.center, "polySphere",
                {{"radius", r}, {"subdivisionsAxis", segments}, {"subdivisionsHeight", rings}}};
    }

    const std::string axisName = a.get<std::string>("axis", "auto");
    int axis = axisName == "x" ? 0 : axisName == "y" ? 1 : axisName == "z" ? 2 : -1;
    if (axis < 0 && axisName != "auto")
        throw CommandError("boundingMesh: axis must be auto, x, y or z, got '" + axisName + "'");
    if (shape != "cylinder" && shape != "capsule")
        throw CommandError("boundingMesh: shape must be box, sphere, capsule, cylinder or hull, got '" + shape + "'");
    geo::RoundShape s = shape == "cylinder" ? geo::fitCylinder(job.points, box.axes, axis) : geo::fitCapsule(job.points, box.axes, axis);
    // The shape's own axes are a cyclic shift of the box's, so the box column it runs along is the matching pad.
    int along = 0;
    for (int i = 0; i < 3; ++i)
        if (std::abs(glm::dot(s.axes[1], box.axes[i])) > 0.5)
            along = i;
    double radialPad = offset + std::max(axisOffset[(along + 1) % 3], axisOffset[(along + 2) % 3]);
    double r = std::max(s.radius + radialPad, 1e-4);
    double h = std::max(s.height + 2.0 * (offset + axisOffset[along]), 1e-4);
    if (shape == "cylinder")
        return {geo::makeCylinder({r, h, segments}), s.axes, s.center, "polyCylinder",
                {{"radius", r}, {"height", h}, {"subdivisionsAxis", segments}}};
    h = std::max(h, 2.0 * r);
    int caps = std::max(2, segments / 4);
    return {geo::makeCapsule({r, h, segments, 1, caps}), s.axes, s.center, "polyCapsule",
            {{"radius", r}, {"height", h}, {"subdivisionsAxis", segments}, {"subdivisionsCaps", caps}}};
}

Node* build(Graph& g, Placed& p, const std::string& name, bool history)
{
    Node* mesh = g.createNode(*types::find("mesh"), freeName(g, name), kNoNode, false);
    if (history && !p.generator.empty())
    {
        const NodeType* genType = types::find(p.generator);
        Node* gen = g.createNode(*genType, {}, kNoNode, true);
        for (auto& [attr, value] : p.settings.items())
            g.setValue({gen->id(), genType->attrIndex(attr), -1}, fromJson(genType->attrs[genType->attrIndex(attr)], value));
        g.connect({gen->id(), static_cast<GeneratorNode*>(gen)->outAttr, -1}, {mesh->id(), MeshNode::kInMesh, -1});
    }
    else
        g.setValue({mesh->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(p.mesh))));
    g.setValue({mesh->id(), Transform::kTranslate, -1}, p.center);
    g.setValue({mesh->id(), Transform::kRotate, -1}, xformops::closestEuler(p.axes, RotateOrder::XYZ, glm::dvec3(0.0)));
    return mesh;
}
} // namespace

void registerBoundingMeshCommands()
{
    ops::registerCommand({
        .name = "boundingMesh",
        .doc = "Wrap the selection's points in a fitted shape, ready to go to Unreal as collision. Orientation is solved from the "
               "points alone (minimum-volume box), so frozen and unfrozen geometry behave the same, and every point is inside. "
               "Works on objects (every mesh at or under them) or on selected components. Returns the new meshes.",
        .positionalDoc = "objects",
        .flags = {
            {"shape", "s", T::String, "box", "box, sphere, capsule, cylinder or hull.", {"box", "sphere", "capsule", "cylinder", "hull"}},
            {"offset", "o", T::Float, 0.0, "Padding pushed out on every side, cm."},
            {"axisOffset", "ao", T::Vec3, nullptr, "Extra padding per box axis, added to offset, cm."},
            {"orient", "or", T::String, "fit", "fit (tightest box from the points), world or object (the owner's axes).",
             {"fit", "world", "object"}},
            {"axis", "ax", T::String, "auto", "Axis a capsule or cylinder runs along: auto (smallest), x, y or z of the box.",
             {"auto", "x", "y", "z"}},
            {"perObject", "po", T::Bool, false, "One shape per mesh instead of one around everything."},
            {"nameFromSelection", "ns", T::Bool, true, "Name after the mesh it wraps, so Unreal pairs it as that mesh's collision."},
            {"name", "n", T::String, "Collision", "Base name when not named from the selection."},
            {"prefix", "p", T::String, "auto", "Put before the name; auto picks Unreal's: UBX_, USP_, UCP_, or UCX_ for cylinders and hulls."},
            {"parent", "pa", T::Bool, true, "Parent under the mesh it wraps, keeping it in place."},
            {"segments", "sg", T::Int, 16, "Segments around a sphere, capsule or cylinder."},
            {"maxHullPoints", "mh", T::Int, 32, "Most points a hull keeps."},
            {"history", "h", T::Bool, false, "Keep the primitive's generator so its size stays editable."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.empty() && components::mode() == ComponentMode::Object)
                throw CommandError("boundingMesh: select some objects or components");
            std::vector<Job> jobs = gather(g, names);
            if (jobs.empty())
                throw CommandError("boundingMesh: no mesh points in the selection");
            if (!a.get<bool>("perObject", false) && jobs.size() > 1)
            {
                NodeId owner = jobs[0].owner;
                if (Node* first = names.empty() ? nullptr : g.find(names[0]); first && components::mode() == ComponentMode::Object)
                    owner = first->id();
                Job merged{owner, {}};
                for (Job& j : jobs)
                    merged.points.insert(merged.points.end(), j.points.begin(), j.points.end());
                jobs = {std::move(merged)};
            }

            std::string shape = a.get<std::string>("shape", "box");
            std::string prefix = a.get<std::string>("prefix", "auto");
            if (prefix == "auto")
                prefix = autoPrefix(shape);
            json out = json::array();
            std::vector<NodeId> made;
            for (const Job& job : jobs)
            {
                Placed placed = solve(g, job, a);
                std::string base = a.get<bool>("nameFromSelection", true) ? g.node(job.owner)->name() : a.get<std::string>("name", "Collision");
                if (auto colon = base.rfind(':'); colon != std::string::npos)
                    base = base.substr(colon + 1);
                Node* mesh = build(g, placed, prefix + base, a.get<bool>("history", false));
                if (a.get<bool>("parent", true))
                    g.setParent(mesh->id(), job.owner, true);
                out.push_back(mesh->name());
                made.push_back(mesh->id());
            }
            if (components::mode() != ComponentMode::Object)
                components::setMode(ComponentMode::Object);
            g.select(made);
            return out;
        },
    });
    ops::setAdjustable("boundingMesh");
}
} // namespace lila::scene
