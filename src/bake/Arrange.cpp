#include "bake/Arrange.h"

#include "bake/BakeNodes.h"
#include "core/Profile.h"
#include "scene/Graph.h"

#include <string>
#include <unordered_map>

namespace lila::bake::arrange
{
namespace
{
using scene::Graph;
using scene::Node;
using scene::NodeId;

struct Group
{
    std::string path;
    glm::dvec3 offset{0.0};
};

struct Project
{
    std::string path;
    std::map<NodeId, Group> groups;
};

struct State
{
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    std::map<NodeId, Project> projects;
    /** Mesh offsets, rebuilt when the graph changes. */
    std::unordered_map<NodeId, glm::dvec3> meshes;
    const Graph* graph = nullptr;
    uint64_t version = 0;
    bool stale = true;
};

State& state()
{
    static State s;
    return s;
}

Node* findNode(Graph& g, const std::string& name)
{
    try
    {
        return g.find(name);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

/** Ids are reused after a new scene or a delete, so a node only counts while its path still matches. */
Node* same(Graph& g, NodeId id, const std::string& path)
{
    Node* n = g.node(id);
    return n && n->path() == path ? n : nullptr;
}

void changed()
{
    state().stale = true;
    // A copy: a listener may remove itself.
    const std::map<int, std::function<void()>> fns = state().listeners;
    for (const auto& [id, fn] : fns)
        fn();
}

void rebuild(Graph& g)
{
    LILA_ZONE();

    State& s = state();
    s.meshes.clear();
    s.graph = &g;
    s.version = g.version();
    s.stale = false;
    for (const auto& [pid, p] : s.projects)
    {
        Node* pn = same(g, pid, p.path);
        if (!pn || !isProject(*pn))
            continue;
        for (const auto& [gid, grp] : p.groups)
        {
            Node* gn = same(g, gid, grp.path);
            if (!gn || !isGroup(*gn))
                continue;
            GroupSettings gs;
            try
            {
                gs = groupSettings(*gn);
            }
            catch (const std::exception&)
            {
                continue;
            }
            auto add = [&](const std::string& name) {
                if (Node* n = name.empty() ? nullptr : findNode(g, name))
                    s.meshes.try_emplace(n->id(), grp.offset);
            };
            for (const std::string& l : gs.lows)
                add(l);
            for (const HighEntry& h : gs.highs)
                if (!h.self && h.file.empty())
                    add(h.node);
            add(gs.cage);
        }
    }
}
} // namespace

void set(Graph& g, NodeId project, const std::map<NodeId, glm::dvec3>& offsets)
{
    Node* p = g.node(project);
    if (!p)
        return;
    Project entry{.path = p->path()};
    for (const auto& [gid, o] : offsets)
        if (Node* gn = g.node(gid))
            entry.groups[gid] = {gn->path(), o};
    state().projects[project] = std::move(entry);
    changed();
}

void clear(NodeId project)
{
    if (state().projects.erase(project))
        changed();
}

void clearAll()
{
    if (state().projects.empty())
        return;
    state().projects.clear();
    changed();
}

bool active(Graph& g, NodeId project)
{
    auto it = state().projects.find(project);
    return it != state().projects.end() && same(g, project, it->second.path);
}

std::map<NodeId, glm::dvec3> offsets(Graph& g, NodeId project)
{
    std::map<NodeId, glm::dvec3> out;
    if (!active(g, project))
        return out;
    for (const auto& [gid, grp] : state().projects[project].groups)
        if (same(g, gid, grp.path))
            out[gid] = grp.offset;
    return out;
}

glm::dvec3 displayOffset(Graph& g, NodeId node)
{
    State& s = state();
    if (s.projects.empty())
        return glm::dvec3(0.0);
    if (s.stale || s.graph != &g || s.version != g.version())
        rebuild(g);
    auto it = s.meshes.find(node);
    return it == s.meshes.end() ? glm::dvec3(0.0) : it->second;
}

glm::dmat4 displayWorld(Graph& g, NodeId node, const glm::dmat4& world)
{
    const glm::dvec3 o = displayOffset(g, node);
    if (o == glm::dvec3(0.0))
        return world;
    glm::dmat4 w = world;
    w[3] += glm::dvec4(o, 0.0);
    return w;
}

int addListener(std::function<void()> fn)
{
    const int id = state().nextListener++;
    state().listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    state().listeners.erase(id);
}
} // namespace lila::bake::arrange
