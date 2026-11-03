#include "geo/Normals.h"

#include "geo/Triangulate.h"
#include "third_party/mikktspace/mikktspace.h"

#include "core/Profile.h"
#include "core/UninitVector.h"

#include <tbb/blocked_range.h>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_scan.h>
#include <tbb/parallel_sort.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
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

/** A corner's share of its fan normal; angle() is the corner's angle, called only when the weight needs it. */
template <class Angle> glm::dvec3 contribution(const glm::dvec3& areaVec, NormalWeight w, Angle&& angle)
{
    switch (w)
    {
    case NormalWeight::Uniform: return glm::normalize(areaVec);
    case NormalWeight::Area: return areaVec;
    case NormalWeight::Angle: return glm::normalize(areaVec) * angle();
    case NormalWeight::AreaAngle: return areaVec * angle();
    }
    return areaVec;
}

glm::dvec3 cornerContribution(const Mesh& mesh, Index c, const glm::dvec3& areaVec, NormalWeight w)
{
    return contribution(areaVec, w, [&] { return cornerAngle(mesh, c); });
}

double angleAt(const glm::dvec3& p, const glm::dvec3& prev, const glm::dvec3& next)
{
    const glm::dvec3 a = prev - p;
    const glm::dvec3 b = next - p;
    const double la = glm::length(a), lb = glm::length(b);
    if (la <= 0.0 || lb <= 0.0)
        return 0.0;
    // atan2 stays accurate for angles near 0 and pi.
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}

/** The corners around one point and the smooth fans they fall into. */
struct FanScratch
{
    std::vector<Index> corners;
    std::vector<Index> parent;
    std::vector<glm::dvec3> sum;
    std::vector<uint32_t> ordinal;

    /** The fan of corners[i], as the index of one corner in it. */
    Index find(Index i)
    {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    }
};

/** Fills s with p's corners grouped into fans: corners on either side of a soft manifold edge share one. */
void groupFans(const Mesh& mesh, Index p, std::span<const bool> hard, FanScratch& s)
{
    s.corners.clear();
    mesh.forEachPointCorner(p, [&](Index c) { s.corners.push_back(c); });
    const size_t k = s.corners.size();
    s.parent.resize(k);
    std::iota(s.parent.begin(), s.parent.end(), Index(0));
    if (k < 2)
        return;
    auto local = [&](Index c) {
        for (size_t i = 0; i < k; ++i)
            if (s.corners[i] == c)
                return Index(i);
        return kInvalid;
    };
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
            s.parent[s.find(a)] = s.find(b);
    });
}

std::span<const bool> hardEdges(const Mesh& mesh, const CornerNormalOptions& opts)
{
    return opts.useHardEdges ? mesh.edgeAttrs().get<bool>(attr::kHard) : std::span<const bool>{};
}

/** Exclusive prefix sum of v in place; returns the total. */
template <class Vec> uint32_t scanCounts(Vec& v)
{
    return tbb::parallel_scan(
        tbb::blocked_range<size_t>(0, v.size(), 1 << 16), uint32_t(0),
        [&](const tbb::blocked_range<size_t>& r, uint32_t sum, bool final) {
            for (size_t i = r.begin(); i < r.end(); ++i)
            {
                const uint32_t x = v[i];
                if (final)
                    v[i] = sum;
                sum += x;
            }
            return sum;
        },
        std::plus<uint32_t>());
}

glm::dvec3 normalizeOr(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double len = glm::length(v);
    return len > 1e-300 ? v / len : fallback;
}
} // namespace

double cornerAngle(const Mesh& mesh, Index c)
{
    return angleAt(mesh.position(mesh.cornerPoint(c)), mesh.position(mesh.cornerPoint(mesh.cornerPrev(c))),
                   mesh.position(mesh.cornerNextPoint(c)));
}

std::vector<glm::dvec3> faceNormals(const Mesh& mesh)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out = faceAreaVectors(mesh);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, out.size(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t f = r.begin(); f < r.end(); ++f)
            out[f] = normalizeOr(out[f], glm::dvec3(0.0, 1.0, 0.0));
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
            out[p] = glm::vec3(normalizeOr(sum, glm::dvec3(0.0, 1.0, 0.0)));
        }
    });
    return out;
}

