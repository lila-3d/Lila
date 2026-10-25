#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/NodeBounds.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <unordered_map>

namespace lila::scene
{
namespace
{
Graph& graph() { return Graph::current(); }

Node& nodeNamed(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

/** Copies every storable input value that isn't driven by a connection. */
void copyValues(Graph& g, const Node& from, Node& to)
{
    const auto& defs = from.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
        if ((defs[i].flags & AttrInput) && !g.source({from.id(), i, -1}))
            g.setValue({to.id(), i, -1}, from.value(i));
    // Locks copy over last, as Maya's duplicate keeps them.
    for (int i = 0; i < int(defs.size()); ++i)
    {
        if (defs[i].type != PlugType::Vec3)
        {
            if (g.isLocked({from.id(), i, -1}))
                g.setLocked({to.id(), i, -1}, true);
            continue;
        }
        for (int c = 0; c < 3; ++c)
            if (g.isLocked({from.id(), i, c}))
                g.setLocked({to.id(), i, c}, true);
    }
}

/**
 * Duplicates the non-DAG history feeding a mesh (generator, modifiers, base source) so the copy keeps a live stack of
 * its own. `copies` maps already duplicated history nodes, so shared upstream nodes are copied once.
 */
void duplicateHistory(Graph& g, NodeId srcMesh, NodeId dstMesh, std::unordered_map<NodeId, NodeId>& copies)
{
    // Walk the chain from the mesh down, copying each history node and reconnecting the copies in the same shape.
    PlugRef srcPlug{srcMesh, MeshNode::kInMesh, -1};
    PlugRef dstPlug{dstMesh, MeshNode::kInMesh, -1};
    while (auto src = g.source(srcPlug))
    {
        Node* up = g.node(src->node);
        if (up->type().dag)
        {
            // Fed by another object's mesh (e.g. a deformer source): share the connection.
            g.connect(*src, dstPlug);
            break;
        }
        NodeId copyId;
        if (auto it = copies.find(up->id()); it != copies.end())
            copyId = it->second;
        else
        {
            Node* copy = g.createNode(up->type(), {}, kNoNode, true);
            copyValues(g, *up, *copy);
            copyId = copy->id();
            copies[up->id()] = copyId;
        }
        g.connect({copyId, src->attr, -1}, dstPlug);
        int inAttr = up->type().attrIndex("inMesh");
        if (inAttr < 0)
            break;
        srcPlug = {up->id(), inAttr, -1};
        dstPlug = {copyId, inAttr, -1};
    }
}

/** Driven inputs of a shape without a mesh stack (curves) take their current value, so the copy keeps its shape without the history. */
void copyEvaluatedInputs(Graph& g, const Node& from, Node& to)
{
    const auto& defs = from.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
        if ((defs[i].flags & AttrInput) && g.source({from.id(), i, -1}))
            g.setValue({to.id(), i, -1}, g.value({from.id(), i, -1}));
}

std::vector<NodeId> topLevel(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> ids;
    for (const std::string& n : names)
        ids.push_back(nodeNamed(n, cmd).id());
    return xformops::topLevel(g, ids);
}
} // namespace

void registerObjectCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "duplicate",
        .doc = "Duplicate objects with their children, keeping each mesh's generator and modifier stack live. Returns the "
               "new top-level names.",
        .positionalDoc = "objects",
        .flags = {{"name", "n", T::String, nullptr, "Name for a single duplicate."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            std::vector<NodeId> roots = topLevel(g, a.objects(), "duplicate");
            if (roots.empty())
                throw CommandError("duplicate: nothing to duplicate");
            std::unordered_map<NodeId, NodeId> copies;
            json out = json::array();
            std::vector<NodeId> made;
            std::function<NodeId(NodeId, NodeId, bool)> copyTree = [&](NodeId src, NodeId parent, bool top) {
                Node* n = g.node(src);
                std::string wanted = top && a.has("name") && roots.size() == 1 ? a.get<std::string>("name") : n->name();
                Node* c = g.createNode(n->type(), wanted, parent, top && a.has("name"));
                copyValues(g, *n, *c);
                if (Node* layer = displayLayers::layerOf(g, src))
                    displayLayers::addMembers(g, layer->id(), {c->id()});
                if (n->type().isA("mesh"))
                    duplicateHistory(g, src, c->id(), copies);
                else if (components::pointProvider(*n))
                    copyEvaluatedInputs(g, *n, *c);
                for (NodeId child : std::vector<NodeId>(g.children(src)))
                    copyTree(child, c->id(), false);
                return c->id();
            };
            for (NodeId r : roots)
            {
                NodeId c = copyTree(r, g.parent(r), true);
                made.push_back(c);
                out.push_back(g.node(c)->path());
            }
            g.select(made);
            return out;
        },
    });

