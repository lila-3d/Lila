#include "ui/MeshCache.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Ops.h"
#include "geo/RenderMesh.h"
#include "ui/ViewportDisplay.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <map>
#include <unordered_map>

namespace lila::ui
{
namespace
{
struct Entry
{
    std::weak_ptr<const geo::Mesh> source;
    std::shared_ptr<const render::RenderMesh> built;
    std::shared_ptr<const geo::RenderMesh> pick;
    bool tangents = false;
};

std::unordered_map<const geo::Mesh*, Entry>& cache()
{
    static std::unordered_map<const geo::Mesh*, Entry> c;
    return c;
}

Entry build(const scene::MeshPtr& source, bool tangents)
{
    LILA_ZONE();

    geo::RenderMeshOptions opts;
    opts.maxUvSets = 2;
    opts.colors = true;
    opts.sections = true;
    opts.tangents = tangents;
    auto pick = std::make_shared<geo::RenderMesh>(geo::buildRenderMesh(*source, opts));
    const geo::RenderMesh& rm = *pick;

    auto out = std::make_shared<render::RenderMesh>();
    size_t n = rm.positions.size();
    out->vertices.resize(n);
    bool uvs = !rm.uvs.empty() && rm.uvs[0].size() == n;
    bool uvs1 = rm.uvs.size() > 1 && rm.uvs[1].size() == n;
    bool colors = rm.colors.size() == n;
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        render::MeshVertex& v = out->vertices[i];
        v.pos = rm.positions[i];
        v.normal = i < rm.normals.size() ? rm.normals[i] : glm::vec3(0, 1, 0);
        v.tangent = i < rm.tangents.size() ? rm.tangents[i] : glm::vec4(1, 0, 0, 1);
        v.uv = uvs ? rm.uvs[0][i] : glm::vec2(0.0f);
        v.uv1 = uvs1 ? rm.uvs[1][i] : v.uv;
        if (colors)
            v.color = rm.colors[i];
    });
    out->indices.assign(rm.indices.begin(), rm.indices.end());
    for (const geo::RenderSection& sec : rm.sections)
        out->sections.push_back({uint32_t(std::max(0, sec.material)), sec.firstIndex, sec.indexCount});
    out->edgePoints.resize(source->pointCapacity());
    tbb::parallel_for(geo::Index(0), source->pointCapacity(), [&](geo::Index p) {
        out->edgePoints[p] = source->pointValid(p) ? glm::vec3(source->position(p)) : glm::vec3(0.0f);
    });
    out->edgeMask.assign((rm.triangleSides.size() + 7) / 8, 0u);
    tbb::parallel_for(size_t(0), out->edgeMask.size(), [&](size_t w) {
        uint32_t word = 0;
        for (size_t t = w * 8; t < std::min(w * 8 + 8, rm.triangleSides.size()); ++t)
            word |= uint32_t(rm.triangleSides[t]) << ((t & 7) * 4);
        out->edgeMask[w] = word;
    });
    out->edgeIndices.resize(rm.lineIndices.size());
    tbb::parallel_for(size_t(0), rm.lineIndices.size(), [&](size_t i) { out->edgeIndices[i] = rm.vertexPoint[rm.lineIndices[i]]; });
    if (n)
    {
        out->boundsMin = out->boundsMax = rm.positions[0];
        for (const glm::vec3& p : rm.positions)
        {
            out->boundsMin = glm::min(out->boundsMin, p);
            out->boundsMax = glm::max(out->boundsMax, p);
        }
    }
    return {source, out, pick, tangents};
}

const Entry* entryFor(const scene::MeshPtr& mesh, bool tangents = false)
{
    if (!mesh)
        return nullptr;
    auto& c = cache();
    auto it = c.find(mesh.get());
    if (it != c.end() && it->second.source.lock() == mesh && (it->second.tangents || !tangents))
        return &it->second;
    // Snapshots die as the scene changes; drop their entries when the table gets busy.
    if (c.size() > 64)
        std::erase_if(c, [](const auto& kv) { return kv.second.source.expired(); });
    return &(c[mesh.get()] = build(mesh, tangents));
}
} // namespace

std::shared_ptr<const render::RenderMesh> renderMeshFor(const scene::MeshPtr& mesh, bool tangents)
{
    const Entry* e = entryFor(mesh, tangents);
    return e ? e->built : nullptr;
}

bool reuseRenderMesh(const scene::MeshPtr& from, const scene::MeshPtr& to, const std::function<void(std::vector<render::MeshVertex>&)>& edit)
{
    LILA_ZONE();

    auto& c = cache();
    auto it = c.find(from.get());
    if (!from || !to || it == c.end() || it->second.source.lock() != from)
        return false;
    Entry e = it->second;
    auto copy = std::make_shared<render::RenderMesh>(*e.built);
    copy->key = render::RenderMesh::nextKey();
    edit(copy->vertices);
    e.source = to;
    e.built = std::move(copy);
    c[to.get()] = std::move(e);
    return true;
}

