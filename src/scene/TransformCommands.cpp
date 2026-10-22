#include "cmds/Command.h"
#include "scene/Graph.h"
#include "scene/TransformOps.h"

namespace lila::scene
{
namespace
{
std::vector<NodeId> transformsOf(const Args& a, const char* cmd)
{
    std::vector<NodeId> ids;
    for (const std::string& name : a.objects())
    {
        Node* n = Graph::current().find(name);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
        if (n->type().isA("transform"))
            ids.push_back(n->id());
    }
    if (ids.empty())
        throw CommandError(std::string(cmd) + ": nothing to transform");
    return ids;
}

glm::dvec3 vec3(const json& j)
{
    return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

Space spaceArg(const Args& a)
{
    std::string s = a.get<std::string>("space", "world");
    if (s == "world")
        return Space::World;
    if (s == "object")
        return Space::Object;
    if (s == "parent")
        return Space::Parent;
    throw CommandError("space must be 'world', 'object' or 'parent'");
}

glm::dquat eulerQuat(const glm::dvec3& degrees)
{
    return glm::quat_cast(glm::dmat3(eulerToMatrix(degrees, RotateOrder::XYZ)));
}
} // namespace

void registerTransformCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "move",
        .doc = "Move objects. move(by=(x, y, z)) moves relative in `space`; move(to=(x, y, z)) puts each pivot at a world "
               "position. A selected parent and child move once.",
        .positionalDoc = "objects",
        .flags = {
            {"by", "", T::Vec3, nullptr, "Relative offset, cm."},
            {"to", "", T::Vec3, nullptr, "Absolute world position of the pivot, cm."},
            {"space", "", T::String, "world", "'world', 'object' or 'parent' for `by`."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto ids = transformsOf(a, "move");
            if (a.has("to"))
            {
                glm::dvec3 to = vec3(a.flags["to"]);
                for (NodeId id : xformops::topLevel(g, ids))
                    xformops::moveBy(g, {id}, to - xformops::worldPivot(g, id));
            }
            else if (a.has("by"))
                xformops::moveBy(g, ids, vec3(a.flags["by"]), spaceArg(a));
            else
                throw CommandError("move: give by= or to=");
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "rotate",
        .doc = "Rotate objects about their pivots. rotate(by=(x, y, z)) adds a rotation (degrees, XYZ order) in `space`; "
               "rotate(to=(x, y, z)) sets the local Euler angles.",
        .positionalDoc = "objects",
        .flags = {
            {"by", "", T::Vec3, nullptr, "Relative rotation, degrees."},
            {"to", "", T::Vec3, nullptr, "Absolute local rotation, degrees."},
            {"space", "", T::String, "world", "'world', 'object' or 'parent' for `by`."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto ids = transformsOf(a, "rotate");
            if (a.has("to"))
            {
                for (NodeId id : ids)
                    g.setValue({id, Transform::kRotate}, g.keepLocked({id, Transform::kRotate}, vec3(a.flags["to"])));
            }
            else if (a.has("by"))
                xformops::rotateBy(g, ids, eulerQuat(vec3(a.flags["by"])), spaceArg(a));
            else
                throw CommandError("rotate: give by= or to=");
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "scale",
        .doc = "Scale objects along their own axes. scale(by=(x, y, z)) multiplies; scale(to=(x, y, z)) sets.",
        .positionalDoc = "objects",
        .flags = {
            {"by", "", T::Vec3, nullptr, "Factor per axis."},
            {"to", "", T::Vec3, nullptr, "Absolute scale."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            auto ids = transformsOf(a, "scale");
            if (a.has("to"))
            {
                for (NodeId id : ids)
                    g.setValue({id, Transform::kScale}, g.keepLocked({id, Transform::kScale}, vec3(a.flags["to"])));
            }
            else if (a.has("by"))
                xformops::scaleBy(g, ids, vec3(a.flags["by"]));
            else
                throw CommandError("scale: give by= or to=");
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "setPivot",
        .doc = "Move the rotate and scale pivots to a world position, or orient the pivot, without moving the objects.",
        .positionalDoc = "objects",
        .flags = {
            {"position", "", T::Vec3, nullptr, "World position, cm."},
            {"orient", "", T::Vec3, nullptr, "Pivot orientation relative to the object's axes, Euler XYZ degrees (pivotOrient)."},
        },
        .run = [](const Args& a) -> json {
            if (!a.has("position") && !a.has("orient"))
                throw CommandError("setPivot: give position= or orient=");
            Graph& g = Graph::current();
            for (NodeId id : transformsOf(a, "setPivot"))
            {
                if (a.has("position"))
                    xformops::setWorldPivot(g, id, vec3(a.flags["position"]));
                if (a.has("orient"))
                    g.setValue({id, Transform::kPivotOrient}, vec3(a.flags["orient"]));
            }
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "resetPivot",
        .doc = "Put the pivots back at the object's origin, keeping the object where it is.",
        .positionalDoc = "objects",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            for (NodeId id : transformsOf(a, "resetPivot"))
                xformops::resetPivot(g, id);
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "pivot",
        .doc = "World position of an object's rotate pivot.",
        .positionalDoc = "object",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto ids = transformsOf(a, "pivot");
            glm::dvec3 p = xformops::worldPivot(Graph::current(), ids.front());
            return json::array({p.x, p.y, p.z});
        },
    });
}
} // namespace lila::scene
