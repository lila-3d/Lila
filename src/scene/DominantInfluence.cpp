#include "scene/DominantInfluence.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"

#include <unordered_map>

namespace lila::scene
{
using namespace rigutil;

namespace
{
struct CacheEntry
{
    DataPtr weights;
    MeshPtr mesh;
    std::shared_ptr<const rig::DominantInfluenceMap> map;
};

std::unordered_map<NodeId, CacheEntry>& cache()
{
    static std::unordered_map<NodeId, CacheEntry> c;
    return c;
}

const SkinWeights* weightsOf(const DataPtr& d) { return dynamic_cast<const SkinWeights*>(d.get()); }

std::shared_ptr<rig::DominantInfluenceMap> build(const geo::Mesh& m, const SkinWeights& w)
{
    LILA_ZONE();

    auto out = std::make_shared<rig::DominantInfluenceMap>();
    out->faceInfluence.assign(m.faceCapacity(), -1);
    std::unordered_map<uint32_t, float> sums;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        sums.clear();
        m.forEachFaceCorner(f, [&](geo::Index c) {
            geo::Index p = m.cornerPoint(c);
            if (p >= w.pointCount())
                return;
            for (uint32_t k = w.offsets[p]; k < w.offsets[p + 1]; ++k)
                sums[w.influences[k]] += w.weights[k];
        });
        float best = 0.0f;
        for (auto [inf, s] : sums)
            // Ties go to the lower id so the map is the same whatever the hash order.
            if (s > best || (s == best && s > 0.0f && int64_t(inf) < out->faceInfluence[f]))
            {
                best = s;
                out->faceInfluence[f] = inf;
            }
    }
    return out;
}

NodeId jointOf(Graph& g, NodeId skin, int64_t influence)
{
    if (influence < 0)
        return kNoNode;
    for (const skin::Influence& inf : skin::influences(g, skin))
        if (inf.index == uint64_t(influence))
            return inf.joint;
    return kNoNode;
}
} // namespace

namespace rig
{
std::shared_ptr<const DominantInfluenceMap> dominantInfluences(Graph& g, NodeId skin)
{
    if (!g.node(skin))
        return nullptr;
    DataPtr weights = std::get<DataPtr>(g.storedValue({skin, SkinClusterNode::kWeights, -1}));
    MeshPtr mesh = skin::inputMesh(g, skin);
    const SkinWeights* w = weightsOf(weights);
    if (!w || !mesh)
        return nullptr;
    CacheEntry& e = cache()[skin];
    // Weights and meshes are immutable once shared, so a new pointer is the only change there can be.
    if (!e.map || e.weights != weights || e.mesh != mesh)
        e = {weights, mesh, build(*mesh, *w)};
    return e.map;
}

NodeId dominantJoint(Graph& g, NodeId mesh, uint32_t face)
{
    NodeId skin = skin::find(g, mesh);
    auto map = skin != kNoNode ? dominantInfluences(g, skin) : nullptr;
    if (!map || face >= map->faceInfluence.size())
        return kNoNode;
    return jointOf(g, skin, map->faceInfluence[face]);
}

std::vector<uint32_t> dominatedFaces(Graph& g, NodeId mesh, NodeId joint)
{
    std::vector<uint32_t> out;
    NodeId skin = skin::find(g, mesh);
    auto map = skin != kNoNode ? dominantInfluences(g, skin) : nullptr;
    if (!map)
        return out;
    int64_t id = -1;
    for (const skin::Influence& inf : skin::influences(g, skin))
        if (inf.joint == joint)
            id = inf.index;
    if (id < 0)
        return out;
    for (uint32_t f = 0; f < map->faceInfluence.size(); ++f)
        if (map->faceInfluence[f] == id)
            out.push_back(f);
    return out;
}

std::vector<float> influenceWeights(Graph& g, NodeId mesh, NodeId joint)
{
    NodeId skin = skin::find(g, mesh);
    if (skin == kNoNode)
        return {};
    int64_t id = -1;
    for (const skin::Influence& inf : skin::influences(g, skin))
        if (inf.joint == joint)
            id = inf.index;
    const SkinWeights* w = weightsOf(std::get<DataPtr>(g.storedValue({skin, SkinClusterNode::kWeights, -1})));
    if (id < 0 || !w)
        return {};
    std::vector<float> out(w->pointCount(), 0.0f);
    for (uint32_t p = 0; p < w->pointCount(); ++p)
        for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
            if (w->influences[k] == uint32_t(id))
                out[p] = w->weights[k];
    return out;
}

void clearDominantInfluenceCache() { cache().clear(); }
} // namespace rig

void registerDominantInfluenceCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "dominantInfluence",
        .doc = "Query a skinned mesh's dominant influences: with face, the joint owning that face (null when "
               "unweighted); with joint, the faces it owns.",
        .positionalDoc = "mesh",
        .flags = {
            {"face", "f", T::Int, nullptr, "Face id."},
            {"joint", "j", T::String, nullptr, "Joint name."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<std::string> objs = a.objects();
            if (objs.size() != 1)
                throw CommandError("dominantInfluence: give one mesh");
            NodeId mesh = nodeNamed(objs[0], "dominantInfluence").id();
            if (skin::find(g, mesh) == kNoNode)
                throw CommandError("dominantInfluence: " + objs[0] + " has no skinCluster");
            g.evaluate();
            if (a.has("face"))
            {
                NodeId j = rig::dominantJoint(g, mesh, uint32_t(a.get<int64_t>("face")));
                return j == kNoNode ? json(nullptr) : json(g.node(j)->path());
            }
            if (a.has("joint"))
                return rig::dominatedFaces(g, mesh, nodeNamed(a.get<std::string>("joint"), "dominantInfluence").id());
            throw CommandError("dominantInfluence: pass face or joint");
        },
    });
}
} // namespace lila::scene