std::shared_ptr<const MaskedMesh> maskedRenderMeshFor(const scene::MeshPtr& mesh, uint64_t maskKey, const std::function<std::vector<uint8_t>()>& shownFaces,
                                                      bool tangents, const SmoothCage* cageWire)
{
    LILA_ZONE();

    const Entry* full = entryFor(mesh, tangents);
    if (!full || !full->pick)
        return nullptr;
    struct Masked
    {
        std::weak_ptr<const geo::Mesh> source;
        std::shared_ptr<const render::RenderMesh> base;
        std::shared_ptr<const MaskedMesh> result;
    };
    static std::map<std::tuple<const geo::Mesh*, uint64_t, bool>, Masked> cache;
    auto key = std::make_tuple(mesh.get(), maskKey, cageWire != nullptr);
    if (auto it = cache.find(key); it != cache.end() && it->second.source.lock() == mesh && it->second.base == full->built)
        return it->second.result;
    if (cache.size() > 64)
        std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });

    const geo::RenderMesh& pd = *full->pick;
    const render::RenderMesh& rm = *full->built;
    const std::vector<uint8_t> faces = shownFaces ? shownFaces() : std::vector<uint8_t>();
    auto shown = [&](geo::Index f) { return !shownFaces || (f < faces.size() && faces[f]); };
    auto out = std::make_shared<MaskedMesh>();
    auto sub = std::make_shared<render::RenderMesh>();
    sub->shareVertices = full->built;
    sub->boundsMin = rm.boundsMin;
    sub->boundsMax = rm.boundsMax;
    const size_t tris = rm.indices.size() / 3;
    out->hiddenTriangles.assign(tris, 0);
    for (size_t t = 0; t < tris; ++t)
        out->hiddenTriangles[t] = t < pd.triangleFaces.size() && !shown(pd.triangleFaces[t]);
    std::vector<render::MeshSection> sections = rm.sections;
    if (sections.empty())
        sections.push_back({0, 0, uint32_t(rm.indices.size())});
    for (const render::MeshSection& sec : sections)
    {
        render::MeshSection kept{sec.material, uint32_t(sub->indices.size()), 0};
        for (uint32_t i = sec.firstIndex; i + 2 < sec.firstIndex + sec.indexCount && i + 2 < rm.indices.size(); i += 3)
            if (!out->hiddenTriangles[i / 3])
                sub->indices.insert(sub->indices.end(), rm.indices.begin() + i, rm.indices.begin() + i + 3);
        kept.indexCount = uint32_t(sub->indices.size()) - kept.firstIndex;
        if (kept.indexCount)
            sub->sections.push_back(kept);
    }
    // A wire edge shows while any face on it does.
    std::vector<uint8_t> edgeShown;
    if (!pd.lineEdges.empty())
    {
        edgeShown.assign(mesh->edgeCapacity(), 0);
        for (geo::Index f = 0; f < mesh->faceCapacity(); ++f)
            if (shown(f) && mesh->faceValid(f))
                mesh->forEachFaceCorner(f, [&](geo::Index c) { edgeShown[mesh->cornerEdge(c)] = 1; });
    }
    for (size_t l = 0; l < pd.lineEdges.size() && 2 * l + 1 < rm.edgeVertexCount(); ++l)
        if (pd.lineEdges[l] < edgeShown.size() && edgeShown[pd.lineEdges[l]] &&
            (!cageWire || (pd.lineEdges[l] < cageWire->map.edges.size() && cageWire->map.edges[pd.lineEdges[l]] != geo::kInvalid)))
        {
            sub->edgePoints.push_back(rm.edgePoint(2 * l));
            sub->edgePoints.push_back(rm.edgePoint(2 * l + 1));
        }
    out->mesh = std::move(sub);
    cache[key] = {mesh, full->built, out};
    return out;
}

scene::MeshPtr smoothedFor(const scene::MeshPtr& mesh, int level)
{
    if (!mesh || level <= 0)
        return mesh;
    auto smooth = smoothCageFor(mesh, level);
    return smooth ? smooth->mesh : mesh;
}

namespace
{
/** Groups items by key (one per item, kInvalid skipped) into offsets and ids, keys in [0, keys). */
void invert(std::span<const geo::Index> keyOf, size_t keys, std::vector<geo::Index>& offsets, std::vector<geo::Index>& ids)
{
    offsets.assign(keys + 1, 0);
    for (geo::Index k : keyOf)
        if (k < keys)
            ++offsets[k + 1];
    for (size_t k = 0; k < keys; ++k)
        offsets[k + 1] += offsets[k];
    ids.resize(offsets[keys]);
    std::vector<geo::Index> fill(offsets.begin(), offsets.end() - 1);
    for (geo::Index i = 0; i < keyOf.size(); ++i)
        if (keyOf[i] < keys)
            ids[fill[keyOf[i]]++] = i;
}
} // namespace

