#include "ui/Isolate.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <map>

namespace lila::ui::isolate
{
namespace
{
const Set* live(const Set* set) { return set && set->active() ? set : nullptr; }

/** Distinct per view and node: instances share one mesh but not their isolation. */
uint64_t maskKey(const Set& set, scene::NodeId id) { return set.version * 0x9E3779B97F4A7C15ull ^ uint64_t(id); }
} // namespace

bool shows(const Set* set, scene::NodeId id) { return !live(set) || set->shows(id); }

std::shared_ptr<const MaskedMesh> masked(const Set* set, scene::NodeId id, const scene::MeshPtr& drawn, int smooth, bool tangents, const SmoothCage* cageWire)
{
    if (!live(set) || !set->isPartial(id) || !drawn)
        return nullptr;
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(id);
    if (!n)
        return nullptr;
    scene::MeshPtr cage = scene::modifiers::cage(g, id);
    scene::MeshPtr base = smooth > 0 ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : drawn;
    if (!cage || !base || base->faceCapacity() != cage->faceCapacity())
        return nullptr;
    return maskedRenderMeshFor(drawn, maskKey(*set, id) + uint64_t(smooth), [&] {
        std::vector<uint8_t> faces = set->shownFaces(id, *cage);
        if (smooth <= 0)
            return faces;
        faces = scene::isolation::subdividedFaces(*base, smooth, faces, drawn->faceCapacity());
        // Smoothing the mapping can't follow shows everything rather than nothing.
        if (faces.empty())
            faces.assign(drawn->faceCapacity(), 1);
        return faces;
    }, tangents, cageWire);
}

std::shared_ptr<const scene::isolation::Visibility> visibility(const Set* set, scene::NodeId id, const scene::MeshPtr& cage)
{
    static const auto all = std::make_shared<const scene::isolation::Visibility>();
    if (!live(set) || !set->isPartial(id) || !cage)
        return all;
    struct Entry
    {
        std::weak_ptr<const geo::Mesh> source;
        std::shared_ptr<const scene::isolation::Visibility> vis;
    };
    static std::map<std::pair<const geo::Mesh*, uint64_t>, Entry> cache;
    auto key = std::make_pair(cage.get(), maskKey(*set, id));
    if (auto it = cache.find(key); it != cache.end() && it->second.source.lock() == cage)
        return it->second.vis;
    LILA_ZONE();
    if (cache.size() > 64)
        std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });
    auto vis = std::make_shared<const scene::isolation::Visibility>(scene::isolation::visibility(*cage, set->shownFaces(id, *cage)));
    cache[key] = {cage, vis};
    return vis;
}

std::optional<RayHit> raycast(const Set* set, scene::NodeId id, const scene::MeshPtr& mesh, const glm::dmat4& world, const glm::dvec3& origin,
                              const glm::dvec3& dir, int smooth)
{
    if (!shows(set, id))
        return std::nullopt;
    auto rm = renderMeshFor(mesh);
    if (!rm)
        return std::nullopt;
    auto m = masked(set, id, mesh, smooth);
    return ui::raycast(*rm, world, origin, dir, m ? &m->hiddenTriangles : nullptr);
}
} // namespace lila::ui::isolate
