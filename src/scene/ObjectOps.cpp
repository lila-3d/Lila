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
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>

#include <functional>
#include <unordered_map>
#include <unordered_set>

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

/** Bounds of the node and everything under it, in the node's object space. */
geo::Bounds hierarchyBounds(Graph& g, NodeId root)
{
    geo::Bounds b = objectBounds(g, root);
    const glm::dmat4 toRoot = glm::inverse(g.node(root)->as<glm::dmat4>(Transform::kWorldMatrix));
    std::function<void(NodeId)> walk = [&](NodeId id) {
        for (NodeId c : g.children(id))
        {
            Node* n = g.node(c);
            if (geo::Bounds local = objectBounds(g, c); local.valid() && n->type().isA("transform"))
            {
                const glm::dmat4 m = toRoot * n->as<glm::dmat4>(Transform::kWorldMatrix);
                for (int k = 0; k < 8; ++k)
                    b.extend(glm::dvec3(m * glm::dvec4(k & 1 ? local.max.x : local.min.x, k & 2 ? local.max.y : local.min.y,
                                                       k & 4 ? local.max.z : local.min.z, 1.0)));
            }
            walk(c);
        }
    };
    walk(root);
    return b;
}

/**
 * A joint's frozen rotation moves into jointOrient, rotate and rotateAxis zeroed; its translate stays, like Maya. Frozen
 * scale keeps its sign, so a mirrored joint stays mirrored. With orient the joint's axes line up with the world instead.
 * Returns whether the joint's world matrix changed.
 */
bool freezeJoint(Graph& g, NodeId id, bool rotate, bool scale, bool orient)
{
    const auto& joint = static_cast<const Joint&>(*g.node(id));
    const glm::dmat4 before = joint.as<glm::dmat4>(Transform::kWorldMatrix);
    const glm::dmat3 turn = glm::dmat3(joint.preRotation() * joint.rotation() * joint.rotateAxisMatrix());
    if (scale)
    {
        glm::dvec3 s = joint.as<glm::dvec3>(Transform::kScale);
        g.setValue({id, Transform::kScale, -1}, glm::dvec3(s.x < 0.0 ? -1.0 : 1.0, s.y < 0.0 ? -1.0 : 1.0, s.z < 0.0 ? -1.0 : 1.0));
    }
    if (rotate)
    {
        const glm::dmat3 parent = rigutil::orthonormal(glm::dmat3(joint.as<glm::dmat4>(Transform::kParentMatrix)));
        // Under a mirrored parent the X axis can only line up flipped.
        const glm::dmat3 mirror = glm::determinant(parent) < 0.0 ? glm::dmat3(glm::dvec3(-1, 0, 0), glm::dvec3(0, 1, 0), glm::dvec3(0, 0, 1)) : glm::dmat3(1.0);
        const glm::dmat3 jo = orient ? glm::transpose(parent) * mirror : turn;
        g.setValue({id, Transform::kRotate, -1}, glm::dvec3(0.0));
        g.setValue({id, Transform::kRotateAxis, -1}, glm::dvec3(0.0));
        g.setValue({id, Joint::kJointOrient, -1}, rigutil::clean(matrixToEuler(jo, RotateOrder::XYZ)));
    }
    g.evaluate();
    const glm::dmat4 after = joint.as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        if (glm::length(after[c] - before[c]) > 1e-9)
            return true;
    return false;
}

/** Sets the node's local values so it stays at world under its parent as the parent is now. */
void keepWorld(Graph& g, NodeId id, const glm::dmat4& world)
{
    g.evaluate();
    const Node& n = *g.node(id);
    glm::dmat4 local = glm::inverse(n.as<glm::dmat4>(Transform::kParentMatrix)) * world;
    // A joint compensating its parent's scale has that scale undone after its translate; ask for it up front.
    if (rig::isJoint(n) && n.as<bool>(Joint::kSegmentScaleCompensate))
    {
        const glm::dvec3 parentScale = n.as<glm::dvec3>(Joint::kInverseScale);
        for (int c = 0; c < 3; ++c)
            local[c] = glm::dvec4(parentScale * glm::dvec3(local[c]), 0.0);
    }
    Transform::setLocalMatrix(g, id, local);
}

