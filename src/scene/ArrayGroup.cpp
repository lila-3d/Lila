#include "scene/ArrayGroup.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Repeat.h"
#include "scene/TransformOps.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <unordered_map>

namespace lila::scene::arrays
{
namespace
{
using T = ArgType;

constexpr uint32_t kKeyed = AttrInput | AttrStorable | AttrKeyable;

const std::vector<std::string> kModeNames = {"linear", "radial", "grid"};
const std::vector<std::string> kAxisNames = {"x", "y", "z"};
const std::vector<std::string> kAngleModeNames = {"total", "step"};

AttrDef vec3(std::string name, std::string shortName, glm::dvec3 def, std::string doc, const char* category = "")
{
    AttrDef a{.name = name, .shortName = shortName, .type = PlugType::Vec3, .defaultValue = def, .flags = kKeyed, .doc = std::move(doc),
              .category = category};
    for (int i = 0; i < 3; ++i)
    {
        a.childNames[i] = name + char('X' + i);
        a.childShort[i] = shortName + char('x' + i);
    }
    return a;
}

glm::dmat4 worldOf(const Node& n) { return n.as<glm::dmat4>(Transform::kWorldMatrix); }

int choice(const Args& a, const char* flag, const std::vector<std::string>& names, const char* cmd)
{
    const std::string v = a.get<std::string>(flag);
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == v)
            return int(i);
    std::string list;
    for (const std::string& n : names)
        list += (list.empty() ? "" : ", ") + n;
    throw CommandError(std::string(cmd) + ": " + flag + " is " + list + ", not '" + v + "'");
}

glm::dvec3 vecFlag(const Args& a, const char* flag, const char* cmd)
{
    const json& v = a.flags[flag];
    if (v.is_number())
        return glm::dvec3(v.get<double>());
    if (!v.is_array() || v.size() != 3)
        throw CommandError(std::string(cmd) + ": " + flag + " takes three numbers");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

/** Sets every layout setting the call gives. */
void applySettings(Graph& g, NodeId id, const Args& a)
{
    auto set = [&](int attr, PlugValue v) { g.setValue({id, attr, -1}, std::move(v)); };
    if (a.has("mode"))
        set(ArrayGroup::kMode, int64_t(choice(a, "mode", kModeNames, "array")));
    if (a.has("count"))
        set(ArrayGroup::kCopies, int64_t(std::clamp(a.get<int>("count"), 1, kMaxCount)));
    if (a.has("offset"))
        set(ArrayGroup::kOffset, vecFlag(a, "offset", "array"));
    if (a.has("rotate"))
        set(ArrayGroup::kCopyRotate, vecFlag(a, "rotate", "array"));
    if (a.has("scale"))
        set(ArrayGroup::kCopyScale, vecFlag(a, "scale", "array"));
    if (a.has("axis"))
        set(ArrayGroup::kAxis, int64_t(choice(a, "axis", kAxisNames, "array")));
    if (a.has("angle"))
        set(ArrayGroup::kAngle, a.get<double>("angle"));
    if (a.has("angleMode"))
        set(ArrayGroup::kAngleMode, int64_t(choice(a, "angleMode", kAngleModeNames, "array")));
    if (a.has("rise"))
        set(ArrayGroup::kRise, a.get<double>("rise"));
    if (a.has("countY"))
        set(ArrayGroup::kCountY, int64_t(std::clamp(a.get<int>("countY"), 1, kMaxCount)));
    if (a.has("countZ"))
        set(ArrayGroup::kCountZ, int64_t(std::clamp(a.get<int>("countZ"), 1, kMaxCount)));
    if (a.has("spacing"))
        set(ArrayGroup::kSpacing, vecFlag(a, "spacing", "array"));
}

json pathList(Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        out.push_back(g.node(id)->path());
    return out;
}

Node& arrayNamed(Graph& g, const std::vector<std::string>& objs, const char* cmd)
{
    Node* n = objs.empty() ? nullptr : g.find(objs.front());
    if (!n || !isArray(*n))
        throw CommandError(std::string(cmd) + ": give an array");
    return *n;
}
} // namespace

bool isArray(const Node& n) { return n.type().isA("array"); }

std::vector<NodeId> sources(const Graph& g, NodeId array)
{
    std::vector<NodeId> out;
    for (NodeId c : g.children(array))
        if (const Node* n = g.node(c); n && n->type().isA("transform"))
            out.push_back(c);
    return out;
}

geo::ArrayLayout layout(const Node& n)
{
    geo::ArrayLayout l;
    l.mode = geo::ArrayMode(std::clamp<int64_t>(n.as<int64_t>(ArrayGroup::kMode), 0, 2));
    l.count = int(std::clamp<int64_t>(n.as<int64_t>(ArrayGroup::kCopies), 1, kMaxCount));
    l.gridCount = {l.count, int(std::clamp<int64_t>(n.as<int64_t>(ArrayGroup::kCountY), 1, kMaxCount)),
                   int(std::clamp<int64_t>(n.as<int64_t>(ArrayGroup::kCountZ), 1, kMaxCount))};
    // A stray count must not hang the viewport: grids stay under the total by halving.
    while (int64_t(l.gridCount.x) * l.gridCount.y * l.gridCount.z > kMaxTotal)
        l.gridCount = glm::max(l.gridCount / 2, glm::ivec3(1));
    l.offset = l.mode == geo::ArrayMode::Grid     ? n.as<glm::dvec3>(ArrayGroup::kSpacing)
               : l.mode == geo::ArrayMode::Linear ? n.as<glm::dvec3>(ArrayGroup::kOffset)
                                                  : glm::dvec3(0.0);
    l.rotate = n.as<glm::dvec3>(ArrayGroup::kCopyRotate);
    l.scale = n.as<glm::dvec3>(ArrayGroup::kCopyScale);
    l.axis = int(std::clamp<int64_t>(n.as<int64_t>(ArrayGroup::kAxis), 0, 2));
    l.angle = n.as<double>(ArrayGroup::kAngle);
    l.stepAngle = n.as<int64_t>(ArrayGroup::kAngleMode) == 1;
    l.rise = n.as<double>(ArrayGroup::kRise);
    l.frame = geo::pivotFrame(n.as<glm::dvec3>(Transform::kRotatePivot));
    return l;
}

std::vector<glm::dmat4> copyTransforms(const Node& array)
{
    std::vector<glm::dmat4> out = geo::arrayCopyTransforms(layout(array));
    const glm::dmat4 w = worldOf(array), wi = glm::inverse(w);
    for (size_t k = 1; k < out.size(); ++k)
        out[k] = w * out[k] * wi;
    return out;
}

std::vector<Copy> copiesOf(const Graph& g, NodeId id)
{
    std::vector<Copy> acc{{glm::dmat4(1.0), kNoNode}};
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
    {
        const Node* n = g.node(p);
        if (!n || !isArray(*n))
            continue;
        const std::vector<glm::dmat4> xf = copyTransforms(*n);
        if (xf.size() < 2)
            continue;
        std::vector<Copy> next;
        next.reserve(std::min(acc.size() * xf.size(), size_t(kMaxTotal)));
        for (size_t k = 0; k < xf.size() && next.size() < size_t(kMaxTotal); ++k)
            for (const Copy& c : acc)
                next.push_back({xf[k] * c.transform, k ? p : c.array});
        acc = std::move(next);
    }
    // The first entry is every array's original: the node itself.
    acc.erase(acc.begin());
    return acc;
}

NodeId arrayAbove(const Graph& g, NodeId id)
{
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        if (const Node* n = g.node(p); n && isArray(*n))
            return p;
    return kNoNode;
}

NodeId bake(Graph& g, NodeId array, duplicate::Mode mode)
{
    LILA_ZONE();

    g.evaluate();
    Node* arr = g.node(array);
    if (!arr || !isArray(*arr))
        throw CommandError("bakeArray: " + (arr ? arr->path() : std::string("that")) + " is not an array");
    const std::vector<glm::dmat4> xf = copyTransforms(*arr);
    const std::string name = arr->name();
    Node* group = g.createNode(*types::find("transform"), name, g.parent(array), false);
    const NodeId gid = group->id();
    // Its transform channels as they evaluate now, so a keyed array bakes where it stands.
    for (int a : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotateOrder, Transform::kRotatePivot,
                  Transform::kScalePivot, Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kPivotOrient,
                  Transform::kRotateAxis, Transform::kVisibility})
        g.setValue({gid, a, -1}, arr->value(a));
    const std::vector<NodeId> srcs = sources(g, array);
    for (NodeId s : srcs)
        g.setParent(s, gid, true);
    g.evaluate();
    // One map per copy: history the sources share is copied once per copy, never shared between copies.
    std::vector<std::unordered_map<NodeId, NodeId>> copied(xf.size());
    for (NodeId s : srcs)
    {
        const glm::dmat4 base = worldOf(*g.node(s));
        glm::dvec3 hint = g.node(s)->as<glm::dvec3>(Transform::kRotate);
        for (size_t k = 1; k < xf.size(); ++k)
        {
            const NodeId c = duplicate::copyTree(g, s, gid, mode, "", copied[k]);
            // Each copy's channels follow on from the last one's, so a ring keeps counting past 180 instead of flipping.
            repeat::setWorldMatrix(g, c, xf[k] * base, hint);
            hint = g.node(c)->as<glm::dvec3>(Transform::kRotate);
        }
    }
    g.deleteNode(array);
    g.rename(gid, name);
    return gid;
}