std::vector<glm::vec3> cornerNormals(const Mesh& mesh, const CornerNormalOptions& opts)
{
    LILA_ZONE();

    const std::vector<glm::dvec3> area = faceAreaVectors(mesh);
    const std::span<const bool> hard = hardEdges(mesh, opts);
    std::vector<glm::vec3> out(mesh.cornerCapacity(), glm::vec3(0.0f));

    tbb::enumerable_thread_specific<FanScratch> scratch;
    tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.pointCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        FanScratch& s = scratch.local();
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!mesh.pointValid(p))
                continue;
            groupFans(mesh, p, hard, s);
            const size_t k = s.corners.size();
            s.sum.assign(k, glm::dvec3(0.0));
            for (size_t i = 0; i < k; ++i)
            {
                const Index c = s.corners[i];
                s.sum[s.find(Index(i))] += cornerContribution(mesh, c, area[mesh.cornerFace(c)], opts.weight);
            }
            for (size_t i = 0; i < k; ++i)
            {
                const Index c = s.corners[i];
                const glm::dvec3 fallback = normalizeOr(area[mesh.cornerFace(c)], glm::dvec3(0.0, 1.0, 0.0));
                out[c] = glm::vec3(normalizeOr(s.sum[s.find(Index(i))], fallback));
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

CornerNormalPlan::CornerNormalPlan(const Mesh& mesh, const CornerNormalOptions& opts) : mOpts(opts), mTopology(mesh.topologyId())
{
    LILA_ZONE();

    const std::span<const bool> hard = hardEdges(mesh, opts);
    mHard.resize(hard.size());
    if (!hard.empty())
        std::memcpy(mHard.data(), hard.data(), hard.size());

    std::vector<Index> faces;
    faces.reserve(mesh.numFaces());
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            faces.push_back(f);
    mFaceOffsets.resize(faces.size() + 1);
    tbb::parallel_for(size_t(0), faces.size(), [&](size_t i) { mFaceOffsets[i] = mesh.faceSize(faces[i]); });
    mFaceOffsets.back() = 0;
    const uint32_t rows = scanCounts(mFaceOffsets);

    mRowPoint.resize(rows);
    mRowFace.resize(rows);
    UninitVector<uint32_t> cornerRow(mesh.cornerCapacity());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, faces.size(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            uint32_t row = mFaceOffsets[i];
            mesh.forEachFaceCorner(faces[i], [&](Index c) {
                mRowPoint[row] = mesh.cornerPoint(c);
                mRowFace[row] = uint32_t(i);
                cornerRow[c] = row++;
            });
        }
    });

    // Fans are numbered point by point, so each row first gets its fan's ordinal among the fans of its point.
    mRowFan.resize(rows);
    UninitVector<uint32_t> fanBase(size_t(mesh.pointCapacity()) + 1);
    tbb::enumerable_thread_specific<FanScratch> scratch;
    tbb::parallel_for(tbb::blocked_range<Index>(0, mesh.pointCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        FanScratch& s = scratch.local();
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            fanBase[p] = 0;
            if (!mesh.pointValid(p))
                continue;
            groupFans(mesh, p, hard, s);
            const size_t k = s.corners.size();
            s.ordinal.resize(k);
            uint32_t fans = 0;
            for (size_t i = 0; i < k; ++i)
                if (s.find(Index(i)) == Index(i))
                    s.ordinal[i] = fans++;
            for (size_t i = 0; i < k; ++i)
                mRowFan[cornerRow[s.corners[i]]] = s.ordinal[s.find(Index(i))];
            fanBase[p] = fans;
        }
    });
    fanBase.back() = 0;
    const uint32_t fans = scanCounts(fanBase);

    mFanOffsets.resize(size_t(fans) + 1);
    std::fill(mFanOffsets.begin(), mFanOffsets.end(), 0u);
    tbb::parallel_for(tbb::blocked_range<uint32_t>(0, rows, kGrain), [&](const tbb::blocked_range<uint32_t>& r) {
        for (uint32_t row = r.begin(); row < r.end(); ++row)
        {
            mRowFan[row] += fanBase[mRowPoint[row]];
            std::atomic_ref<uint32_t>(mFanOffsets[mRowFan[row]]).fetch_add(1, std::memory_order_relaxed);
        }
    });
    scanCounts(mFanOffsets);

    mFanRows.resize(rows);
    UninitVector<uint32_t> cursor(mFanOffsets.begin(), mFanOffsets.end() - 1);
    tbb::parallel_for(tbb::blocked_range<uint32_t>(0, rows, kGrain), [&](const tbb::blocked_range<uint32_t>& r) {
        for (uint32_t row = r.begin(); row < r.end(); ++row)
            mFanRows[std::atomic_ref<uint32_t>(cursor[mRowFan[row]]).fetch_add(1, std::memory_order_relaxed)] = row;
    });
    // Sorted so a fan sums its corners in the same order every time.
    tbb::parallel_for(tbb::blocked_range<uint32_t>(0, fans, kGrain), [&](const tbb::blocked_range<uint32_t>& r) {
        for (uint32_t fan = r.begin(); fan < r.end(); ++fan)
            std::sort(mFanRows.begin() + mFanOffsets[fan], mFanRows.begin() + mFanOffsets[fan + 1]);
    });
}