    ops::registerCommand({
        .name = "centerPivot",
        .doc = "Move pivots to the centre of each object's bounding box (mesh points, curve points and handles) without moving the "
               "object.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "centerPivot");
                if (!n.type().isA("transform"))
                    continue;
                geo::Bounds b = objectBounds(g, n.id());
                glm::dvec3 centre = b.valid() ? b.center() : glm::dvec3(0.0);
                glm::dvec3 world = glm::dvec3(n.as<glm::dmat4>(Transform::kWorldMatrix) * glm::dvec4(centre, 1.0));
                xformops::setWorldPivot(g, n.id(), world);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "freezeTransforms",
        .doc = "Bake translate/rotate/scale into mesh or curve points (and children's transforms), leaving identity values. "
               "World positions don't change.",
        .positionalDoc = "objects",
        .flags = {{"translate", "", T::Bool, true, "Freeze translation."},
                  {"rotate", "", T::Bool, true, "Freeze rotation."},
                  {"scale", "", T::Bool, true, "Freeze scale."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            UndoChunk chunk("Freeze Transformations");
            for (NodeId id : topLevel(g, a.objects(), "freezeTransforms"))
            {
                g.evaluate();
                Node* n = g.node(id);
                if (!n->type().isA("transform"))
                    continue;
                glm::dmat4 before = n->as<glm::dmat4>(Transform::kMatrix);
                // Children keep their world placement by absorbing what this node gives up.
                std::vector<std::pair<NodeId, glm::dmat4>> children;
                for (NodeId c : g.children(id))
                    children.push_back({c, g.node(c)->as<glm::dmat4>(Transform::kWorldMatrix)});

                glm::dvec3 pivot = xformops::worldPivot(g, id);
                if (a.get<bool>("translate", true))
                    g.setValue({id, Transform::kTranslate, -1}, glm::dvec3(0.0));
                if (a.get<bool>("rotate", true))
                    g.setValue({id, Transform::kRotate, -1}, glm::dvec3(0.0));
                if (a.get<bool>("scale", true))
                    g.setValue({id, Transform::kScale, -1}, glm::dvec3(1.0));
                for (int attr : {Transform::kRotatePivot, Transform::kScalePivot, Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate})
                    g.setValue({id, attr, -1}, glm::dvec3(0.0));
                g.evaluate();
                glm::dmat4 after = n->as<glm::dmat4>(Transform::kMatrix);
                glm::dmat4 bake = glm::inverse(after) * before;

                if (n->type().isA("mesh"))
                {
                    PlugRef base = modifiers::editPlug(g, id);
                    if (MeshPtr m = std::get<MeshPtr>(g.storedValue(base)))
                    {
                        geo::Mesh copy = *m;
                        for (geo::Index p = 0; p < copy.pointCapacity(); ++p)
                            if (copy.pointValid(p))
                                copy.setPosition(p, glm::dvec3(bake * glm::dvec4(copy.position(p), 1.0)));
                        // A mirroring freeze turns faces inside out; flip them back.
                        if (glm::determinant(glm::dmat3(bake)) < 0.0)
                            for (geo::Index f = 0; f < copy.faceCapacity(); ++f)
                                if (copy.faceValid(f))
                                    copy.flipFace(f);
                        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(copy))));
                    }
                }
                else if (const components::PointProvider* pp = components::pointProvider(*n); pp && pp->bake)
                    pp->bake(g, id, bake);
                // The pivot stays where it was in the world, like Maya.
                xformops::setWorldPivot(g, id, pivot);
                g.evaluate();
                for (auto& [c, world] : children)
                    Transform::setLocalMatrix(g, c, glm::inverse(g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix)) * world);
            }
            return nullptr;
        },
    });

    // What the last hide hid, for showLastHidden.
    struct LastHidden
    {
        const Graph* graph = nullptr;
        std::vector<NodeId> ids;
    };
    static LastHidden lastHidden;

    ops::registerCommand({
        .name = "hide",
        .doc = "Hide objects (visibility off). Defaults to the selection.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Hide");
            std::vector<NodeId> hidden;
            for (const std::string& name : a.objects())
                if (Node& n = nodeNamed(name, "hide"); n.type().isA("transform"))
                {
                    g.setValue({n.id(), Transform::kVisibility, -1}, false);
                    hidden.push_back(n.id());
                }
            if (!hidden.empty())
                lastHidden = {&g, std::move(hidden)};
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "showLastHidden",
        .doc = "Show the objects the last hide (or hiding toggle) hid. Returns them.",
        .run = [](const Args&) -> json {
            Graph& g = graph();
            UndoChunk chunk("Show Last Hidden");
            json out = json::array();
            if (lastHidden.graph != &g)
                return out;
            for (NodeId id : lastHidden.ids)
                if (Node* n = g.node(id); n && n->type().isA("transform"))
                {
                    g.setValue({id, Transform::kVisibility, -1}, true);
                    out.push_back(n->path());
                }
            return out;
        },
    });

    ops::registerCommand({
        .name = "toggleVisibility",
        .doc = "Toggle visibility (the H key). Defaults to the selection. Every object ends up the same: hidden if most "
               "were visible (a tie hides), shown otherwise. Returns the new visibility.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Toggle Visibility");
            std::vector<NodeId> ids;
            size_t shown = 0;
            for (const std::string& name : a.objects())
                if (Node& n = nodeNamed(name, "toggleVisibility"); n.type().isA("transform"))
                {
                    ids.push_back(n.id());
                    shown += n.as<bool>(Transform::kVisibility) ? 1 : 0;
                }
            if (ids.empty())
                return nullptr;
            const bool show = shown * 2 < ids.size();
            for (NodeId id : ids)
                if (g.node(id)->as<bool>(Transform::kVisibility) != show)
                    g.setValue({id, Transform::kVisibility, -1}, show);
            if (!show)
                lastHidden = {&g, ids};
            return show;
        },
    });

    ops::registerCommand({
        .name = "show",
        .doc = "Show objects. all=True shows every hidden object.",
        .positionalDoc = "objects",
        .flags = {{"all", "", T::Bool, false, "Show everything."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Show");
            if (a.get<bool>("all"))
            {
                for (NodeId id : g.nodes())
                    if (g.node(id)->type().isA("transform") && !g.node(id)->as<bool>(Transform::kVisibility))
                        g.setValue({id, Transform::kVisibility, -1}, true);
                return nullptr;
            }
            for (const std::string& name : a.objects())
                if (Node& n = nodeNamed(name, "show"); n.type().isA("transform"))
                    g.setValue({n.id(), Transform::kVisibility, -1}, true);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "ungroup",
        .doc = "Move each group's children up to the group's parent, keeping where they are, then delete the group. "
               "Defaults to the selection; returns the children.",
        .positionalDoc = "groups",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Ungroup");
            json out = json::array();
            for (const std::string& name : a.objects())
            {
                Node& grp = nodeNamed(name, "ungroup");
                if (grp.type().name != "transform")
                    throw CommandError("ungroup: '" + name + "' is not a group");
                if (grp.referenceId())
                    throw CommandError("ungroup: '" + name + "' comes from a reference");
                NodeId up = g.parent(grp.id());
                for (NodeId c : std::vector<NodeId>(g.children(grp.id())))
                {
                    g.setParent(c, up, true);
                    out.push_back(g.node(c)->path());
                }
                g.deleteNode(grp.id());
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "resetTransforms",
        .doc = "Put translate, rotate and scale back to their defaults (the object moves).",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            UndoChunk chunk("Reset Transformations");
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "resetTransforms");
                if (!n.type().isA("transform"))
                    continue;
                g.setValue({n.id(), Transform::kTranslate, -1}, glm::dvec3(0.0));
                g.setValue({n.id(), Transform::kRotate, -1}, glm::dvec3(0.0));
                g.setValue({n.id(), Transform::kScale, -1}, glm::dvec3(1.0));
            }
            return nullptr;
        },
    });
}
} // namespace lila::scene