void registerTypes()
{
    if (types::find("array"))
        return;
    NodeType t;
    t.name = "array";
    t.icon = "layout-grid";
    t.base = types::find("transform");
    t.dag = true;
    t.doc = "A live array: shows its children repeated in a row, around a circle or in a grid about its pivot, drawn and exported as "
            "instances of them. Edit a child and every copy follows; bakeArray makes the copies real objects.";
    t.ownAttrs = {
        {.name = "mode", .shortName = "amd", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = kModeNames,
         .doc = "linear steps each copy by offset; radial turns copies about the pivot; grid fills rows, columns and layers.",
         .category = "Array"},
        {.name = "count", .shortName = "acn", .type = PlugType::Int, .defaultValue = int64_t(5), .flags = kKeyed,
         .doc = "Copies including the original. In grid mode, copies along x.", .minValue = 1.0, .maxValue = double(kMaxCount),
         .softMax = 50.0, .category = "Array"},
        vec3("offset", "aof", glm::dvec3(100.0, 0.0, 0.0), "Linear: step from one copy to the next, in the array's space, cm.", "Array"),
        vec3("copyRotate", "acr", glm::dvec3(0.0), "Extra rotation per copy about the pivot, degrees. A linear array with a rotation curves.", "Array"),
        vec3("copyScale", "acs", glm::dvec3(1.0), "Scale per copy about the pivot: each copy is this much the size of the last.", "Array"),
        {.name = "axis", .shortName = "aax", .type = PlugType::Enum, .defaultValue = int64_t(1), .enumNames = kAxisNames,
         .doc = "The array's axis radial copies turn about.", .category = "Radial"},
        {.name = "angle", .shortName = "aan", .type = PlugType::Float, .defaultValue = 360.0, .flags = kKeyed,
         .doc = "Degrees the copies span, or between neighbours when angleMode is step. A whole turn closes the ring.", .softMin = -360.0,
         .softMax = 360.0, .category = "Radial"},
        {.name = "angleMode", .shortName = "aam", .type = PlugType::Enum, .defaultValue = int64_t(0), .enumNames = kAngleModeNames,
         .doc = "total spreads count copies over angle; step puts angle between neighbours.", .category = "Radial"},
        {.name = "rise", .shortName = "ars", .type = PlugType::Float, .defaultValue = 0.0, .flags = kKeyed,
         .doc = "Distance each copy climbs along the axis, cm, for spirals and stairs.", .category = "Radial"},
        {.name = "countY", .shortName = "acy", .type = PlugType::Int, .defaultValue = int64_t(3), .flags = kKeyed,
         .doc = "Grid copies along y.", .minValue = 1.0, .maxValue = double(kMaxCount), .softMax = 50.0, .category = "Grid"},
        {.name = "countZ", .shortName = "acz", .type = PlugType::Int, .defaultValue = int64_t(1), .flags = kKeyed,
         .doc = "Grid copies along z.", .minValue = 1.0, .maxValue = double(kMaxCount), .softMax = 50.0, .category = "Grid"},
        vec3("spacing", "asp", glm::dvec3(100.0), "Distance between grid copies along each axis, in the array's space, cm.", "Grid"),
    };
    t.create = [] { return std::make_unique<ArrayGroup>(); };
    types::registerType(std::move(t));
}

