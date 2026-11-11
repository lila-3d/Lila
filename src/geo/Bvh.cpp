#include "geo/Bvh.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <tbb/task_group.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>

namespace lila::geo
{
namespace
{
constexpr int kBins = 16;
constexpr uint32_t kMaxLeaf = 8;
constexpr uint32_t kParallelThreshold = 16384;
constexpr int kMaxSahDepth = 40;
constexpr int kStack = 96;

struct Box
{
    glm::vec3 min{std::numeric_limits<float>::max()};
    glm::vec3 max{-std::numeric_limits<float>::max()};

    void extend(const glm::vec3& p)
    {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void extend(const Box& b)
    {
        min = glm::min(min, b.min);
        max = glm::max(max, b.max);
    }
    float area() const
    {
        if (min.x > max.x)
            return 0.0f;
        const glm::vec3 d = max - min;
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
};

/** Float box that contains the double one. */
Box outward(const glm::dvec3& lo, const glm::dvec3& hi)
{
    Box b;
    for (int k = 0; k < 3; ++k)
    {
        b.min[k] = std::nextafter(float(lo[k]), -std::numeric_limits<float>::infinity());
        b.max[k] = std::nextafter(float(hi[k]), std::numeric_limits<float>::infinity());
    }
    return b;
}

/** Slab test against a node; returns the entry distance or +inf. */
double hitBox(const float* bmin, const float* bmax, const glm::dvec3& o, const glm::dvec3& inv, double tMax)
{
    double t0 = 0.0, t1 = tMax;
    for (int k = 0; k < 3; ++k)
    {
        double a = (double(bmin[k]) - o[k]) * inv[k];
        double b = (double(bmax[k]) - o[k]) * inv[k];
        if (a > b)
            std::swap(a, b);
        // NaN from 0 * inf means the ray runs inside the slab plane; keep the interval.
        if (a == a)
            t0 = std::max(t0, a);
        if (b == b)
            t1 = std::min(t1, b);
        if (t0 > t1)
            return std::numeric_limits<double>::infinity();
    }
    return t0;
}

double boxDistance2(const float* bmin, const float* bmax, const glm::dvec3& p)
{
    double d2 = 0.0;
    for (int k = 0; k < 3; ++k)
    {
        const double v = p[k] < bmin[k] ? bmin[k] - p[k] : p[k] > bmax[k] ? p[k] - bmax[k] : 0.0;
        d2 += v * v;
    }
    return d2;
}

/** Moller-Trumbore. Returns t, writes barycentrics (u, v) for corners 1 and 2. */
bool intersectTri(const glm::dvec3& o, const glm::dvec3& d, const glm::dvec3& a, const glm::dvec3& b,
                  const glm::dvec3& c, double tMax, bool backfaces, double& t, double& u, double& v, bool& front)
{
    const glm::dvec3 e1 = b - a, e2 = c - a;
    const glm::dvec3 p = glm::cross(d, e2);
    const double det = glm::dot(e1, p);
    front = det > 0.0;
    if (det == 0.0 || (!backfaces && !front))
        return false;
    const double inv = 1.0 / det;
    const glm::dvec3 s = o - a;
    u = glm::dot(s, p) * inv;
    if (u < 0.0 || u > 1.0)
        return false;
    const glm::dvec3 q = glm::cross(s, e1);
    v = glm::dot(d, q) * inv;
    if (v < 0.0 || u + v > 1.0)
        return false;
    t = glm::dot(e2, q) * inv;
    return t >= 0.0 && t <= tMax;
}

/** Closest point on triangle abc to p (Ericson), with barycentrics. */
glm::dvec3 closestOnTri(const glm::dvec3& p, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c,
                        glm::dvec3& bary)
{
    const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0)
    {
        bary = {1, 0, 0};
        return a;
    }
    const glm::dvec3 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3)
    {
        bary = {0, 1, 0};
        return b;
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        const double v = d1 / (d1 - d3);
        bary = {1 - v, v, 0};
        return a + v * ab;
    }
    const glm::dvec3 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6)
    {
        bary = {0, 0, 1};
        return c;
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        const double w = d2 / (d2 - d6);
        bary = {1 - w, 0, w};
        return a + w * ac;
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        bary = {0, 1 - w, w};
        return b + w * (c - b);
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    bary = {1 - v - w, v, w};
    return a + ab * v + ac * w;
}
} // namespace

void MeshBvh::triPoints(Index t, glm::dvec3& a, glm::dvec3& b, glm::dvec3& c) const
{
    const Index* p = &mTriPoints[size_t(t) * 3];
    a = mPositions[p[0]];
    b = mPositions[p[1]];
    c = mPositions[p[2]];
}

void MeshBvh::build(const Mesh& mesh)
{
    build(mesh, triangulate(mesh));
}

void MeshBvh::build(const Mesh& mesh, Triangulation tris)
{
    LILA_ZONE();

    mTris = std::move(tris);
    mParent.clear();
    mTriLeaf.clear();
    mPointTriOffsets.clear();
    mPointTris.clear();
    mNodeDirty.clear();
    mPositions.assign(mesh.positions().begin(), mesh.positions().end());
    mTriPoints.resize(mTris.corners.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, mTris.corners.size(), 1 << 16), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
            mTriPoints[i] = mesh.cornerPoint(mTris.corners[i]);
    });
    buildTree();
}

