#include "scene/Duplicate.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/Profile.h"
#include "scene/Components.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/MeshNodes.h"
#include "scene/Repeat.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

#include <algorithm>
#include <functional>

namespace lila::scene::duplicate
{
namespace
{
Graph& graph() { return Graph::current(); }

std::vector<Pair>& pairs()
{
    static std::vector<Pair> p;
    return p;
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
    {
        Node* node = g.find(n);
        if (!node)
            throw CommandError(std::string(cmd) + ": no node named '" + n + "'");
        ids.push_back(node->id());
    }
    return xformops::topLevel(g, ids);
}

glm::dmat4 worldOf(Graph& g, NodeId id) { return std::get<glm::dmat4>(g.value({id, Transform::kWorldMatrix, -1})); }

glm::dvec3 rotateOf(const Graph& g, NodeId id) { return g.node(id)->as<glm::dvec3>(Transform::kRotate); }

glm::dvec3 vecFlag(const Args& a, const char* flag, double fallback)
{
    if (!a.has(flag))
        return glm::dvec3(fallback);
    const json& v = a.flags[flag];
    if (v.is_number())
        return glm::dvec3(v.get<double>());
    if (!v.is_array() || v.size() != 3)
        throw CommandError(std::string("duplicate: ") + flag + " takes three numbers");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

json pathsOf(Graph& g, const std::vector<NodeId>& ids)
{
    json out = json::array();
    for (NodeId id : ids)
        out.push_back(g.node(id)->path());
    return out;
}

struct Request
{
    int count = 1;
    Mode mode = Mode::Copy;
    repeat::Step step;
    std::string pivot = "object";
    glm::dvec3 pivotPoint{0.0};
    std::string space = "world";
    bool group = false;
    std::string name;
};

/** The copies of every root, in order, with the pairs Duplicate with Transform repeats. */
std::vector<NodeId> duplicateRoots(Graph& g, const std::vector<NodeId>& roots, const Request& r)
{
    LILA_ZONE();

    g.evaluate();
    // One map per repetition: history shared by several roots is copied once per copy, not shared by every copy.
    std::vector<std::unordered_map<NodeId, NodeId>> copies(size_t(r.count));
    std::vector<NodeId> made;
    std::vector<Pair> recorded;
    for (NodeId root : roots)
    {
        const glm::dmat4 base = worldOf(g, root);
        const glm::dvec3 pivot = r.pivot == "world" ? glm::dvec3(0.0) : r.pivot == "point" ? r.pivotPoint : xformops::worldPivot(g, root);
        const glm::dmat3 frame = r.space == "object" ? xformops::frame(g, root, Space::Object) : glm::dmat3(1.0);
        const std::vector<glm::dmat4> placed = repeat::copyMatrices(base, repeat::stepMatrix(r.step, pivot, frame), r.count);
        NodeId prev = root, last = root;
        for (int i = 0; i < r.count; ++i)
        {
            NodeId c = copyTree(g, root, g.parent(root), r.mode, r.name, copies[size_t(i)]);
            if (!r.step.identity())
                repeat::setWorldMatrix(g, c, placed[size_t(i)], rotateOf(g, root) + r.step.rotate * double(i + 1));
            made.push_back(c);
            prev = last;
            last = c;
        }
        recorded.push_back({prev, last, r.mode});
    }
    if (r.group)
    {
        NodeId parent = g.parent(roots.front());
        for (NodeId root : roots)
            if (g.parent(root) != parent)
                parent = kNoNode;
        Node* grp = g.createNode(*types::find("transform"), "Group", parent, true);
        for (NodeId c : made)
            g.setParent(c, grp->id());
    }
    setLastPairs(std::move(recorded));
    return made;
}
} // namespace

const char* modeName(Mode m)
{
    switch (m)
    {
    case Mode::Instance: return "instance";
    case Mode::Reference: return "reference";
    case Mode::Copy: break;
    }
    return "copy";
}

Mode parseMode(std::string_view s)
{
    if (s == "copy")
        return Mode::Copy;
    if (s == "instance")
        return Mode::Instance;
    if (s == "reference")
        return Mode::Reference;
    throw CommandError("unknown duplicate mode '" + std::string(s) + "' (copy, instance or reference)");
}

void copyValues(Graph& g, const Node& from, Node& to)
{
    const auto& defs = from.type().attrs;
    for (int i = 0; i < int(defs.size()); ++i)
        if ((defs[i].flags & AttrInput) && !(defs[i].flags & (AttrUnique | AttrArray)) && !g.source({from.id(), i, -1}))
            g.setValue({to.id(), i, -1}, from.value(i));
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

NodeId copyTree(Graph& g, NodeId src, NodeId parent, Mode mode, std::string_view name, std::unordered_map<NodeId, NodeId>& copies)
{
    std::function<NodeId(NodeId, NodeId, bool)> copy = [&](NodeId s, NodeId p, bool top) {
        Node* n = g.node(s);
        const bool named = top && !name.empty();
        Node* c = g.createNode(n->type(), named ? std::string(name) : n->name(), p, named);
        copyValues(g, *n, *c);
        if (Node* layer = displayLayers::layerOf(g, s))
            displayLayers::addMembers(g, layer->id(), {c->id()});
        if (n->type().isA("mesh") && !n->type().isA("lattice"))
        {
            if (mode == Mode::Instance)
                instancing::connectInstance(g, s, c->id());
            else if (mode == Mode::Reference)
                instancing::connectReference(g, s, c->id());
            else if (instancing::instanceParent(g, s) != kNoNode)
            {
                // A copy of an instance is a mesh of its own, not one more instance.
                std::unordered_map<NodeId, NodeId> own;
                instancing::copyHistory(g, instancing::root(g, s), c->id(), own);
            }
            else
                instancing::copyHistory(g, s, c->id(), copies);
        }
        else if (n->type().isA("mesh"))
            instancing::copyHistory(g, s, c->id(), copies);
        else if (components::pointProvider(*n))
            copyEvaluatedInputs(g, *n, *c);
        for (NodeId child : std::vector<NodeId>(g.children(s)))
            copy(child, c->id(), false);
        return c->id();
    };
    return copy(src, parent, true);
}

const std::vector<Pair>& lastPairs() { return pairs(); }

void setLastPairs(std::vector<Pair> p) { pairs() = std::move(p); }

void clearLastPairs() { pairs().clear(); }

void registerCommands()
{
    using T = ArgType;

    static bool listening = false;
    if (!listening)
    {
        listening = true;
        graph().addListener(
            [](Change c, NodeId id) {
                if (c == Change::Cleared)
                    pairs().clear();
                else if (c == Change::Deleted)
                    std::erase_if(pairs(), [id](const Pair& p) { return p.source == id || p.copy == id; });
            },
            true);
    }

    ops::registerCommand({
        .name = "duplicate",
        .doc = "Duplicate objects with their children, keeping each mesh's generator and modifier stack live. Returns the "
               "new top-level names.\n\n"
               "count makes a run of copies, copy i placed by the translate/rotate/scale step applied i times about the "
               "pivot: a row, a ring or a spiral in one call. mode instance shares the mesh (edits to one change all); "
               "reference shares the base mesh but gives each copy its own modifier stack.",
        .positionalDoc = "objects",
        .flags = {{"name", "n", T::String, nullptr, "Name for a single duplicate."},
                  {"count", "c", T::Int, 1, "Copies of each object, 1 to 10000."},
                  {"translate", "t", T::Vec3, json::array({0.0, 0.0, 0.0}), "Move from one copy to the next, cm."},
                  {"rotate", "r", T::Vec3, json::array({0.0, 0.0, 0.0}), "Rotation from one copy to the next about the pivot, degrees (XYZ)."},
                  {"scale", "s", T::Vec3, json::array({1.0, 1.0, 1.0}), "Scale from one copy to the next about the pivot."},
                  {"pivot", "", T::String, "object", "What the step rotates and scales about.", {"object", "world", "point"}},
                  {"pivotPoint", "", T::Vec3, json::array({0.0, 0.0, 0.0}), "World point rotated and scaled about when pivot is point, cm."},
                  {"space", "", T::String, "world", "Axes the step is measured along: the world's or each object's own.", {"world", "object"}},
                  {"mode", "m", T::String, "copy", "What a duplicated mesh shares with its source.", {"copy", "instance", "reference"}},
                  {"group", "g", T::Bool, false, "Put the copies under a new group."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> roots = topLevel(g, a.objects(), "duplicate");
            if (roots.empty())
                throw CommandError("duplicate: nothing to duplicate");
            Request r;
            r.count = std::clamp(a.get<int>("count", 1), 1, 10000);
            r.mode = parseMode(a.get<std::string>("mode", "copy"));
            r.step = {vecFlag(a, "translate", 0.0), vecFlag(a, "rotate", 0.0), vecFlag(a, "scale", 1.0)};
            r.pivot = a.get<std::string>("pivot", "object");
            if (r.pivot != "object" && r.pivot != "world" && r.pivot != "point")
                throw CommandError("duplicate: pivot is object, world or point");
            r.pivotPoint = vecFlag(a, "pivotPoint", 0.0);
            r.space = a.get<std::string>("space", "world");
            if (r.space != "world" && r.space != "object")
                throw CommandError("duplicate: space is world or object");
            r.group = a.get<bool>("group", false);
            if (a.has("name") && roots.size() == 1)
                r.name = a.get<std::string>("name");
            std::vector<NodeId> made = duplicateRoots(g, roots, r);
            g.select(made);
            return pathsOf(g, made);
        },
        .adjustable = true,
        .examples = {"duplicate(\"Pillar\", count=7, translate=[200, 0, 0])",
                     "duplicate(\"Spoke\", count=11, rotate=[0, 30, 0], pivot=\"world\", mode=\"instance\")",
                     "duplicate(\"Step\", count=20, translate=[0, 18, 0], rotate=[0, 15, 0], pivot=\"point\", pivotPoint=[0, 0, 0])"},
    });

    ops::options::Override o;
    o.order = {"mode", "group"};
    o.hidden = {"count", "translate", "rotate", "scale", "pivot", "pivotPoint", "space"};
    o.adjust = {"translate", "rotate", "scale", "pivotPoint"};
    ops::options::setOverride("duplicate", std::move(o));

    ops::registerCommand({
        .name = "duplicateWithTransform",
        .doc = "Duplicate the last duplicated object again, placed by how its copy now sits relative to its source. Duplicate "
               "an object, move, rotate or scale the copy, then repeat this to continue the run. Returns the new names.\n\n"
               "The step is measured from the current world matrices of the last source and copy, so it follows any later "
               "edit to the copy and survives selection changes. It is forgotten when either object is deleted.",
        .flags = {{"count", "c", T::Int, 1, "Copies to add, 1 to 10000."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<Pair> from = lastPairs();
            if (from.empty())
                throw CommandError("duplicateWithTransform: nothing to repeat; duplicate an object first");
            const int count = std::clamp(a.get<int>("count", 1), 1, 10000);
            g.evaluate();
            std::unordered_map<NodeId, NodeId> copies;
            std::vector<NodeId> made;
            std::vector<Pair> recorded;
            for (const Pair& p : from)
            {
                const glm::dmat4 step = repeat::delta(worldOf(g, p.source), worldOf(g, p.copy));
                const glm::dvec3 turn = rotateOf(g, p.copy) - rotateOf(g, p.source);
                NodeId prev = p.source, last = p.copy;
                for (int i = 0; i < count; ++i)
                {
                    NodeId c = copyTree(g, last, g.parent(last), p.mode, "", copies);
                    repeat::setWorldMatrix(g, c, step * worldOf(g, last), rotateOf(g, last) + turn);
                    made.push_back(c);
                    prev = last;
                    last = c;
                }
                recorded.push_back({prev, last, p.mode});
            }
            setLastPairs(std::move(recorded));
            g.select(made);
            return pathsOf(g, made);
        },
        .examples = {"duplicate(\"Fence\")\nmove(\"Fence1\", by=[120, 0, 0])\nduplicateWithTransform(count=8)"},
    });

    ops::registerCommand({
        .name = "instance",
        .doc = "Make instances of objects: meshes that share their source's mesh, so an edit to any of them changes all. "
               "Returns the new names.\n\nreference=True makes references instead, which share the base mesh under the "
               "source's modifiers but keep a modifier stack of their own (same base, a different bevel).",
        .positionalDoc = "objects",
        .flags = {{"name", "n", T::String, nullptr, "Name for a single instance."},
                  {"reference", "", T::Bool, false, "Share only the base mesh, with modifiers of its own."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<NodeId> roots = topLevel(g, a.objects(), "instance");
            if (roots.empty())
                throw CommandError("instance: nothing to instance");
            Request r;
            r.mode = a.get<bool>("reference", false) ? Mode::Reference : Mode::Instance;
            if (a.has("name") && roots.size() == 1)
                r.name = a.get<std::string>("name");
            std::vector<NodeId> made = duplicateRoots(g, roots, r);
            g.select(made);
            return pathsOf(g, made);
        },
        .examples = {"instance(\"Bolt\")", "instance(\"Panel\", reference=True)"},
    });
}
} // namespace lila::scene::duplicate