void registerCommands()
{
    if (ops::find("array"))
        return;

    ops::registerCommand({
        .name = "array",
        .doc = "Make a live array of objects, or edit the array named. The objects become its children and it shows them repeated "
               "in a row, around a circle or in a grid about its pivot, drawn and exported as instances. Editing an object updates "
               "every copy; bakeArray makes them real objects. Returns the array's path.\n\n"
               "Settings left out keep the array's current values (a new array's defaults: linear, 5 copies 100 cm apart).",
        .positionalDoc = "objects, or one array to edit",
        .flags = {{"mode", "m", T::String, nullptr, "linear, radial or grid.", kModeNames},
                  {"count", "c", T::Int, nullptr, "Copies including the original, 1 to 10000; along x in grid mode."},
                  {"offset", "o", T::Vec3, nullptr, "Linear: step between copies in the array's space, cm."},
                  {"rotate", "r", T::Vec3, nullptr, "Rotation per copy about the centre, degrees (XYZ)."},
                  {"scale", "s", T::Vec3, nullptr, "Scale per copy about the centre."},
                  {"axis", "ax", T::String, nullptr, "Radial: the axis copies turn about. A new array defaults to the scene's up axis.", kAxisNames},
                  {"angle", "a", T::Float, nullptr, "Radial: degrees spanned (360 closes the ring), or between neighbours with angleMode step."},
                  {"angleMode", "am", T::String, nullptr, "Radial: total or step.", kAngleModeNames},
                  {"rise", "ri", T::Float, nullptr, "Radial: climb per copy along the axis, cm."},
                  {"countY", "cy", T::Int, nullptr, "Grid: copies along y."},
                  {"countZ", "cz", T::Int, nullptr, "Grid: copies along z."},
                  {"spacing", "sp", T::Vec3, nullptr, "Grid: distance between copies along each axis, cm."},
                  {"center", "ce", T::Vec3, nullptr,
                   "World point the copies turn and scale about (the array's pivot). A new array defaults to the objects' pivot."},
                  {"edit", "e", T::Bool, nullptr, "With one array given: edit it (the default), or with False, make an array of it."},
                  {"name", "n", T::String, nullptr, "Name of a new array."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            std::vector<NodeId> ids;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n || !n->type().isA("transform"))
                    throw CommandError("array: " + name + " is not an object");
                ids.push_back(n->id());
            }
            ids = xformops::topLevel(g, ids);
            if (ids.empty())
                throw CommandError("array: select the objects to array");
            g.evaluate();
            NodeId id = kNoNode;
            const bool editing = ids.size() == 1 && isArray(*g.node(ids[0])) && a.get<bool>("edit", true);
            if (editing)
            {
                id = ids[0];
                if (a.has("name"))
                    g.rename(id, a.get<std::string>("name"));
            }
            else
            {
                NodeId parent = g.parent(ids.front());
                for (NodeId s : ids)
                    if (g.parent(s) != parent)
                        parent = kNoNode;
                glm::dvec3 pivot(0.0);
                for (NodeId s : ids)
                    pivot += xformops::worldPivot(g, s);
                pivot /= double(ids.size());
                const glm::dvec3 center = a.has("center") ? vecFlag(a, "center", "array") : pivot;
                Node* made = g.createNode(*types::find("array"), a.has("name") ? a.get<std::string>("name") : std::string("Array"), parent);
                id = made->id();
                const glm::dmat4 parentWorld = parent != kNoNode ? worldOf(*g.node(parent)) : glm::dmat4(1.0);
                g.setValue({id, Transform::kTranslate, -1}, glm::dvec3(glm::inverse(parentWorld) * glm::dvec4(center, 1.0)));
                const glm::dvec3 up = sceneUp();
                g.setValue({id, ArrayGroup::kAxis, -1}, int64_t(std::abs(up.z) > 0.5 ? 2 : 1));
                g.evaluate();
                for (NodeId s : ids)
                    g.setParent(s, id, true);
            }
            applySettings(g, id, a);
            if (editing && a.has("center"))
                xformops::setWorldPivot(g, id, vecFlag(a, "center", "array"));
            g.select({id});
            return g.node(id)->path();
        },
        .adjustable = true,
        .examples = {"array(\"Pillar\", count=8, offset=[300, 0, 0])",
                     "array(\"Spoke\", mode=\"radial\", count=12, center=[0, 0, 0])",
                     "array(\"Step\", mode=\"radial\", count=24, angle=15, angleMode=\"step\", rise=18, center=[0, 0, 0])",
                     "array(\"Crate\", mode=\"grid\", count=4, countY=1, countZ=3, spacing=[120, 0, 120])"},
    });

    ops::options::Override o;
    o.order = {"mode", "count", "axis", "angle", "angleMode", "rise", "countY", "countZ"};
    o.hidden = {"edit", "name", "center"};
    o.adjust = {"offset", "rotate", "scale", "spacing"};
    ops::options::setOverride("array", std::move(o));

    ops::registerCommand({
        .name = "bakeArray",
        .doc = "Turn arrays into plain groups holding their sources and a real object per copy, placed where the copies were. "
               "Returns the groups' paths.",
        .positionalDoc = "arrays",
        .flags = {{"mode", "m", T::String, "instance", "What each copy's meshes share with the source's.", {"instance", "copy", "reference"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const duplicate::Mode mode = duplicate::parseMode(a.get<std::string>("mode", "instance"));
            std::vector<NodeId> arrays;
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && isArray(*n))
                    arrays.push_back(n->id());
            if (arrays.empty())
                throw CommandError("bakeArray: select an array");
            std::vector<NodeId> groups;
            // Inner arrays first, so baking an outer one copies already baked contents.
            std::ranges::sort(arrays, [&](NodeId x, NodeId y) { return g.descendants(x).size() < g.descendants(y).size(); });
            for (NodeId id : arrays)
                if (g.node(id))
                    groups.push_back(bake(g, id, mode));
            std::erase_if(groups, [&](NodeId id) { return !g.node(id); });
            g.select(groups);
            return pathList(g, groups);
        },
        .examples = {"bakeArray(\"Array\")", "bakeArray(\"Array\", mode=\"copy\")"},
    });

    ops::registerCommand({
        .name = "arrayInfo",
        .doc = "An array's settings summary: mode, copies (the original included), its sources, and for each source the world "
               "matrix of every generated copy (16 numbers, column by column).",
        .positionalDoc = "array",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            g.evaluate();
            Node& n = arrayNamed(g, a.objects(), "arrayInfo");
            const std::vector<glm::dmat4> xf = copyTransforms(n);
            json placements = json::array();
            for (NodeId s : sources(g, n.id()))
            {
                json worlds = json::array();
                const glm::dmat4 w = worldOf(*g.node(s));
                for (size_t k = 1; k < xf.size(); ++k)
                    worlds.push_back(toJson(PlugValue(xf[k] * w)));
                placements.push_back({{"source", g.node(s)->path()}, {"worlds", std::move(worlds)}});
            }
            return {{"array", n.path()},
                    {"mode", kModeNames[size_t(layout(n).mode)]},
                    {"copies", xf.size()},
                    {"sources", pathList(g, sources(g, n.id()))},
                    {"placements", std::move(placements)}};
        },
        .examples = {"arrayInfo(\"Array\")[\"copies\"]"},
    });
}
} // namespace lila::scene::arrays