std::shared_ptr<const SmoothCage> smoothCageFor(const scene::MeshPtr& mesh, int level)
{
    if (!mesh || level <= 0)
        return nullptr;
    struct Smoothed
    {
        std::weak_ptr<const geo::Mesh> source;
        int level = 0;
        bool keepHardEdges = false;
        std::shared_ptr<const SmoothCage> result;
    };
    static std::unordered_map<const geo::Mesh*, Smoothed> cache;
    const bool keepHardEdges = PrefStore::get().value<bool>("general", viewportDisplay::kSmoothHardEdges, false);
    auto it = cache.find(mesh.get());
    if (it != cache.end() && it->second.level == level && it->second.keepHardEdges == keepHardEdges && it->second.source.lock() == mesh)
        return it->second.result;
    LILA_ZONE();
    if (cache.size() > 64)
        std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });
    geo::SubdivideOptions o;
    o.levels = level;
    o.keepHardEdges = keepHardEdges;
    auto out = std::make_shared<SmoothCage>();
    auto smoothed = std::make_shared<const geo::Mesh>(geo::subdivide(*mesh, o, &out->map));
    out->cagePoints.assign(smoothed->pointCapacity(), geo::kInvalid);
    for (geo::Index p = 0; p < out->map.points.size(); ++p)
        if (out->map.points[p] < out->cagePoints.size())
            out->cagePoints[out->map.points[p]] = p;
    invert(out->map.edges, mesh->edgeCapacity(), out->edgeOffsets, out->edgeSegments);
    invert(out->map.faces, mesh->faceCapacity(), out->faceOffsets, out->faceParts);
    out->mesh = std::move(smoothed);
    cache[mesh.get()] = {mesh, level, keepHardEdges, out};
    return out;
}

std::shared_ptr<const render::RenderMesh> cageWireMeshFor(const SmoothCage& smooth, bool tangents)
{
    auto masked = maskedRenderMeshFor(smooth.mesh, 0, {}, tangents, &smooth);
    return masked ? masked->mesh : renderMeshFor(smooth.mesh, tangents);
}

std::shared_ptr<const geo::RenderMesh> pickDataFor(const scene::MeshPtr& mesh)
{
    const Entry* e = entryFor(mesh);
    return e ? e->pick : nullptr;
}

std::optional<RayHit> raycast(const render::RenderMesh& mesh, const glm::dmat4& world, const glm::dvec3& origin, const glm::dvec3& dir,
                              const std::vector<uint8_t>* hidden)
{
    LILA_ZONE();

    glm::dmat4 inv = glm::inverse(world);
    glm::dvec3 o = glm::dvec3(inv * glm::dvec4(origin, 1.0));
    glm::dvec3 d = glm::dvec3(inv * glm::dvec4(dir, 0.0));

    // Slab test against the bounds first; most meshes miss.
    glm::dvec3 lo(mesh.boundsMin), hi(mesh.boundsMax);
    double t0 = 0.0, t1 = 1e300;
    for (int a = 0; a < 3; ++a)
    {
        if (std::abs(d[a]) < 1e-300)
        {
            if (o[a] < lo[a] || o[a] > hi[a])
                return std::nullopt;
            continue;
        }
        double ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
        if (ta > tb)
            std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1)
            return std::nullopt;
    }

    using Best = std::pair<double, size_t>; // distance along the local ray, triangle
    size_t tris = mesh.indices.size() / 3;
    Best best = tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, tris, 4096), Best{1e300, 0},
        [&](const tbb::blocked_range<size_t>& r, Best acc) {
            for (size_t t = r.begin(); t != r.end(); ++t)
            {
                if (hidden && t < hidden->size() && (*hidden)[t])
                    continue;
                // Moller-Trumbore, both sides.
                glm::dvec3 a(mesh.vertices[mesh.indices[t * 3]].pos), b(mesh.vertices[mesh.indices[t * 3 + 1]].pos),
                    c(mesh.vertices[mesh.indices[t * 3 + 2]].pos);
                glm::dvec3 e1 = b - a, e2 = c - a, p = glm::cross(d, e2);
                double det = glm::dot(e1, p);
                if (std::abs(det) < 1e-18)
                    continue;
                double invDet = 1.0 / det;
                glm::dvec3 s = o - a;
                double u = glm::dot(s, p) * invDet;
                if (u < 0.0 || u > 1.0)
                    continue;
                glm::dvec3 q = glm::cross(s, e1);
                double v = glm::dot(d, q) * invDet;
                if (v < 0.0 || u + v > 1.0)
                    continue;
                double dist = glm::dot(e2, q) * invDet;
                if (dist > 0.0 && dist < acc.first)
                    acc = {dist, t};
            }
            return acc;
        },
        [](Best x, Best y) { return x.first <= y.first ? x : y; });
    if (best.first >= 1e300)
        return std::nullopt;
    // Distance is in the local ray's units; convert to world.
    return RayHit{glm::length(glm::dvec3(world * glm::dvec4(d * best.first, 0.0))), uint32_t(best.second)};
}
} // namespace lila::ui