bool CornerNormalPlan::fits(const Mesh& mesh) const
{
    if (mesh.topologyId() != mTopology)
        return false;
    const std::span<const bool> hard = hardEdges(mesh, mOpts);
    return hard.size() == mHard.size() && (hard.empty() || std::memcmp(hard.data(), mHard.data(), hard.size()) == 0);
}

void CornerNormalPlan::compute(const Mesh& mesh, std::span<glm::vec3> out)
{
    LILA_ZONE();

    const std::span<const glm::dvec3> pos = mesh.positions();
    const bool angles = mOpts.weight == NormalWeight::Angle || mOpts.weight == NormalWeight::AreaAngle;
    const size_t faces = mFaceOffsets.size() - 1;
    mArea.resize(faces);
    if (angles)
        mRowAngle.resize(rows());
    // Face by face, so the positions each face needs are loaded once for its area and its corner angles.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, faces, kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t f = r.begin(); f < r.end(); ++f)
        {
            const uint32_t first = mFaceOffsets[f], end = mFaceOffsets[f + 1];
            const glm::dvec3 origin = pos[mRowPoint[first]];
            glm::dvec3 n(0.0);
            for (uint32_t row = first; row < end; ++row)
            {
                const glm::dvec3& p = pos[mRowPoint[row]];
                const glm::dvec3& next = pos[mRowPoint[row + 1 == end ? first : row + 1]];
                n += glm::cross(p - origin, next - origin);
                if (angles)
                    mRowAngle[row] = angleAt(p, pos[mRowPoint[row == first ? end - 1 : row - 1]], next);
            }
            mArea[f] = n;
        }
    });

    const size_t fans = mFanOffsets.size() - 1;
    mFanNormal.resize(fans);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, fans, kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t fan = r.begin(); fan < r.end(); ++fan)
        {
            glm::dvec3 sum(0.0);
            for (uint32_t j = mFanOffsets[fan]; j < mFanOffsets[fan + 1]; ++j)
            {
                const uint32_t row = mFanRows[j];
                sum += contribution(mArea[mRowFace[row]], mOpts.weight, [&] { return mRowAngle[row]; });
            }
            // Zero marks a degenerate fan, whose corners take their own face's normal.
            const double len = glm::length(sum);
            mFanNormal[fan] = len > 1e-300 ? glm::vec3(sum / len) : glm::vec3(0.0f);
        }
    });

    tbb::parallel_for(tbb::blocked_range<size_t>(0, rows(), kGrain), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t row = r.begin(); row < r.end(); ++row)
        {
            const glm::vec3 n = mFanNormal[mRowFan[row]];
            out[row] = n != glm::vec3(0.0f) ? n : glm::vec3(normalizeOr(mArea[mRowFace[row]], glm::dvec3(0.0, 1.0, 0.0)));
        }
    });
}