void MeshBvh::buildSoup(std::span<const glm::vec3> corners)
{
    LILA_ZONE();

    const size_t n = corners.size() / 3;
    mParent.clear();
    mTriLeaf.clear();
    mPointTriOffsets.clear();
    mPointTris.clear();
    mNodeDirty.clear();
    mTris.corners.resize(n * 3);
    mTris.faces.resize(n);
    mTris.faceFirst.resize(n);
    mTriPoints.resize(n * 3);
    mPositions.resize(n * 3);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 1 << 14), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t t = r.begin(); t < r.end(); ++t)
        {
            mTris.faces[t] = mTris.faceFirst[t] = Index(t);
            for (size_t k = t * 3; k < t * 3 + 3; ++k)
            {
                mTris.corners[k] = mTriPoints[k] = Index(k);
                mPositions[k] = glm::dvec3(corners[k]);
            }
        }
    });
    buildTree();
}

void MeshBvh::buildTree()
{
    LILA_ZONE();

    const uint32_t n = uint32_t(mTris.faces.size());
    mNodes.clear();
    mOrder.resize(n);
    if (n == 0)
        return;
    std::vector<Box> triBox(n);
    std::vector<glm::vec3> centroid(n);
    tbb::parallel_for(tbb::blocked_range<uint32_t>(0, n, 4096), [&](const tbb::blocked_range<uint32_t>& r) {
        for (uint32_t t = r.begin(); t < r.end(); ++t)
        {
            glm::dvec3 a, b, c;
            triPoints(t, a, b, c);
            triBox[t] = outward(glm::min(a, glm::min(b, c)), glm::max(a, glm::max(b, c)));
            centroid[t] = (triBox[t].min + triBox[t].max) * 0.5f;
            mOrder[t] = t;
        }
    });

    mNodes.resize(size_t(n) * 2);
    std::atomic<uint32_t> nodeCount{1};

    auto setNode = [&](uint32_t id, const Box& b, uint32_t first, uint32_t count) {
        Node& nd = mNodes[id];
        for (int k = 0; k < 3; ++k)
        {
            nd.min[k] = b.min[k];
            nd.max[k] = b.max[k];
        }
        nd.first = first;
        nd.count = count;
    };

    auto rangeBounds = [&](uint32_t begin, uint32_t end, Box& bounds, Box& cbounds) {
        for (uint32_t i = begin; i < end; ++i)
        {
            bounds.extend(triBox[mOrder[i]]);
            cbounds.extend(centroid[mOrder[i]]);
        }
    };

    std::function<void(uint32_t, uint32_t, uint32_t, int)> recurse = [&](uint32_t id, uint32_t begin, uint32_t end, int depth) {
        Box bounds, cbounds;
        rangeBounds(begin, end, bounds, cbounds);
        const uint32_t count = end - begin;
        if (count <= 2)
        {
            setNode(id, bounds, begin, count);
            return;
        }
        const glm::vec3 ext = cbounds.max - cbounds.min;
        const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        uint32_t mid = begin;
        // Past this depth, split by count so traversal stacks stay small.
        if (ext[axis] > 0.0f && depth < kMaxSahDepth)
        {
            Box binBox[kBins];
            uint32_t binCount[kBins] = {};
            const float scale = kBins / ext[axis];
            auto binOf = [&](uint32_t t) {
                return std::min(kBins - 1, int((centroid[t][axis] - cbounds.min[axis]) * scale));
            };
            for (uint32_t i = begin; i < end; ++i)
            {
                const int b = binOf(mOrder[i]);
                ++binCount[b];
                binBox[b].extend(triBox[mOrder[i]]);
            }
            float rightArea[kBins];
            uint32_t rightCount[kBins];
            Box acc;
            uint32_t cnt = 0;
            for (int b = kBins - 1; b > 0; --b)
            {
                acc.extend(binBox[b]);
                cnt += binCount[b];
                rightArea[b] = acc.area();
                rightCount[b] = cnt;
            }
            float bestCost = std::numeric_limits<float>::max();
            int bestSplit = -1;
            acc = Box();
            cnt = 0;
            for (int b = 1; b < kBins; ++b)
            {
                acc.extend(binBox[b - 1]);
                cnt += binCount[b - 1];
                if (cnt == 0 || rightCount[b] == 0)
                    continue;
                const float cost = acc.area() * float(cnt) + rightArea[b] * float(rightCount[b]);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestSplit = b;
                }
            }
            const float leafCost = bounds.area() * float(count);
            if (count <= kMaxLeaf && (bestSplit < 0 || leafCost <= bestCost))
            {
                setNode(id, bounds, begin, count);
                return;
            }
            if (bestSplit > 0)
                mid = uint32_t(std::partition(mOrder.begin() + begin, mOrder.begin() + end,
                                              [&](uint32_t t) { return binOf(t) < bestSplit; })
                               - mOrder.begin());
        }
        if (mid == begin || mid == end)
        {
            if (count <= kMaxLeaf)
            {
                setNode(id, bounds, begin, count);
                return;
            }
            // Coincident centroids: split by count.
            mid = begin + count / 2;
            std::nth_element(mOrder.begin() + begin, mOrder.begin() + mid, mOrder.begin() + end,
                             [&](uint32_t a, uint32_t b) { return centroid[a][axis] < centroid[b][axis]; });
        }
        const uint32_t left = nodeCount.fetch_add(2);
        setNode(id, bounds, left, 0);
        if (count > kParallelThreshold)
        {
            tbb::task_group g;
            g.run([&, left, begin, mid, depth] { recurse(left, begin, mid, depth + 1); });
            recurse(left + 1, mid, end, depth + 1);
            g.wait();
        }
        else
        {
            recurse(left, begin, mid, depth + 1);
            recurse(left + 1, mid, end, depth + 1);
        }
    };
    recurse(0, 0, n, 0);
    mNodes.resize(nodeCount.load());
}

