#include "geo/Normals.h"

#include "geo/Triangulate.h"
#include "third_party/mikktspace/mikktspace.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <cmath>
#include <numeric>

namespace lila::geo
{
namespace
{
constexpr Index kGrain = 4096;

std::vector<glm::dvec3> faceAreaVectors(const Mesh& mesh)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(mesh.faceCapacity(), glm::dvec3(0.0));
    tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.faceCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
            if (mesh.faceValid(f))
                out[f] = mesh.faceAreaVector(f);
    });
    return out;
}

glm::dvec3 cornerContribution(const Mesh& mesh, Index c, const glm::dvec3& areaVec, NormalWeight w)
{
    switch (w)
    {
    case NormalWeight::Uniform: return glm::normalize(areaVec);
    case NormalWeight::Area: return areaVec;
    case NormalWeight::Angle: return glm::normalize(areaVec) * cornerAngle(mesh, c);
    case NormalWeight::AreaAngle: return areaVec * cornerAngle(mesh, c);
    }
    return areaVec;
}

glm::dvec3 safeNormalize(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double len = glm::length(v);
    return len > 1e-300 ? v / len : fallback;
}
} // namespace

double cornerAngle(const Mesh& mesh, Index c)
{
    const glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
    const glm::dvec3 a = mesh.position(mesh.cornerPoint(mesh.cornerPrev(c))) - p;
    const glm::dvec3 b = mesh.position(mesh.cornerNextPoint(c)) - p;
    const double la = glm::length(a), lb = glm::length(b);
    if (la <= 0.0 || lb <= 0.0)
        return 0.0;
    // atan2 stays accurate for angles near 0 and pi.
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

std::vector<glm::dvec3> faceNormals(const Mesh& mesh)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out = faceAreaVectors(mesh);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, out.size(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t f = r.begin(); f < r.end(); ++f)
            out[f] = safeNormalize(out[f], glm::dvec3(0.0, 1.0, 0.0));
    });
    return out;
}

std::vector<glm::vec3> pointNormals(const Mesh& mesh, NormalWeight weight)
{
    LILA_ZONE();

    const std::vector<glm::dvec3> area = faceAreaVectors(mesh);
    std::vector<glm::vec3> out(mesh.pointCapacity(), glm::vec3(0.0f));
    tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.pointCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!mesh.pointValid(p))
                continue;
            glm::dvec3 sum(0.0);
            mesh.forEachPointCorner(p, [&](Index c) {
                sum += cornerContribution(mesh, c, area[mesh.cornerFace(c)], weight);
            });
            out[p] = glm::vec3(safeNormalize(sum, glm::dvec3(0.0, 1.0, 0.0)));
        }
    });
    return out;
}

std::vector<glm::vec3> cornerNormals(const Mesh& mesh, const CornerNormalOptions& opts)
{
    LILA_ZONE();

    const std::vector<glm::dvec3> area = faceAreaVectors(mesh);
    std::span<const bool> hard = opts.useHardEdges ? mesh.edgeAttrs().get<bool>(attr::kHard) : std::span<const bool>{};
    std::vector<glm::vec3> out(mesh.cornerCapacity(), glm::vec3(0.0f));

    struct Scratch
    {
        std::vector<Index> corners;
        std::vector<Index> parent;
        std::vector<glm::dvec3> sum;
    };
    tbb::enumerable_thread_specific<Scratch> scratch;
    tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.pointCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        Scratch& s = scratch.local();
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!mesh.pointValid(p))
                continue;
            s.corners.clear();
            mesh.forEachPointCorner(p, [&](Index c) { s.corners.push_back(c); });
            const size_t k = s.corners.size();
            if (k == 0)
                continue;
            s.parent.resize(k);
            std::iota(s.parent.begin(), s.parent.end(), Index(0));
            auto find = [&](Index i) {
                while (s.parent[i] != i)
                    i = s.parent[i] = s.parent[s.parent[i]];
                return i;
            };
            auto local = [&](Index c) {
                for (size_t i = 0; i < k; ++i)
                    if (s.corners[i] == c)
                        return Index(i);
                return kInvalid;
            };

            // Corners on either side of a soft manifold edge share a fan.
            mesh.forEachPointEdge(p, [&](Index e) {
                if (!hard.empty() && hard[e])
                    return;
                if (!mesh.isManifoldEdge(e))
                    return;
                const Index r1 = mesh.edgeCorner(e);
                const Index r2 = mesh.cornerRadial(r1);
                const Index a = local(mesh.cornerPoint(r1) == p ? r1 : mesh.cornerNext(r1));
                const Index b = local(mesh.cornerPoint(r2) == p ? r2 : mesh.cornerNext(r2));
                if (a != kInvalid && b != kInvalid)
                    s.parent[find(a)] = find(b);
            });

            s.sum.assign(k, glm::dvec3(0.0));
            for (size_t i = 0; i < k; ++i)
            {
                const Index c = s.corners[i];
                s.sum[find(Index(i))] += cornerContribution(mesh, c, area[mesh.cornerFace(c)], opts.weight);
            }
            for (size_t i = 0; i < k; ++i)
            {
                const Index c = s.corners[i];
                const glm::dvec3 fallback = safeNormalize(area[mesh.cornerFace(c)], glm::dvec3(0.0, 1.0, 0.0));
                out[c] = glm::vec3(safeNormalize(s.sum[find(Index(i))], fallback));
            }
        }
    });
    return out;
}

