#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Components.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
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

std::vector<NodeId> topLevel(Graph& g, const std::vector<std::string>& names, const char* cmd)
{
    std::vector<NodeId> ids;
    for (const std::string& n : names)
        ids.push_back(nodeNamed(n, cmd).id());
    return xformops::topLevel(g, ids);
}
} // namespace

void xformops::bakeIntoGeometry(Graph& g, NodeId id, const glm::dmat4& bake)
{
    LILA_ZONE();

    Node* n = g.node(id);
    if (n->type().isA("mesh"))
    {
        // The points are shared with other objects that keep their own transforms, so this one stops sharing.
        if (instancing::makeUnique(g, id))
            log::warn("{} shared its mesh; it has a copy of its own now", n->path());
        PlugRef base = modifiers::editPlug(g, id);
        if (MeshPtr m = std::get<MeshPtr>(g.storedValue(base)))
        {
            geo::Mesh copy = *m;
            for (geo::Index p = 0; p < copy.pointCapacity(); ++p)
                if (copy.pointValid(p))
                    copy.setPosition(p, glm::dvec3(bake * glm::dvec4(copy.position(p), 1.0)));
            // A mirroring bake turns faces inside out; flip them back.
            if (glm::determinant(glm::dmat3(bake)) < 0.0)
                for (geo::Index f = 0; f < copy.faceCapacity(); ++f)
                    if (copy.faceValid(f))
                        copy.flipFace(f);
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(copy))));
        }
    }
    else if (const components::PointProvider* pp = components::pointProvider(*n); pp && pp->bake)
        pp->bake(g, id, bake);
}

void registerObjectCommands()
{
    using T = ArgType;

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
                xformops::bakeIntoGeometry(g, id, glm::inverse(after) * before);
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
