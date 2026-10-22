#include "scene/Rig.h"

#include "core/Profile.h"
#include "scene/RigUtil.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene
{
using namespace rigutil;

namespace
{
const char* kSideNames[] = {"center", "left", "right", "none"};
const char* kTypeNames[] = {"none", "root", "hip", "knee", "foot", "toe", "spine", "neck", "head", "collar", "shoulder",
                            "elbow", "hand", "finger", "thumb", "propA", "propB", "propC", "other", "indexFinger",
                            "middleFinger", "ringFinger", "pinkyFinger", "extraFinger", "bigToe", "indexToe",
                            "middleToe", "ringToe", "pinkyToe", "footThumb"};
const char* kDrawStyleNames[] = {"bone", "box", "none"};

AttrDef vec3Attr(std::string name, std::string shortName, glm::dvec3 def, uint32_t flags, std::string doc)
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = flags, .doc = std::move(doc)};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + char('X' + i);
        a.childShort[i] = shortName + char('x' + i);
    }
    return a;
}

std::vector<NodeId> jointChildren(Graph& g, NodeId id)
{
    std::vector<NodeId> out;
    for (NodeId c : g.children(id))
        if (rig::isJoint(*g.node(c)))
            out.push_back(c);
    return out;
}

int depthOf(Graph& g, NodeId id)
{
    int d = 0;
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        ++d;
    return d;
}

Node& jointNamed(const std::string& name, const char* cmd)
{
    Node& n = nodeNamed(name, cmd);
    if (!rig::isJoint(n))
        throw CommandError(std::string(cmd) + ": " + name + " is not a joint");
    return n;
}