void updateCornerNormals(Mesh& mesh, const CornerNormalOptions& opts)
{
    LILA_ZONE();

    const std::vector<glm::vec3> n = cornerNormals(mesh, opts);
    std::span<glm::vec3> dst = mesh.cornerAttrs().getOrAdd<glm::vec3>(attr::kNormal);
    std::copy(n.begin(), n.end(), dst.begin());
}

void setHardEdgesByAngle(Mesh& mesh, double degrees, std::span<const Index> edges)
{
    LILA_ZONE();

    const double cosLimit = std::cos(glm::radians(degrees));
    std::span<bool> hard = mesh.edgeAttrs().getOrAdd<bool>(attr::kHard);
    auto apply = [&](Index e) {
        if (!mesh.edgeValid(e))
            return;
        if (!mesh.isManifoldEdge(e))
        {
            hard[e] = false;
            return;
        }
        const Index c = mesh.edgeCorner(e);
        const glm::dvec3 a = mesh.faceNormal(mesh.cornerFace(c));
        const glm::dvec3 b = mesh.faceNormal(mesh.cornerFace(mesh.cornerRadial(c)));
        hard[e] = glm::dot(a, b) < cosLimit - 1e-12;
    };
    if (edges.empty())
        tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.edgeCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
            for (Index e = r.begin(); e < r.end(); ++e)
                apply(e);
        });
    else
        for (Index e : edges)
            apply(e);
}

void setSoftEdges(Mesh& mesh, std::span<const Index> edges)
{
    std::span<bool> hard = mesh.edgeAttrs().getOrAdd<bool>(attr::kHard);
    if (edges.empty())
        std::fill(hard.begin(), hard.end(), false);
    for (Index e : edges)
        if (e < hard.size())
            hard[e] = false;
}

namespace
{
struct MikkData
{
    const Mesh* mesh;
    const Triangulation* tris;
    std::span<const glm::vec3> normals;
    std::span<const glm::vec2> uv;
    std::vector<glm::vec4>* out;