std::vector<glm::vec3> weightedNormals(const Mesh& mesh, const WeightedNormalOptions& opts)
{
    LILA_ZONE();

    const std::vector<glm::dvec3> area = faceAreaVectors(mesh);
    std::span<const bool> hard = opts.keepSharp ? mesh.edgeAttrs().get<bool>(attr::kHard) : std::span<const bool>{};
    std::span<const int32_t> strength = opts.useFaceStrength ? mesh.faceAttrs().get<int32_t>("faceStrength") : std::span<const int32_t>{};
    const double dominance = std::clamp(opts.dominance, 0.0, 1.0);
    std::vector<glm::vec3> out(mesh.cornerCapacity(), glm::vec3(0.0f));

    auto weightOf = [&](Index c) {
        const double a = glm::length(area[mesh.cornerFace(c)]);
        switch (opts.mode)
        {
        case WeightedNormalMode::FaceArea: return a;
        case WeightedNormalMode::CornerAngle: return cornerAngle(mesh, c);
        case WeightedNormalMode::AreaAngle: return a * cornerAngle(mesh, c);
        }
        return a;
    };

    struct Scratch
    {
        std::vector<Index> corners;
        std::vector<Index> parent;
        std::vector<double> weight;
        std::vector<double> maxWeight;
        std::vector<int32_t> maxStrength;
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
            mesh.forEachPointEdge(p, [&](Index e) {
                if ((!hard.empty() && hard[e]) || !mesh.isManifoldEdge(e))
                    return;
                const Index r1 = mesh.edgeCorner(e);
                const Index r2 = mesh.cornerRadial(r1);
                const Index a = local(mesh.cornerPoint(r1) == p ? r1 : mesh.cornerNext(r1));
                const Index b = local(mesh.cornerPoint(r2) == p ? r2 : mesh.cornerNext(r2));
                if (a != kInvalid && b != kInvalid)
                    s.parent[find(a)] = find(b);
            });

            s.weight.resize(k);
            s.maxWeight.assign(k, 0.0);
            s.maxStrength.assign(k, std::numeric_limits<int32_t>::min());
            auto strengthOf = [&](Index c) { return strength.empty() ? 0 : strength[mesh.cornerFace(c)]; };
            for (size_t i = 0; i < k; ++i)
                s.maxStrength[find(Index(i))] = std::max(s.maxStrength[find(Index(i))], strengthOf(s.corners[i]));
            for (size_t i = 0; i < k; ++i)
            {
                s.weight[i] = weightOf(s.corners[i]);
                const Index root = find(Index(i));
                if (strengthOf(s.corners[i]) == s.maxStrength[root])
                    s.maxWeight[root] = std::max(s.maxWeight[root], s.weight[i]);
            }
            s.sum.assign(k, glm::dvec3(0.0));
            for (size_t i = 0; i < k; ++i)
            {
                const Index c = s.corners[i];
                const Index root = find(Index(i));
                if (strengthOf(c) != s.maxStrength[root] || s.weight[i] < dominance * s.maxWeight[root])
                    continue;
                s.sum[root] += normalizeOr(area[mesh.cornerFace(c)], glm::dvec3(0.0)) * s.weight[i];
            }
            for (size_t i = 0; i < k; ++i)
            {
                const Index c = s.corners[i];
                const glm::dvec3 fallback = normalizeOr(area[mesh.cornerFace(c)], glm::dvec3(0.0, 1.0, 0.0));
                out[c] = glm::vec3(normalizeOr(s.sum[find(Index(i))], fallback));
            }
        }
    });
    return out;
}

void applyWeightedNormals(Mesh& mesh, const WeightedNormalOptions& opts, std::span<const Index> faces)
{
    LILA_ZONE();

    const bool had = mesh.cornerAttrs().has(attr::kNormal);
    const std::vector<glm::vec3> before = had || faces.empty() ? std::vector<glm::vec3>() : cornerNormals(mesh);
    const std::vector<glm::vec3> n = weightedNormals(mesh, opts);
    std::span<glm::vec3> dst = mesh.cornerAttrs().getOrAdd<glm::vec3>(attr::kNormal);
    if (faces.empty())
    {
        std::copy(n.begin(), n.end(), dst.begin());
        return;
    }
    // Corners outside the faces keep what they shaded with: their stored normal, or the computed one when none was stored.
    if (!had)
        std::copy(before.begin(), before.end(), dst.begin());
    for (Index f : faces)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](Index c) { dst[c] = n[c]; });
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
    std::span<const char> own; // per triangle: write its tangents; empty writes all

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
    if (!d.own.empty() && !d.own[face])
        return;
    (*d.out)[d.corner(face, vert)] = glm::vec4(t[0], t[1], t[2], sign);
}

/** Per point, one representative of every point at the same float position. MikkTSpace welds by value, not by id. */
std::vector<Index> weldedPoints(const Mesh& mesh)
{
    LILA_ZONE();

    auto key = [&](Index p) {
        const glm::vec3 f = glm::vec3(mesh.position(p)) + 0.0f; // -0 and 0 weld
        return std::array<uint32_t, 3>{std::bit_cast<uint32_t>(f.x), std::bit_cast<uint32_t>(f.y), std::bit_cast<uint32_t>(f.z)};
    };
    std::vector<Index> order;
    order.reserve(mesh.numPoints());
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            order.push_back(p);
    tbb::parallel_sort(order.begin(), order.end(), [&](Index a, Index b) { return key(a) < key(b); });
    std::vector<Index> weld(mesh.pointCapacity(), kInvalid);
    for (size_t i = 0; i < order.size(); ++i)
        weld[order[i]] = i > 0 && key(order[i]) == key(order[i - 1]) ? weld[order[i - 1]] : order[i];
    return weld;
}

