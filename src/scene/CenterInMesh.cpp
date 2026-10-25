#include "scene/CenterInMesh.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/TransformOps.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace lila::scene
{
using namespace rigutil;

namespace
{
std::vector<std::string> splitNames(const std::string& s)
{
    std::string spaced = s;
    std::replace(spaced.begin(), spaced.end(), ',', ' ');
    std::istringstream in(spaced);
    std::vector<std::string> out;
    for (std::string w; in >> w;)
        out.push_back(w);
    return out;
}

bool isMesh(const Node* n) { return n && n->type().isA("mesh"); }

/**
 * The stored centerMeshes list. The id is authoritative while the mesh lives; the name binds entries read from a file
 * (relative to the owner's namespace first, as references store them).
 */
struct CenterMeshList final : Data
{
    struct Entry
    {
        // Binding cache, refreshed by resolve(); the entry's meaning never changes.
        mutable NodeId id = kNoNode;
        mutable std::string name;
    };
    std::vector<Entry> entries;

    const char* kind() const override { return rig::kCenterMeshListKind; }
    json toJson() const override
    {
        const Graph& g = Graph::current();
        json names = json::array();
        for (const Entry& e : entries)
        {
            const Node* n = e.id != kNoNode ? g.node(e.id) : nullptr;
            if (n)
                names.push_back(n->path());
            else if (e.id == kNoNode)
                names.push_back(e.name);
        }
        return {{"kind", kind()}, {"meshes", std::move(names)}};
    }

    static DataPtr fromJson(const json& j)
    {
        auto out = std::make_shared<CenterMeshList>();
        auto addName = [&](const std::string& name) { out->entries.push_back({kNoNode, name}); };
        if (j.contains("meshes") && j["meshes"].is_array())
        {
            for (const json& m : j["meshes"])
                if (m.is_string())
                    addName(m.get<std::string>());
        }
        else if (j.contains("value") && j["value"].is_string())
            for (const std::string& name : splitNames(j["value"].get<std::string>()))
                addName(name);
        return out;
    }

    std::vector<NodeId> resolve(const Graph& g, const std::string& ownerNamespace) const
    {
        std::vector<NodeId> out;
        for (const Entry& e : entries)
        {
            const Node* n = nullptr;
            if (e.id != kNoNode)
                n = g.node(e.id); // a deleted mesh stays unbound
            else
                try
                {
                    if (!ownerNamespace.empty())
                        n = g.find(ownerNamespace + ":" + e.name);
                    if (!n)
                        n = g.find(e.name);
                }
                catch (const std::exception&)
                {
                    continue; // ambiguous: stays unbound until the name is made unique
                }
            if (!isMesh(n))
                continue;
            e.id = n->id();
            e.name = n->path();
            out.push_back(n->id());
        }
        return out;
    }
};

bool influences(Graph& g, NodeId skin, NodeId joint)
{
    for (const skin::Influence& inf : skin::influences(g, skin))
        if (inf.joint == joint)
            return true;
    return false;
}

std::vector<NodeId> targets(Graph&, const Args& a, const char* cmd)
{
    std::vector<NodeId> out;
    for (const std::string& name : a.objects())
    {
        Node& n = nodeNamed(name, cmd);
        if (dynamic_cast<Transform*>(&n))
            out.push_back(n.id());
    }
    return out;
}

/** World positions of the selected components' points; every kind with a selection in object mode. */
std::vector<glm::dvec3> componentPoints(Graph& g)
{
    g.evaluate();
    std::vector<ComponentMode> kinds;
    if (components::mode() == ComponentMode::Object)
        kinds = {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face};
    else
        kinds = {components::mode()};
    std::vector<glm::dvec3> out;
    for (ComponentMode kind : kinds)
        for (auto& [id, ids] : components::selected(kind))
        {
            Node* n = g.node(id);
            const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr;
            if (pp && kind == ComponentMode::Vertex)
            {
                std::unordered_set<uint32_t> want(ids.begin(), ids.end());
                glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
                for (const auto& [cid, p] : pp->points(g, id))
                    if (want.contains(cid))
                        out.push_back(glm::dvec3(w * glm::dvec4(p, 1.0)));
                continue;
            }
            MeshPtr cage = isMesh(n) ? modifiers::cage(g, id) : nullptr;
            if (!cage || ids.empty())
                continue;
            glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            for (geo::Index p : components::convert(*cage, kind, ids, ComponentMode::Vertex))
                if (cage->pointValid(p))
                    out.push_back(glm::dvec3(w * glm::dvec4(cage->position(p), 1.0)));
        }
    return out;
}
} // namespace

namespace rig
{
DataPtr makeCenterMeshList(const Graph& g, const std::vector<NodeId>& meshes)
{
    auto out = std::make_shared<CenterMeshList>();
    for (NodeId id : meshes)
        if (const Node* n = g.node(id))
            out->entries.push_back({id, n->path()});
    return out;
}

void registerCenterMeshListKind() { data::registerKind(kCenterMeshListKind, &CenterMeshList::fromJson); }

bool centerEnabled(const Node& n)
{
    int a = n.type().attrIndex("centerInMesh");
    return a >= 0 && n.type().attrs[size_t(a)].type == PlugType::Bool && n.as<bool>(a);
}

std::vector<NodeId> centerMeshes(Graph& g, NodeId node)
{
    std::vector<NodeId> out;
    Node* n = g.node(node);
    if (!n)
        return out;
    int a = n->type().attrIndex("centerMeshes");
    if (a >= 0 && n->type().attrs[size_t(a)].type == PlugType::Data)
        if (auto* list = dynamic_cast<const CenterMeshList*>(n->as<DataPtr>(a).get()))
            out = list->resolve(g, n->nameSpace());
    if (!out.empty())
        return out;
    for (NodeId id : g.nodes())
        if (isMesh(g.node(id)))
            if (NodeId s = skin::find(g, id); s != kNoNode && influences(g, s, node))
                out.push_back(id);
    return out;
}

CenterSource centerSource(Graph& g, const std::vector<NodeId>& meshes)
{
    CenterSource src;
    g.evaluate();
    for (NodeId id : meshes)
    {
        Node* n = g.node(id);
        if (!isMesh(n))
            continue;
        MeshPtr m;
        if (NodeId s = skin::find(g, id); s != kNoNode)
            m = skin::inputMesh(g, s);
        if (!m)
            m = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!m)
            continue;
        src.keep.push_back(m);
        src.meshes.push_back({m.get(), n->as<glm::dmat4>(Transform::kWorldMatrix)});
    }
    return src;
}

glm::dvec3 boneDirection(Graph& g, NodeId joint)
{
    g.evaluate();
    Node* n = g.node(joint);
    if (!n)
        return glm::dvec3(0.0);
    glm::dvec3 here = positionOf(*n);
    // Joints chain with joints; other centring nodes (rig template markers) chain with their own type.
    auto link = [&](const Node& o) { return isJoint(*n) ? isJoint(o) : &o.type() == &n->type(); };
    if (NodeId p = g.parent(joint); p != kNoNode && link(*g.node(p)))
        if (glm::dvec3 d = here - positionOf(*g.node(p)); glm::length(d) > 1e-9)
            return glm::normalize(d);
    for (NodeId c : g.children(joint))
        if (link(*g.node(c)))
            if (glm::dvec3 d = positionOf(*g.node(c)) - here; glm::length(d) > 1e-9)
                return glm::normalize(d);
    return glm::dvec3(0.0);
}

std::optional<glm::dvec3> centeredPoint(Graph& g, NodeId node, const glm::dvec3& point, const glm::dvec3& direction)
{
    LILA_ZONE();

    glm::dvec3 dir = glm::length(direction) > 1e-12 ? direction : boneDirection(g, node);
    if (glm::length(dir) < 1e-12)
        return std::nullopt;
    CenterSource src = centerSource(g, centerMeshes(g, node));
    if (src.meshes.empty())
        return std::nullopt;
    auto pick = geo::sectionCenter(src.meshes, point, dir);
    if (!pick)
        return std::nullopt;
    return pick->center;
}

void moveNodeTo(Graph& g, NodeId node, const glm::dvec3& world, bool keepChildren)
{
    glm::dvec3 delta = world - xformops::worldPivot(g, node);
    if (glm::length(delta) < 1e-12)
        return;
    xformops::moveBy(g, {node}, delta);
    if (!keepChildren)
        return;
    std::vector<NodeId> kids;
    for (NodeId c : g.children(node))
        if (dynamic_cast<Transform*>(g.node(c)))
            kids.push_back(c);
    if (!kids.empty())
        xformops::moveBy(g, kids, -delta);
}

bool centerNode(Graph& g, NodeId node, bool keepChildren, const glm::dvec3& direction)
{
    g.evaluate();
    auto p = centeredPoint(g, node, xformops::worldPivot(g, node), direction);
    if (!p)
        return false;
    moveNodeTo(g, node, *p, keepChildren);
    return true;
}
} // namespace rig

void registerCenterCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "centerJoint",
        .doc = "Move each joint (or any node with centerInMesh/centerMeshes attrs) to the centre of the mesh cross-section "
               "across its bone: the meshes are cut by the plane through the joint perpendicular to parent->joint (or "
               "joint->first child), and the joint moves to the area centroid of the loop it sits in, or the nearest "
               "loop. Meshes come from centerMeshes, else the skinClusters it influences. Returns the nodes moved.",
        .positionalDoc = "joints",
        .flags = {
            {"keepChildren", "kc", T::Bool, false, "Leave the children where they are."},
            {"direction", "d", T::Vec3, nullptr, "World cutting direction instead of the bone's."},
            {"meshes", "m", T::List, nullptr, "Meshes to centre in instead of the node's own."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            glm::dvec3 dir = a.has("direction") ? vec3Of(a.flags["direction"]) : glm::dvec3(0.0);
            std::vector<NodeId> override;
            if (a.has("meshes"))
                for (const json& m : a.flags["meshes"])
                {
                    Node& n = nodeNamed(m.get<std::string>(), "centerJoint");
                    if (!isMesh(&n))
                        throw CommandError("centerJoint: " + n.path() + " is not a mesh");
                    override.push_back(n.id());
                }
            std::vector<NodeId> list = targets(g, a, "centerJoint");
            if (list.empty())
                throw CommandError("centerJoint: no joints given or selected");
            json moved = json::array();
            for (NodeId id : list)
            {
                std::optional<glm::dvec3> p;
                g.evaluate();
                glm::dvec3 here = xformops::worldPivot(g, id);
                if (override.empty())
                    p = rig::centeredPoint(g, id, here, dir);
                else
                {
                    glm::dvec3 d = glm::length(dir) > 1e-12 ? dir : rig::boneDirection(g, id);
                    rig::CenterSource src = rig::centerSource(g, override);
                    if (auto pick = glm::length(d) > 1e-12 ? geo::sectionCenter(src.meshes, here, d) : std::nullopt)
                        p = pick->center;
                }
                if (!p)
                {
                    log::warn("centerJoint: nothing to centre {} in (no bone direction or no meshes under it)", g.node(id)->path());
                    continue;
                }
                rig::moveNodeTo(g, id, *p, a.get<bool>("keepChildren"));
                moved.push_back(g.node(id)->path());
            }
            return moved;
        },
    });

    ops::registerCommand({
        .name = "centerMeshes",
        .doc = "Bind nodes to the meshes they centre in (their centerMeshes attr), overriding the skinCluster lookup. "
               "With no meshes flag, returns the meshes each node centres in now.",
        .positionalDoc = "joints",
        .flags = {
            {"meshes", "m", T::List, nullptr, "Mesh names; [] clears the binding."},
            {"centerInMesh", "cim", T::Bool, nullptr, "Also turn live centring on or off."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> list = targets(g, a, "centerMeshes");
            if (list.empty())
                throw CommandError("centerMeshes: no nodes given or selected");
            std::vector<NodeId> meshes;
            if (a.has("meshes"))
                for (const json& m : a.flags["meshes"])
                {
                    Node& n = nodeNamed(m.get<std::string>(), "centerMeshes");
                    if (!isMesh(&n))
                        throw CommandError("centerMeshes: " + n.path() + " is not a mesh");
                    meshes.push_back(n.id());
                }
            json out = json::object();
            for (NodeId id : list)
            {
                Node* n = g.node(id);
                int attr = n->type().attrIndex("centerMeshes");
                int on = n->type().attrIndex("centerInMesh");
                if (attr < 0 || on < 0)
                    throw CommandError("centerMeshes: " + n->path() + " has no centerMeshes attr");
                if (a.has("meshes"))
                    g.setValue({id, attr, -1}, rig::makeCenterMeshList(g, meshes));
                if (a.has("centerInMesh"))
                    g.setValue({id, on, -1}, a.get<bool>("centerInMesh"));
                json names = json::array();
                for (NodeId m : rig::centerMeshes(g, id))
                    names.push_back(g.node(m)->path());
                out[n->path()] = names;
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "snapToComponentCenter",
        .doc = "Move transforms, joints or markers to the centre of the current component selection. Targets are the "
               "given nodes, else the selected transforms that aren't meshes holding the components.",
        .positionalDoc = "nodes",
        .flags = {
            {"center", "c", T::String, "boundingBox", "boundingBox (middle of the points' bounds) or average.", {"boundingBox", "average"}},
            {"keepChildren", "kc", T::Bool, nullptr, "Leave the children where they are. Default: on for joints, off otherwise."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            std::vector<glm::dvec3> pts = componentPoints(g);
            if (pts.empty())
                throw CommandError("snapToComponentCenter: no components selected");
            glm::dvec3 target(0.0);
            std::string mode = a.get<std::string>("center", "boundingBox");
            if (mode == "average")
            {
                for (const glm::dvec3& p : pts)
                    target += p;
                target /= double(pts.size());
            }
            else if (mode == "boundingBox")
            {
                glm::dvec3 lo = pts[0], hi = pts[0];
                for (const glm::dvec3& p : pts)
                    lo = glm::min(lo, p), hi = glm::max(hi, p);
                target = (lo + hi) * 0.5;
            }
            else
                throw CommandError("snapToComponentCenter: center must be boundingBox or average, got '" + mode + "'");

            std::vector<NodeId> list;
            if (!a.positional.empty())
                list = targets(g, a, "snapToComponentCenter");
            else
                for (NodeId id : g.selection())
                {
                    Node* n = g.node(id);
                    bool holdsComponents = false;
                    for (ComponentMode k : {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face})
                        holdsComponents |= !components::selected(id, k).empty();
                    if (dynamic_cast<Transform*>(n) && !holdsComponents)
                        list.push_back(id);
                }
            if (list.empty())
                throw CommandError("snapToComponentCenter: no transforms to move; select them along with the components");
            json moved = json::array();
            for (NodeId id : xformops::topLevel(g, list))
            {
                bool keep = a.has("keepChildren") ? a.get<bool>("keepChildren") : rig::isJoint(*g.node(id));
                g.evaluate();
                rig::moveNodeTo(g, id, target, keep);
                moved.push_back(g.node(id)->path());
            }
            return moved;
        },
    });
}
} // namespace lila::scene