void MeshBvh::refit(const Mesh& mesh)
{
    LILA_ZONE();

    if (mesh.pointCapacity() != mPositions.size() || mNodes.empty())
    {
        build(mesh);
        return;
    }
    mPositions.assign(mesh.positions().begin(), mesh.positions().end());
    // Children always come after their parent, so a reverse sweep sees children first.
    for (size_t i = mNodes.size(); i-- > 0;)
    {
        Node& nd = mNodes[i];
        Box b;
        if (nd.count)
            for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
            {
                glm::dvec3 a, bb, c;
                triPoints(mOrder[k], a, bb, c);
                b.extend(outward(glm::min(a, glm::min(bb, c)), glm::max(a, glm::max(bb, c))));
            }
        else
            for (uint32_t k = nd.first; k < nd.first + 2; ++k)
            {
                const Node& ch = mNodes[k];
                b.extend(glm::vec3(ch.min[0], ch.min[1], ch.min[2]));
                b.extend(glm::vec3(ch.max[0], ch.max[1], ch.max[2]));
            }
        for (int k = 0; k < 3; ++k)
        {
            nd.min[k] = b.min[k];
            nd.max[k] = b.max[k];
        }
    }
}

void MeshBvh::refitPoints(const Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    if (mesh.pointCapacity() != mPositions.size() || mNodes.empty())
    {
        build(mesh);
        return;
    }
    if (mParent.size() != mNodes.size())
    {
        mParent.assign(mNodes.size(), 0);
        mTriLeaf.assign(mTris.faces.size(), 0);
        for (uint32_t i = 0; i < mNodes.size(); ++i)
        {
            const Node& nd = mNodes[i];
            if (nd.count)
                for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
                    mTriLeaf[mOrder[k]] = i;
            else
                mParent[nd.first] = mParent[nd.first + 1] = i;
        }
        mPointTriOffsets.assign(mPositions.size() + 1, 0);
        for (Index p : mTriPoints)
            ++mPointTriOffsets[p + 1];
        for (size_t i = 1; i < mPointTriOffsets.size(); ++i)
            mPointTriOffsets[i] += mPointTriOffsets[i - 1];
        mPointTris.resize(mTriPoints.size());
        std::vector<uint32_t> fill(mPointTriOffsets.begin(), mPointTriOffsets.end() - 1);
        for (size_t i = 0; i < mTriPoints.size(); ++i)
            mPointTris[fill[mTriPoints[i]]++] = Index(i / 3);
        mNodeDirty.assign(mNodes.size(), 0);
    }

    std::vector<uint32_t> dirty;
    for (Index p : points)
    {
        if (p >= mPositions.size())
            continue;
        mPositions[p] = mesh.position(p);
        for (uint32_t k = mPointTriOffsets[p]; k < mPointTriOffsets[p + 1]; ++k)
            for (uint32_t n = mTriLeaf[mPointTris[k]];; n = mParent[n])
            {
                if (mNodeDirty[n])
                    break;
                mNodeDirty[n] = 1;
                dirty.push_back(n);
                if (n == 0)
                    break;
            }
    }
    // Children have larger ids than their parent, so descending order refits children first.
    std::sort(dirty.begin(), dirty.end(), std::greater<>());
    for (uint32_t i : dirty)
    {
        Node& nd = mNodes[i];
        Box b;
        if (nd.count)
            for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
            {
                glm::dvec3 a, bb, c;
                triPoints(mOrder[k], a, bb, c);
                b.extend(outward(glm::min(a, glm::min(bb, c)), glm::max(a, glm::max(bb, c))));
            }
        else
            for (uint32_t k = nd.first; k < nd.first + 2; ++k)
            {
                const Node& ch = mNodes[k];
                b.extend(glm::vec3(ch.min[0], ch.min[1], ch.min[2]));
                b.extend(glm::vec3(ch.max[0], ch.max[1], ch.max[2]));
            }
        for (int k = 0; k < 3; ++k)
        {
            nd.min[k] = b.min[k];
            nd.max[k] = b.max[k];
        }
        mNodeDirty[i] = 0;
    }
}