glm::dvec3 axisArg(const json& j, const char* cmd)
{
    try
    {
        if (j.is_string())
            return rig::axisVector(j.get<std::string>());
        return glm::normalize(vec3Of(j));
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

/** Sets a joint's translate so its world position is `world`, given its current parent. */
void placeAt(Graph& g, NodeId id, const glm::dvec3& world)
{
    g.evaluate();
    glm::dmat4 parentInv = glm::inverse(g.node(id)->as<glm::dmat4>(Transform::kParentMatrix));
    g.setValue({id, Transform::kTranslate}, clean(glm::dvec3(parentInv * glm::dvec4(world, 1.0))));
}

/** Rotation of a joint's local frame relative to its parent's (parent scale removed). */
glm::dmat3 localRotation(const Node& n, const glm::dmat3& worldRotation)
{
    return glm::transpose(orthonormal(glm::dmat3(n.as<glm::dmat4>(Transform::kParentMatrix)))) * worldRotation;
}

/** Puts a local rotation entirely into jointOrient, zeroing rotate. */
void setOrientation(Graph& g, NodeId id, const glm::dmat3& local)
{
    g.setValue({id, Transform::kRotate}, glm::dvec3(0.0));
    g.setValue({id, Joint::kJointOrient}, clean(matrixToEuler(local, RotateOrder::XYZ)));
}
} // namespace

glm::dmat4 Joint::preRotation() const
{
    return eulerToMatrix(as<glm::dvec3>(kJointOrient), RotateOrder::XYZ);
}

void Joint::compute(const EvalContext&)
{
    glm::dmat4 local = localMatrix(*this);
    if (as<bool>(kSegmentScaleCompensate))
    {
        glm::dvec3 s = as<glm::dvec3>(kInverseScale);
        for (int i = 0; i < 3; ++i)
            s[i] = std::abs(s[i]) > 1e-12 ? 1.0 / s[i] : 1.0;
        glm::dvec3 t = as<glm::dvec3>(kTranslate);
        // The parent's scale is undone right after translate, so this joint's own segment keeps its length.
        local = glm::translate(glm::dmat4(1.0), t) * glm::scale(glm::dmat4(1.0), s) * glm::translate(glm::dmat4(1.0), -t) * local;
    }
    setOutput(kMatrix, local);
    setOutput(kWorldMatrix, as<glm::dmat4>(kParentMatrix) * local);
}

NodeType Joint::makeType(const NodeType& base)
{
    const uint32_t stored = AttrInput | AttrStorable;
    NodeType t;
    t.name = "joint";
    t.icon = "bone";
    t.base = &base;
    t.dag = true;
    t.ownAttrs = {
        vec3Attr("jointOrient", "jo", glm::dvec3(0.0), stored, "Rotation between rotate and the parent, degrees (xyz order)."),
        {.name = "radius", .shortName = "radi", .type = PlugType::Float, .defaultValue = 2.0, .flags = stored,
         .doc = "Display size, cm."},
        {.name = "segmentScaleCompensate", .shortName = "ssc", .type = PlugType::Bool, .defaultValue = false, .flags = stored,
         .doc = "Undo the parent's scale (inverseScale). Off by default: Unreal ignores it."},
        vec3Attr("inverseScale", "is", glm::dvec3(1.0), AttrInput | AttrStorable | AttrHidden,
                 "The parent's scale, undone when segmentScaleCompensate is on. Usually connected from the parent's scale."),
        {.name = "side", .shortName = "sd", .type = PlugType::Enum, .defaultValue = int64_t(kCenter), .flags = stored,
         .enumNames = {kSideNames, kSideNames + 4}, .doc = "Side label, used when mirroring."},
        {.name = "type", .shortName = "typ", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = stored,
         .enumNames = {std::begin(kTypeNames), std::end(kTypeNames)}, .doc = "Body part label."},
        {.name = "otherType", .shortName = "otp", .type = PlugType::String, .defaultValue = std::string(), .flags = stored,
         .doc = "Label used when type is 'other'."},
        {.name = "drawStyle", .shortName = "ds", .type = PlugType::Enum, .defaultValue = int64_t(kBone), .flags = stored,
         .enumNames = {kDrawStyleNames, kDrawStyleNames + 3}, .doc = "bone, box or none."},
    };
    t.create = [] { return std::make_unique<Joint>(); };
    return t;
}

namespace rig
{
bool isJoint(const Node& n) { return dynamic_cast<const Joint*>(&n) != nullptr; }

glm::dvec3 axisVector(std::string_view axis)
{
    double sign = 1.0;
    if (!axis.empty() && (axis[0] == '-' || axis[0] == '+'))
    {
        sign = axis[0] == '-' ? -1.0 : 1.0;
        axis.remove_prefix(1);
    }
    if (axis.size() == 1 && axis[0] >= 'x' && axis[0] <= 'z')
    {
        glm::dvec3 v(0.0);
        v[axis[0] - 'x'] = sign;
        return v;
    }
    throw std::invalid_argument("expected an axis like x, -y or z, got '" + std::string(axis) + "'");
}

glm::dmat3 aimFrame(const glm::dvec3& primary, const glm::dvec3& secondary, const glm::dvec3& aim, const glm::dvec3& up)
{
    glm::dvec3 a = glm::length(aim) > 1e-12 ? glm::normalize(aim) : glm::dvec3(1, 0, 0);
    glm::dvec3 u = up - glm::dot(up, a) * a;
    if (glm::length(u) < 1e-6)
    {
        // Up is parallel to the aim: lean toward whichever world axis is least aligned with it.
        glm::dvec3 alt = std::abs(a.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, 1);
        u = alt - glm::dot(alt, a) * a;
    }
    u = glm::normalize(u);

    // Local basis built from the requested axes, world basis from aim/up; the rotation maps one onto the other.
    glm::dvec3 lp = glm::normalize(primary);
    glm::dvec3 ls = secondary - glm::dot(secondary, lp) * lp;
    ls = glm::length(ls) > 1e-12 ? glm::normalize(ls) : glm::dvec3(0, 1, 0);
    glm::dmat3 local(lp, ls, glm::cross(lp, ls));
    glm::dmat3 world(a, u, glm::cross(a, u));
    return world * glm::transpose(local);
}

std::string mirrorName(const std::string& name, const std::string& search, const std::string& replace)
{
    if (search.empty())
        return name;
    if (name.size() > search.size() && name.ends_with(search))
        return name.substr(0, name.size() - search.size()) + replace;
    if (name.size() > search.size() && name.starts_with(search))
        return replace + name.substr(search.size());
    // Whole tokens only, so "_l" doesn't hit the "_l" in "_leg".
    std::string core = search;
    while (!core.empty() && core.front() == '_')
        core.erase(core.begin());
    while (!core.empty() && core.back() == '_')
        core.pop_back();
    std::string coreRep = replace;
    while (!coreRep.empty() && coreRep.front() == '_')
        coreRep.erase(coreRep.begin());
    while (!coreRep.empty() && coreRep.back() == '_')
        coreRep.pop_back();
    if (core.empty())
        return name;
    std::string out;
    size_t start = 0;
    bool changed = false;
    while (start <= name.size())
    {
        size_t end = name.find('_', start);
        if (end == std::string::npos)
            end = name.size();
        std::string token = name.substr(start, end - start);
        if (token == core)
        {
            token = coreRep;
            changed = true;
        }
        out += token;
        if (end < name.size())
            out += '_';
        start = end + 1;
    }
    return changed ? out : name;
}
} // namespace rig

namespace
{
void registerJointCommands()
{
    using T = ArgType;

    cmds::registerCommand({
        .name = "joint",
        .doc = "Create a joint. With no parent given it goes under the selected joint, so repeated calls build a chain "
               "(the new joint is selected). Returns its name.",
        .flags = {
            {"position", "p", T::Vec3, nullptr, "World position, cm."},
            {"name", "n", T::String, nullptr, "Name; made unique (joints follow the snake_case convention)."},
            {"parent", "", T::String, nullptr, "Parent; \"\" for a new root even when a joint is selected."},
            {"orientation", "o", T::Vec3, nullptr, "jointOrient, degrees."},
            {"radius", "rad", T::Float, nullptr, "Display radius, cm; defaults to the parent joint's."},
            {"segmentScaleCompensate", "ssc", T::Bool, false, "Undo the parent's scale (connects parent.scale)."},
            {"skipSelect", "ss", T::Bool, false, "Don't select the new joint."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            NodeId parent = kNoNode;
            if (a.has("parent"))
            {
                std::string p = a.get<std::string>("parent");
                if (!p.empty())
                    parent = nodeNamed(p, "joint").id();
            }
            else
                for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
                    if (rig::isJoint(*g.node(*it)))
                    {
                        parent = *it;
                        break;
                    }

            Node* j = guarded([&] { return g.createNode(*types::find("joint"), a.get<std::string>("name", ""), parent); });
            NodeId id = j->id();
            Node* pj = parent != kNoNode ? g.node(parent) : nullptr;
            if (a.has("radius"))
                g.setValue({id, Joint::kRadius}, a.get<double>("radius"));
            else if (pj && rig::isJoint(*pj))
                g.setValue({id, Joint::kRadius}, pj->value(Joint::kRadius));
            if (a.has("orientation"))
                g.setValue({id, Joint::kJointOrient}, vec3Of(a.flags["orientation"]));
            if (a.get<bool>("segmentScaleCompensate"))
            {
                g.setValue({id, Joint::kSegmentScaleCompensate}, true);
                if (pj && rig::isJoint(*pj))
                    g.connect({parent, Transform::kScale, -1}, {id, Joint::kInverseScale, -1});
            }
            if (a.has("position"))
                placeAt(g, id, vec3Of(a.flags["position"]));
            if (!a.get<bool>("skipSelect"))
                g.select({id});
            return g.node(id)->name();
        },
    });

    cmds::registerCommand({
        .name = "orientJoint",
        .doc = "Aim each joint's primary axis at its child with the secondary axis toward a world axis, putting the "
               "rotation in jointOrient and zeroing rotate. Children keep their world placement. Leaf joints take "
               "their parent's orientation.",
        .positionalDoc = "joints",
        .flags = {
            {"primaryAxis", "pa", T::Any, "x", "Local axis that points down the bone: x, -x, y, ..."},
            {"secondaryAxis", "sa", T::Any, "y", "Local axis that leans toward secondaryWorldAxis."},
            {"secondaryWorldAxis", "swa", T::Any, "y", "World axis (or [x, y, z]) the secondary axis leans toward."},
            {"children", "ch", T::Bool, true, "Orient the whole hierarchy below too."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            glm::dvec3 primary = axisArg(a.flags["primaryAxis"], "orientJoint");
            glm::dvec3 secondary = axisArg(a.flags["secondaryAxis"], "orientJoint");
            glm::dvec3 worldUp = axisArg(a.flags["secondaryWorldAxis"], "orientJoint");
            if (std::abs(glm::dot(primary, secondary)) > 0.999)
                throw CommandError("orientJoint: primaryAxis and secondaryAxis must differ");

            std::vector<NodeId> list;
            for (const std::string& name : a.objects())
            {
                NodeId id = jointNamed(name, "orientJoint").id();
                list.push_back(id);
                if (a.get<bool>("children"))
                    for (NodeId d : g.descendants(id))
                        if (rig::isJoint(*g.node(d)))
                            list.push_back(d);
            }
            std::sort(list.begin(), list.end());
            list.erase(std::unique(list.begin(), list.end()), list.end());
            std::stable_sort(list.begin(), list.end(), [&](NodeId x, NodeId y) { return depthOf(g, x) < depthOf(g, y); });
            std::unordered_set<NodeId> oriented(list.begin(), list.end());

            json out = json::array();
            for (NodeId id : list)
            {
                g.evaluate();
                Node* j = g.node(id);
                std::vector<std::pair<NodeId, glm::dmat4>> kids;
                for (NodeId c : g.children(id))
                    kids.push_back({c, worldOf(*g.node(c))});

                glm::dmat3 local(1.0);
                std::vector<NodeId> cj = jointChildren(g, id);
                glm::dvec3 aim = cj.empty() ? glm::dvec3(0.0) : positionOf(*g.node(cj[0])) - positionOf(*j);
                if (glm::length(aim) > 1e-9)
                    local = localRotation(*j, rig::aimFrame(primary, secondary, aim, worldUp));
                setOrientation(g, id, local);

                g.evaluate();
                glm::dmat4 inv = glm::inverse(worldOf(*g.node(id)));
                for (auto& [c, world] : kids)
                {
                    // Joints oriented later only need their position kept; their rotation is about to be replaced.
                    if (oriented.contains(c))
                        g.setValue({c, Transform::kTranslate}, clean(glm::dvec3(inv * world[3])));
                    else
                        Transform::setLocalMatrix(g, c, inv * world);
                }
                out.push_back(j->name());
            }
            return out;
        },
    });

    cmds::registerCommand({
        .name = "mirrorJoints",
        .doc = "Mirror a joint hierarchy across a plane through the world origin. behavior=True mirrors the axes so "
               "equal rotations make mirrored poses (Maya's Behavior); False keeps world orientation. Names swap "
               "searchReplace; side labels swap left/right. Returns the new joints.",
        .positionalDoc = "root",
        .flags = {
            {"axis", "ax", T::String, "x", "Mirror across this axis: x mirrors across the YZ plane."},
            {"behavior", "b", T::Bool, true, "Mirror behavior instead of orientation."},
            {"searchReplace", "sr", T::List, json::array({"_l", "_r"}), "[search, replace] for the new names."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("mirrorJoints: give a root joint");
            NodeId root = jointNamed(objs.front(), "mirrorJoints").id();
            glm::dvec3 axis = axisArg(a.flags["axis"], "mirrorJoints");
            glm::dmat3 reflect = glm::dmat3(1.0) - 2.0 * glm::outerProduct(axis, axis);
            bool behavior = a.get<bool>("behavior");
            json sr = a.flags["searchReplace"];
            std::string search = sr.size() > 0 ? sr[0].get<std::string>() : "";
            std::string replace = sr.size() > 1 ? sr[1].get<std::string>() : "";

            // Joints only, top-down, stopping at anything that isn't a joint.
            std::vector<NodeId> order{root};
            for (size_t i = 0; i < order.size(); ++i)
                for (NodeId c : jointChildren(g, order[i]))
                    order.push_back(c);

            g.evaluate();
            std::unordered_map<NodeId, NodeId> copies;
            json out = json::array();
            for (NodeId src : order)
            {
                Node* s = g.node(src);
                NodeId p = g.parent(src);
                NodeId parent = copies.contains(p) ? copies[p] : p;
                std::string want = s->name();
                std::string swapped = rig::mirrorName(want, search, replace);
                if (swapped == want)
                    swapped = rig::mirrorName(want, replace, search);
                Node* d = guarded([&] { return g.createNode(s->type(), swapped, parent); });
                NodeId dst = d->id();
                copies[src] = dst;
                for (int attr : {int(Transform::kRotateOrder), int(Transform::kScale), int(Joint::kRadius), int(Joint::kSegmentScaleCompensate),
                                 int(Joint::kJointType), int(Joint::kOtherType), int(Joint::kDrawStyle), int(Transform::kVisibility)})
                    g.setValue({dst, attr}, s->value(attr));
                int64_t side = s->as<int64_t>(Joint::kSide);
                g.setValue({dst, Joint::kSide}, side == Joint::kLeft ? int64_t(Joint::kRight) : side == Joint::kRight ? int64_t(Joint::kLeft) : side);

                glm::dmat4 sw = worldOf(*s);
                glm::dmat3 r = orthonormal(glm::dmat3(sw));
                glm::dmat3 rm = behavior ? -(reflect * r) : r;
                glm::dvec3 pos = reflect * glm::dvec3(sw[3]);

                g.evaluate();
                glm::dmat4 parentWorld = g.node(dst)->as<glm::dmat4>(Transform::kParentMatrix);
                glm::dmat3 local = glm::transpose(orthonormal(glm::dmat3(parentWorld))) * rm;
                // Keep the source's rotate values (so poses and keys carry over) and put the rest in jointOrient.
                auto order3 = RotateOrder(s->as<int64_t>(Transform::kRotateOrder));
                glm::dvec3 rot = s->as<glm::dvec3>(Transform::kRotate);
                glm::dmat3 jo = local * glm::transpose(glm::dmat3(eulerToMatrix(rot, order3)));
                g.setValue({dst, Transform::kRotate}, rot);
                g.setValue({dst, Joint::kJointOrient}, clean(matrixToEuler(jo, RotateOrder::XYZ)));
                g.setValue({dst, Transform::kTranslate}, clean(glm::dvec3(glm::inverse(parentWorld) * glm::dvec4(pos, 1.0))));
                out.push_back(d->name());
            }
            return out;
        },
    });

    cmds::registerCommand({
        .name = "jointLength",
        .doc = "Length of a joint's bone (distance to its first child joint). With length= the child slides along the "
               "bone to that length. Returns the length.",
        .positionalDoc = "joint",
        .flags = {{"length", "l", T::Float, nullptr, "New length, cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("jointLength: give a joint");
            NodeId id = jointNamed(objs.back(), "jointLength").id();
            std::vector<NodeId> kids = jointChildren(g, id);
            if (kids.empty())
                throw CommandError("jointLength: " + objs.back() + " has no child joint");
            g.evaluate();
            Node* child = g.node(kids[0]);
            double len = glm::length(positionOf(*child) - positionOf(*g.node(id)));
            if (!a.has("length"))
                return len;
            glm::dvec3 t = child->as<glm::dvec3>(Transform::kTranslate);
            double want = a.get<double>("length");
            if (glm::length(t) < 1e-12)
                throw CommandError("jointLength: the child sits on the joint, so the bone has no direction");
            // Scaling the local offset scales the world length by the same factor.
            g.setValue({kids[0], Transform::kTranslate}, t * (want / len));
            return want;
        },
    });

    cmds::registerCommand({
        .name = "insertJoint",
        .doc = "Split a bone: insert count joints evenly between a joint and its child. Returns the new joints.",
        .positionalDoc = "joint",
        .flags = {
            {"count", "c", T::Int, 1, "Joints to insert."},
            {"child", "ch", T::String, nullptr, "Which child bone to split; defaults to the first child joint."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("insertJoint: give a joint");
            NodeId id = jointNamed(objs.back(), "insertJoint").id();
            NodeId child = kNoNode;
            if (a.has("child"))
                child = jointNamed(a.get<std::string>("child"), "insertJoint").id();
            else if (auto kids = jointChildren(g, id); !kids.empty())
                child = kids[0];
            if (child == kNoNode || g.parent(child) != id)
                throw CommandError("insertJoint: " + objs.back() + " has no such child joint");
            int count = std::max(1, a.get<int>("count"));
            g.evaluate();
            glm::dvec3 from = positionOf(*g.node(id)), to = positionOf(*g.node(child));
            json out = json::array();
            NodeId parent = id;
            for (int k = 1; k <= count; ++k)
            {
                Node* j = guarded([&] { return g.createNode(*types::find("joint"), g.node(id)->name(), parent); });
                g.setValue({j->id(), Joint::kRadius}, g.node(id)->value(Joint::kRadius));
                placeAt(g, j->id(), glm::mix(from, to, double(k) / double(count + 1)));
                parent = j->id();
                out.push_back(j->name());
            }
            guarded([&] { g.setParent(child, parent, true); return 0; });
            return out;
        },
    });

    cmds::registerCommand({
        .name = "rerootSkeleton",
        .doc = "Make a joint the root of its skeleton by reversing the parent chain above it. Joints keep their world "
               "placement.",
        .positionalDoc = "joint",
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("rerootSkeleton: give a joint");
            NodeId id = jointNamed(objs.back(), "rerootSkeleton").id();
            std::vector<NodeId> path{id};
            for (NodeId p = g.parent(id); p != kNoNode && rig::isJoint(*g.node(p)); p = g.parent(p))
                path.push_back(p);
            if (path.size() == 1)
                return g.node(id)->name();
            NodeId top = g.parent(path.back());
            guarded([&] {
                g.setParent(id, top, true);
                for (size_t k = 1; k < path.size(); ++k)
                    g.setParent(path[k], path[k - 1], true);
                return 0;
            });
            return g.node(id)->name();
        },
    });
}
} // namespace

void registerRigTypes()
{
    if (types::find("joint"))
        return;
    const NodeType* transform = types::find("transform");
    if (!transform)
        throw std::logic_error("registerRigTypes needs the built-in types first");
    types::registerType(Joint::makeType(*transform));
    registerSkinTypes();
    registerConstraintTypes();
}

void registerRigCommands()
{
    if (cmds::find("joint"))
        return;
    registerJointCommands();
    registerSkinCommands();
    registerConstraintCommands();
}
} // namespace lila::scene