    Index corner(int face, int vert) const { return tris->corners[size_t(face) * 3 + vert]; }
};

const MikkData& data(const SMikkTSpaceContext* ctx)
{
    return *static_cast<const MikkData*>(ctx->m_pUserData);
}

int mikkNumFaces(const SMikkTSpaceContext* ctx)
{
    return int(data(ctx).tris->triangleCount());
}

int mikkNumVerts(const SMikkTSpaceContext*, const int)
{
    return 3;
}

void mikkPosition(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert)
{
    const MikkData& d = data(ctx);
    const glm::dvec3& p = d.mesh->position(d.mesh->cornerPoint(d.corner(face, vert)));
    out[0] = float(p.x);
    out[1] = float(p.y);
    out[2] = float(p.z);
}

void mikkNormal(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert)
{
    const MikkData& d = data(ctx);
    const glm::vec3& n = d.normals[d.corner(face, vert)];
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void mikkTexCoord(const SMikkTSpaceContext* ctx, float out[], const int face, const int vert)
{
    const MikkData& d = data(ctx);
    const glm::vec2& t = d.uv[d.corner(face, vert)];
    out[0] = t.x;
    out[1] = t.y;
}

void mikkSetBasic(const SMikkTSpaceContext* ctx, const float t[], const float sign, const int face, const int vert)
{
    const MikkData& d = data(ctx);
    (*d.out)[d.corner(face, vert)] = glm::vec4(t[0], t[1], t[2], sign);
}
/**
 * Groups triangles into batches of whole point-connected parts, each batch at least minTriangles
 * unless it is the last. One batch when everything is connected.
 */
std::vector<Triangulation> splitByPoints(const Mesh& mesh, const Triangulation& tris, Index minTriangles)
{
    LILA_ZONE();

    const Index n = tris.triangleCount();
    if (n < 2 * minTriangles)
        return {};
    std::vector<Index> parent(mesh.pointCapacity());
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index i) {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    };
    for (Index t = 0; t < n; ++t)
    {
        const Index a = find(mesh.cornerPoint(tris.corners[size_t(t) * 3]));
        for (int k = 1; k < 3; ++k)
        {
            const Index b = find(mesh.cornerPoint(tris.corners[size_t(t) * 3 + k]));
            if (a != b)
                parent[std::max(a, b)] = std::min(a, b);
        }
    }
    // Triangles per root, then roots packed into batches in id order.
    std::vector<Index> rootTris(mesh.pointCapacity(), 0);
    std::vector<Index> triRoot(n);
    for (Index t = 0; t < n; ++t)
    {
        triRoot[t] = find(mesh.cornerPoint(tris.corners[size_t(t) * 3]));
        ++rootTris[triRoot[t]];
    }
    std::vector<Index> batchOf(mesh.pointCapacity(), kInvalid);
    Index batches = 0, fill = 0;
    for (Index r = 0; r < mesh.pointCapacity(); ++r)
    {
        if (!rootTris[r])
            continue;
        if (fill >= minTriangles)
        {
            ++batches;
            fill = 0;
        }
        batchOf[r] = batches;
        fill += rootTris[r];
    }
    if (batches == 0)
        return {};
    std::vector<Triangulation> out(size_t(batches) + 1);
    for (Index t = 0; t < n; ++t)
    {
        Triangulation& part = out[batchOf[triRoot[t]]];
        part.corners.insert(part.corners.end(), tris.corners.begin() + size_t(t) * 3, tris.corners.begin() + size_t(t) * 3 + 3);
        part.faces.push_back(tris.faces[t]);
    }
    return out;
}
} // namespace

std::vector<glm::vec4> cornerTangents(const Mesh& mesh, const Triangulation& tris, std::span<const glm::vec3> normals,
                                      std::string_view uvSet, bool parallel)
{
    LILA_ZONE();

    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    if (uv.empty() || normals.size() < mesh.cornerCapacity())
        return {};
    std::vector<glm::vec4> out(mesh.cornerCapacity(), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = mikkNumFaces;
    iface.m_getNumVerticesOfFace = mikkNumVerts;
    iface.m_getPosition = mikkPosition;
    iface.m_getNormal = mikkNormal;
    iface.m_getTexCoord = mikkTexCoord;
    iface.m_setTSpaceBasic = mikkSetBasic;
    auto run = [&](const Triangulation& part) {
        MikkData d{&mesh, &part, normals, uv, &out};
        SMikkTSpaceContext ctx{&iface, &d};
        genTangSpaceDefault(&ctx);
    };

    // MikkTSpace only relates triangles through shared vertices, so parts sharing no point can run
    // in parallel with the same result.
    const std::vector<Triangulation> parts = parallel ? splitByPoints(mesh, tris, 1u << 16) : std::vector<Triangulation>{};
    if (parts.size() <= 1)
        run(tris);
    else
        tbb::parallel_for(size_t(0), parts.size(), [&](size_t i) { run(parts[i]); });
    return out;
}

bool updateTangents(Mesh& mesh, const TangentOptions& opts)
{
    LILA_ZONE();

    if (!mesh.cornerAttrs().has(opts.uvSet))
        return false;
    std::vector<glm::vec3> computed;
    std::span<const glm::vec3> normals = std::as_const(mesh).cornerAttrs().get<glm::vec3>(attr::kNormal);
    if (!opts.useStoredNormals || normals.size() != mesh.cornerCapacity())
    {
        computed = cornerNormals(mesh);
        normals = computed;
    }
    const Triangulation tris = triangulate(mesh);
    const std::vector<glm::vec4> t = cornerTangents(mesh, tris, normals, opts.uvSet);
    if (t.empty())
        return false;
    std::span<glm::vec4> dst = mesh.cornerAttrs().getOrAdd<glm::vec4>(attr::kTangent);
    std::copy(t.begin(), t.end(), dst.begin());
    return true;
}
} // namespace lila::geo