FlatBvh MeshBvh::exportFlat() const
{
    LILA_ZONE();

    static_assert(sizeof(FlatBvh::Node) == sizeof(Node));
    FlatBvh out;
    out.nodes.resize(mNodes.size());
    std::memcpy(out.nodes.data(), mNodes.data(), mNodes.size() * sizeof(Node));
    out.corners.resize(mOrder.size() * 3);
    tbb::parallel_for(size_t(0), mOrder.size(), [&](size_t k) {
        glm::dvec3 p[3];
        triPoints(mOrder[k], p[0], p[1], p[2]);
        uint32_t tri = mOrder[k];
        float w;
        std::memcpy(&w, &tri, sizeof(w));
        for (int c = 0; c < 3; ++c)
            out.corners[k * 3 + size_t(c)] = glm::vec4(glm::vec3(p[c]), c == 0 ? w : 0.0f);
    });
    return out;
}

Bounds MeshBvh::bounds() const
{
    Bounds b;
    if (!mNodes.empty())
    {
        b.extend(glm::dvec3(mNodes[0].min[0], mNodes[0].min[1], mNodes[0].min[2]));
        b.extend(glm::dvec3(mNodes[0].max[0], mNodes[0].max[1], mNodes[0].max[2]));
    }
    return b;
}

bool MeshBvh::raycast(const glm::dvec3& origin, const glm::dvec3& dir, BvhHit& hit, double tMax, bool backfaces) const
{
    if (mNodes.empty())
        return false;
    const glm::dvec3 inv = 1.0 / dir;
    double best = tMax;
    Index bestTri = kInvalid;
    double bu = 0, bv = 0;
    bool bestFront = true;
    uint32_t stack[kStack];
    int sp = 0;
    stack[sp++] = 0;
    while (sp)
    {
        const Node& nd = mNodes[stack[--sp]];
        if (hitBox(nd.min, nd.max, origin, inv, best) == std::numeric_limits<double>::infinity())
            continue;
        if (nd.count)
        {
            for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
            {
                const Index t = mOrder[k];
                glm::dvec3 a, b, c;
                triPoints(t, a, b, c);
                double tt, u, v;
                bool front;
                if (intersectTri(origin, dir, a, b, c, best, backfaces, tt, u, v, front))
                {
                    best = tt;
                    bestTri = t;
                    bu = u;
                    bv = v;
                    bestFront = front;
                }
            }
            continue;
        }
        const Node& l = mNodes[nd.first];
        const Node& r = mNodes[nd.first + 1];
        const double tl = hitBox(l.min, l.max, origin, inv, best);
        const double tr = hitBox(r.min, r.max, origin, inv, best);
        // Push the far child first so the near one is tested first.
        if (tl < tr)
        {
            if (tr != std::numeric_limits<double>::infinity())
                stack[sp++] = nd.first + 1;
            stack[sp++] = nd.first;
        }
        else
        {
            if (tl != std::numeric_limits<double>::infinity())
                stack[sp++] = nd.first;
            if (tr != std::numeric_limits<double>::infinity())
                stack[sp++] = nd.first + 1;
        }
    }
    if (bestTri == kInvalid)
        return false;
    hit.t = best;
    hit.triangle = bestTri;
    hit.face = mTris.faces[bestTri];
    hit.bary = {1.0 - bu - bv, bu, bv};
    hit.point = origin + dir * best;
    hit.frontFacing = bestFront;
    return true;
}