void appendDescendants(Graph& g, NodeId id, std::vector<NodeId>& out)
{
    for (NodeId c : g.children(id))
    {
        out.push_back(c);
        appendDescendants(g, c, out);
    }
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
        .doc = "Move pivots to the centre of each object's bounding box (mesh points, curve points and handles, and everything "
               "parented under it, so a group centres on its children) without moving the object.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            for (const std::string& name : a.objects())
            {
                Node& n = nodeNamed(name, "centerPivot");
                if (!n.type().isA("transform"))
                    continue;
                geo::Bounds b = hierarchyBounds(g, n.id());
                glm::dvec3 centre = b.valid() ? b.center() : glm::dvec3(0.0);
                glm::dvec3 world = glm::dvec3(n.as<glm::dmat4>(Transform::kWorldMatrix) * glm::dvec4(centre, 1.0));
                xformops::setWorldPivot(g, n.id(), world);
            }
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "freezeTransforms",
        .doc = "Bake translate/rotate/scale into mesh or curve points, leaving identity values. World positions don't change. "
               "With hierarchy (the default, like Maya) everything parented under the objects is frozen too; without it the "
               "children's transforms absorb what their parent gives up. Joints keep their translate and take their rotation "
               "into jointOrient, or with jointOrient line up with the world.",
        .positionalDoc = "objects",
        .flags = {{"translate", "", T::Bool, true, "Freeze translation."},
                  {"rotate", "", T::Bool, true, "Freeze rotation."},
                  {"scale", "", T::Bool, true, "Freeze scale."},
                  {"hierarchy", "", T::Bool, true, "Freeze everything parented under the objects too."},
                  {"jointOrient", "", T::Bool, false, "Also line joints' axes up with the world, zeroing their orientation."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            UndoChunk chunk("Freeze Transformations");
            std::vector<NodeId> ids = topLevel(g, a.objects(), "freezeTransforms");
            // Parents first, so each child is frozen after taking on what its parent gave up.
            if (a.get<bool>("hierarchy", true))
                for (size_t i = 0, roots = ids.size(); i < roots; ++i)
                    appendDescendants(g, ids[i], ids);
            for (NodeId id : ids)
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

                if (rig::isJoint(*n))
                {
                    // An unchanged world needs no child fix-up, which would also bake a segment scale compensate into them.
                    if (freezeJoint(g, id, a.get<bool>("rotate", true), a.get<bool>("scale", true), a.get<bool>("jointOrient", false)))
                        for (auto& [c, world] : children)
                            keepWorld(g, c, world);
                    continue;
                }

                glm::dvec3 pivot = xformops::worldPivot(g, id);
                const glm::dmat3 pivotAxes = xformops::pivotFrame(g, id);
                const bool customAxes = n->as<glm::dvec3>(Transform::kPivotOrient) != glm::dvec3(0.0);
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
                // The pivot stays where it was in the world, like Maya, and so do axes the user oriented.
                xformops::setWorldPivot(g, id, pivot);
                if (customAxes)
                    xformops::setWorldPivotFrame(g, id, pivotAxes);
                g.evaluate();
                for (auto& [c, world] : children)
                    keepWorld(g, c, world);
            }
            return nullptr;
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "bakePivot",
        .doc = "Turn the objects onto their pivots' axes, counter-rotating their points, so the pivot orientation becomes "
               "the object's own and scale works along it. Nothing moves in the world. Instances are made unique.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            UndoChunk chunk("Bake Pivot");
            for (NodeId id : topLevel(g, a.objects(), "bakePivot"))
            {
                g.evaluate();
                Node* n = g.node(id);
                const glm::dvec3 orient = n->as<glm::dvec3>(Transform::kPivotOrient);
                if (!n->type().isA("transform") || rig::isJoint(*n) || orient == glm::dvec3(0.0))
                    continue;
                const glm::dmat4 before = n->as<glm::dmat4>(Transform::kMatrix);
                std::vector<std::pair<NodeId, glm::dmat4>> children;
                for (NodeId c : g.children(id))
                    children.push_back({c, g.node(c)->as<glm::dmat4>(Transform::kWorldMatrix)});
                const glm::dvec3 pivot = xformops::worldPivot(g, id);
                const glm::dquat turn = glm::quat_cast(glm::dmat3(eulerToMatrix(orient, RotateOrder::XYZ)));
                g.setValue({id, Transform::kRotate, -1}, xformops::rotatedEuler(g, id, turn, Space::Object));
                g.setValue({id, Transform::kPivotOrient, -1}, glm::dvec3(0.0));
                g.evaluate();
                xformops::bakeIntoGeometry(g, id, glm::inverse(n->as<glm::dmat4>(Transform::kMatrix)) * before);
                xformops::setWorldPivot(g, id, pivot);
                g.evaluate();
                for (auto& [c, world] : children)
                    keepWorld(g, c, world);
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

    ops::registerCommand({
        .name = "unfrozenTransforms",
        .doc = "List the objects whose translate, rotate or scale is not identity (0, 0, 0 and 1, 1, 1), with their values.\n\n"
               "Checks the objects given or selected, or with scene every mesh, curve and group (joints, cameras, lights and "
               "locators are placed by their transforms and are left out). With rotateStep, rotations that are whole "
               "multiples of the step count as frozen; invert lists only rotations on the step instead. Returns one "
               "{name, translate, rotate, scale, unfrozen} per object listed, unfrozen naming the channels that put it there.",
        .positionalDoc = "objects",
        .flags = {{"scene", "", T::Bool, false, "Check the whole scene instead of the objects."},
                  {"hierarchy", "", T::Bool, false, "Also check everything parented under the objects."},
                  {"translate", "", T::Bool, true, "List objects with translation."},
                  {"rotate", "", T::Bool, true, "List objects with rotation."},
                  {"scale", "", T::Bool, true, "List objects with scale."},
                  {"rotateStep", "", T::Float, 0.0, "Degrees: rotations on whole multiples of it count as frozen. 0 is off."},
                  {"invert", "", T::Bool, false, "With rotateStep, list rotations on the step rather than off it."},
                  {"tolerance", "", T::Float, 1e-4, "How far from identity, or from the step, still counts as on it."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            g.evaluate();
            const double tol = a.get<double>("tolerance", 1e-4);
            const double step = a.get<double>("rotateStep", 0.0);
            const bool invert = step > 0.0 && a.get<bool>("invert", false);
            // What a scene-wide or hierarchy check takes in: the things whose transforms are meant to be frozen.
            auto checkable = [&](const Node& n) {
                return n.type().isA("transform") && !rig::isJoint(n) && !n.type().isA("locator") && !n.type().isA("light");
            };
            std::vector<NodeId> ids;
            if (a.get<bool>("scene", false))
            {
                for (NodeId id : g.nodes())
                    if (checkable(*g.node(id)))
                        ids.push_back(id);
            }
            else
            {
                for (const std::string& name : a.objects())
                {
                    Node& n = nodeNamed(name, "unfrozenTransforms");
                    if (n.type().isA("transform"))
                        ids.push_back(n.id());
                    if (a.get<bool>("hierarchy", false))
                        for (NodeId d : g.descendants(n.id()))
                            if (checkable(*g.node(d)))
                                ids.push_back(d);
                }
            }

            auto near = [tol](const glm::dvec3& v, const glm::dvec3& to) { return glm::all(glm::lessThanEqual(glm::abs(v - to), glm::dvec3(tol))); };
            auto onStep = [&](const glm::dvec3& r) { return near(r, glm::round(r / step) * step); };
            auto vec = [](const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); };
            json out = json::array();
            std::unordered_set<NodeId> seen;
            for (NodeId id : ids)
            {
                if (!seen.insert(id).second)
                    continue;
                const Node& n = *g.node(id);
                const glm::dvec3 t = n.as<glm::dvec3>(Transform::kTranslate);
                const glm::dvec3 r = n.as<glm::dvec3>(Transform::kRotate);
                const glm::dvec3 s = n.as<glm::dvec3>(Transform::kScale);
                json unfrozen = json::array();
                if (a.get<bool>("translate", true) && !near(t, glm::dvec3(0.0)))
                    unfrozen.push_back("translate");
                // Zero is on every step, so invert never lists an unrotated object.
                if (a.get<bool>("rotate", true) && !near(r, glm::dvec3(0.0)) && (step <= 0.0 || onStep(r) == invert))
                    unfrozen.push_back("rotate");
                if (a.get<bool>("scale", true) && !near(s, glm::dvec3(1.0)))
                    unfrozen.push_back("scale");
                if (!unfrozen.empty())
                    out.push_back({{"name", n.path()}, {"translate", vec(t)}, {"rotate", vec(r)}, {"scale", vec(s)}, {"unfrozen", unfrozen}});
            }
            return out;
        },
        .examples = {"unfrozenTransforms()", "unfrozenTransforms(scene=True, translate=False, rotateStep=90)"},
    });
}
} // namespace lila::scene