/** Triangles around each welded point, CSR: point w's triangles are tris[offsets[w], offsets[w + 1]), ascending. */
struct PointTriangles
{
    std::vector<uint32_t> offsets;
    std::vector<Index> tris;
};

PointTriangles pointTriangles(const Mesh& mesh, const Triangulation& tris, std::span<const Index> weld)
{
    LILA_ZONE();

    PointTriangles a;
    a.offsets.assign(size_t(mesh.pointCapacity()) + 1, 0);
    const size_t n = tris.triangleCount();
    for (size_t t = 0; t < n; ++t)
        for (int k = 0; k < 3; ++k)
            ++a.offsets[weld[mesh.cornerPoint(tris.corners[t * 3 + k])] + 1];
    std::partial_sum(a.offsets.begin(), a.offsets.end(), a.offsets.begin());
    a.tris.resize(a.offsets.back());
    std::vector<uint32_t> fill(a.offsets.begin(), a.offsets.end() - 1);
    for (size_t t = 0; t < n; ++t)
        for (int k = 0; k < 3; ++k)
            a.tris[fill[weld[mesh.cornerPoint(tris.corners[t * 3 + k])]]++] = Index(t);
    return a;
}

/** Chunk boundaries in triangles, about size apart and never inside a face, so each corner is written by one chunk. */
std::vector<Index> chunkBounds(const Triangulation& tris, Index size)
{
    const Index n = tris.triangleCount();
    std::vector<Index> bounds{0};
    for (Index t = size; t < n; t += size)
    {
        while (t < n && tris.faces[t] == tris.faces[t - 1])
            ++t;
        if (t < n)
            bounds.push_back(t);
    }
    bounds.push_back(n);
    return bounds;
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
    auto run = [&](const Triangulation& part, std::span<const char> own) {
        MikkData d{&mesh, &part, normals, uv, &out, own};
        SMikkTSpaceContext ctx{&iface, &d};
        genTangSpaceDefault(&ctx);
    };

    const Index n = tris.triangleCount();
    const Index chunk = std::max<Index>(1u << 15, n / Index(4 * tbb::this_task_arena::max_concurrency()));
    const std::vector<Index> bounds = parallel ? chunkBounds(tris, chunk) : std::vector<Index>{};
    if (bounds.size() <= 2)
    {
        run(tris, {});
        return out;
    }

    // A corner's tangent depends only on the triangles around its welded point. Each chunk runs with every triangle
    // touching it, in the original order, and writes only its own corners: the same result as one call.
    const std::vector<Index> weld = weldedPoints(mesh);
    const PointTriangles around = pointTriangles(mesh, tris, weld);
    tbb::parallel_for(size_t(0), bounds.size() - 1, size_t(1), [&](size_t c) {
        const Index lo = bounds[c], hi = bounds[c + 1];
        std::vector<Index> halo;
        for (size_t i = size_t(lo) * 3; i < size_t(hi) * 3; ++i)
        {
            const Index w = weld[mesh.cornerPoint(tris.corners[i])];
            for (uint32_t k = around.offsets[w]; k < around.offsets[w + 1]; ++k)
                if (around.tris[k] < lo || around.tris[k] >= hi)
                    halo.push_back(around.tris[k]);
        }
        std::sort(halo.begin(), halo.end());
        halo.erase(std::unique(halo.begin(), halo.end()), halo.end());

        Triangulation part;
        std::vector<char> own;
        const size_t count = size_t(hi - lo) + halo.size();
        part.corners.reserve(count * 3);
        part.faces.reserve(count);
        own.reserve(count);
        auto add = [&](Index t, bool mine) {
            part.corners.insert(part.corners.end(), tris.corners.begin() + size_t(t) * 3, tris.corners.begin() + size_t(t) * 3 + 3);
            part.faces.push_back(tris.faces[t]);
            own.push_back(mine);
        };
        auto h = halo.begin();
        for (; h != halo.end() && *h < lo; ++h)
            add(*h, false);
        for (Index t = lo; t < hi; ++t)
            add(t, true);
        for (; h != halo.end(); ++h)
            add(*h, false);
        run(part, own);
    });
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