bool MeshBvh::occluded(const glm::dvec3& origin, const glm::dvec3& dir, double tMax) const
{
    if (mNodes.empty())
        return false;
    const glm::dvec3 inv = 1.0 / dir;
    uint32_t stack[kStack];
    int sp = 0;
    stack[sp++] = 0;
    while (sp)
    {
        const Node& nd = mNodes[stack[--sp]];
        if (hitBox(nd.min, nd.max, origin, inv, tMax) == std::numeric_limits<double>::infinity())
            continue;
        if (nd.count)
        {
            for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
            {
                glm::dvec3 a, b, c;
                triPoints(mOrder[k], a, b, c);
                double tt, u, v;
                bool front;
                if (intersectTri(origin, dir, a, b, c, tMax, true, tt, u, v, front))
                    return true;
            }
            continue;
        }
        stack[sp++] = nd.first;
        stack[sp++] = nd.first + 1;
    }
    return false;
}

bool MeshBvh::closestPoint(const glm::dvec3& p, BvhClosest& out, double maxDistance) const
{
    if (mNodes.empty())
        return false;
    double best2 = maxDistance == std::numeric_limits<double>::max() ? maxDistance : maxDistance * maxDistance;
    Index bestTri = kInvalid;
    glm::dvec3 bestPoint(0.0), bestBary(0.0);
    uint32_t stack[kStack];
    int sp = 0;
    stack[sp++] = 0;
    while (sp)
    {
        const Node& nd = mNodes[stack[--sp]];
        if (boxDistance2(nd.min, nd.max, p) > best2)
            continue;
        if (nd.count)
        {
            for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
            {
                const Index t = mOrder[k];
                glm::dvec3 a, b, c, bary;
                triPoints(t, a, b, c);
                const glm::dvec3 q = closestOnTri(p, a, b, c, bary);
                const glm::dvec3 d = q - p;
                const double d2 = glm::dot(d, d);
                if (d2 < best2)
                {
                    best2 = d2;
                    bestTri = t;
                    bestPoint = q;
                    bestBary = bary;
                }
            }
            continue;
        }
        const Node& l = mNodes[nd.first];
        const Node& r = mNodes[nd.first + 1];
        const double dl = boxDistance2(l.min, l.max, p);
        const double dr = boxDistance2(r.min, r.max, p);
        if (dl < dr)
        {
            stack[sp++] = nd.first + 1;
            stack[sp++] = nd.first;
        }
        else
        {
            stack[sp++] = nd.first;
            stack[sp++] = nd.first + 1;
        }
    }
    if (bestTri == kInvalid)
        return false;
    out.distance = std::sqrt(best2);
    out.triangle = bestTri;
    out.face = mTris.faces[bestTri];
    out.point = bestPoint;
    out.bary = bestBary;
    return true;
}

std::vector<Index> MeshBvh::overlapping(const Bounds& box) const
{
    std::vector<Index> out;
    if (mNodes.empty())
        return out;
    uint32_t stack[kStack];
    int sp = 0;
    stack[sp++] = 0;
    while (sp)
    {
        const Node& nd = mNodes[stack[--sp]];
        bool overlap = true;
        for (int k = 0; k < 3; ++k)
            overlap &= nd.min[k] <= box.max[k] && nd.max[k] >= box.min[k];
        if (!overlap)
            continue;
        if (nd.count)
        {
            for (uint32_t k = nd.first; k < nd.first + nd.count; ++k)
                out.push_back(mOrder[k]);
            continue;
        }
        stack[sp++] = nd.first;
        stack[sp++] = nd.first + 1;
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace lila::geo
