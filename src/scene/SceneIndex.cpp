#include "scene/SceneIndex.h"

#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/ArrayGroup.h"
#include "scene/DisplayLayers.h"
#include "scene/Transform.h"

#include <map>
#include <string>
#include <unordered_map>

namespace lila::scene::sceneIndex
{
namespace
{
struct TypeLists
{
    uint64_t structure = 0;
    std::unordered_map<std::string, std::vector<Node*>> lists;
};

std::unordered_map<const Graph*, TypeLists>& typeLists()
{
    static std::unordered_map<const Graph*, TypeLists> m;
    return m;
}

const std::vector<Node*>& cachedList(const Graph& g, std::string_view key, const std::function<bool(const Node&)>& keep)
{
    TypeLists& t = typeLists()[&g];
    if (t.structure != g.structure())
    {
        t.lists.clear();
        t.structure = g.structure();
    }
    auto [it, added] = t.lists.try_emplace(std::string(key));
    if (added)
    {
        LILA_ZONE();
        for (NodeId id : g.nodes())
            if (Node* n = g.node(id); n && keep(*n))
                it->second.push_back(n);
    }
    return it->second;
}

bool hiddenHere(const Graph& g, const Node& n)
{
    return n.type().dag && (!n.as<bool>(Transform::kVisibility) || displayLayers::hiddenByOwnLayer(g, n.id()));
}
} // namespace

const std::vector<Node*>& ofType(const Graph& g, std::string_view typeName)
{
    return cachedList(g, std::string("type:").append(typeName), [typeName](const Node& n) { return n.type().isA(typeName); });
}

const std::vector<Node*>& matching(const Graph& g, std::string_view key, const std::function<bool(const Node&)>& keep)
{
    return cachedList(g, std::string("key:").append(key), keep);
}

uint64_t passStamp(const Graph& g, std::string_view typeName)
{
    uint64_t h = g.structure() * 0x9E3779B97F4A7C15ull;
    for (const Node* n : ofType(g, typeName))
        h = (h ^ (n->evaluatedPass() + n->id() * 0xC2B2AE3D27D4EB4Full)) * 0x100000001B3ull;
    return h;
}

namespace
{
uint64_t passesOf(const Graph& g, const std::vector<NodeId>& ids)
{
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (NodeId id : ids)
        if (const Node* n = g.node(id))
            h = (h ^ (n->evaluatedPass() + id * 0xC2B2AE3D27D4EB4Full)) * 0x100000001B3ull;
    return h;
}
} // namespace

const Hierarchy& hierarchy(const Graph& g, bool inherit)
{
    struct Cached
    {
        uint64_t structure = 0, visibility = 0, driven = 0;
        bool valid = false;
        int playing = 0;
        // Nodes whose hiding can change by evaluation alone, without an edit: driven visibility, display layers.
        std::vector<NodeId> evaluated;
        Hierarchy h;
    };
    // Views and the light fit may ask with different inherit settings in one frame; each keeps its own walk.
    static std::map<std::pair<const Graph*, bool>, Cached> cache;
    Cached& c = cache[{&g, inherit}];
    const int playing = anim::playDirection();
    if (c.valid && c.structure == g.structure() && c.visibility == g.visibilityVersion() && c.playing == playing &&
        c.driven == passesOf(g, c.evaluated))
        return c.h;
    LILA_ZONE();
    c.valid = true;
    c.structure = g.structure();
    c.visibility = g.visibilityVersion();
    c.playing = playing;
    c.evaluated.clear();

    const std::vector<NodeId> ids = g.nodes();
    const size_t size = ids.empty() ? 0 : size_t(ids.back()) + 1;
    Hierarchy& h = c.h;
    h.shown.assign(size, 1);
    h.arrays.clear();
    std::vector<NodeId> arrays(size, kNoNode);
    bool anyArray = false;
    // Parents before children; nodes outside the hierarchy are roots of their own.
    std::vector<std::pair<NodeId, NodeId>> stack; // node, parent
    for (NodeId r : g.roots())
        stack.emplace_back(r, kNoNode);
    std::vector<uint8_t> seen(size, 0);
    auto visit = [&](NodeId id, NodeId parent) {
        const Node* n = g.node(id);
        if (!n || id >= size)
            return false;
        seen[id] = 1;
        bool shown = !hiddenHere(g, *n);
        if (inherit && parent != kNoNode)
            shown = shown && h.shown[parent];
        h.shown[id] = shown;
        if (parent != kNoNode)
        {
            const Node* p = g.node(parent);
            arrays[id] = p && arrays::isArray(*p) ? parent : arrays[parent];
            anyArray = anyArray || arrays[id] != kNoNode;
        }
        return true;
    };
    while (!stack.empty())
    {
        auto [id, parent] = stack.back();
        stack.pop_back();
        if (!visit(id, parent))
            continue;
        for (NodeId child : g.children(id))
            stack.emplace_back(child, id);
    }
    for (NodeId id : ids)
    {
        if (!seen[id])
            h.shown[id] = effectivelyVisible(g, id, inherit);
        if (const Node* n = g.node(id);
            n && (displayLayers::isLayer(*n) || (n->type().dag && g.source({id, Transform::kVisibility, -1}))))
            c.evaluated.push_back(id);
    }
    c.driven = passesOf(g, c.evaluated);
    if (anyArray)
        h.arrays = std::move(arrays);
    return h;
}
} // namespace lila::scene::sceneIndex
