#include "geo/Reduce.h"

#include "core/Profile.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Symmetry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <queue>
#include <set>

namespace lila::geo
{
namespace
{
std::vector<Index> liveFaces(const Mesh& m)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

Index findRoot(std::vector<Index>& parent, Index x)
{
    while (parent[x] != x)
        x = parent[x] = parent[parent[x]];
    return x;
}

/** Per corner: an id shared by the corners around a point that a run of soft edges joins (one smoothing fan). */
std::vector<Index> smoothFans(const Mesh& m, bool useHardEdges)
{
    std::vector<Index> parent(m.cornerCapacity());
    std::iota(parent.begin(), parent.end(), Index(0));
    std::span<const bool> hard = m.edgeAttrs().get<bool>(attr::kHard);
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        m.forEachFaceCorner(f, [&](Index c) {
            const Index e = m.cornerEdge(c);
            if (!m.isManifoldEdge(e) || (useHardEdges && e < hard.size() && hard[e]))
                return;
            const Index other = m.cornerRadial(c);
            const Index p = m.cornerPoint(c), q = m.cornerNextPoint(c);
            const Index atP = m.cornerPoint(other) == p ? other : m.cornerNext(other);
            const Index atQ = m.cornerPoint(other) == q ? other : m.cornerNext(other);
            parent[findRoot(parent, c)] = findRoot(parent, atP);
            parent[findRoot(parent, m.cornerNext(c))] = findRoot(parent, atQ);
        });
    }
    for (Index c = 0; c < Index(parent.size()); ++c)
        parent[c] = findRoot(parent, c);
    return parent;
}

struct Wedges
{
    std::vector<Index> ofCorner; // per corner capacity
    std::vector<Index> corner;   // representative corner per wedge
    std::vector<Index> point;    // per wedge
    std::vector<Index> fan;      // per wedge, smoothing fan id
};

/** Splits each point into one wedge per distinct look: smoothing fan, corner attributes (tangents aside), material. */
Wedges buildWedges(const Mesh& m, bool useHardEdges)
{
    Wedges w;
    w.ofCorner.assign(m.cornerCapacity(), kInvalid);
    const std::vector<Index> fans = smoothFans(m, useHardEdges);
    std::vector<const Attribute*> layers;
    for (size_t i = 0; i < m.cornerAttrs().layerCount(); ++i)
        if (m.cornerAttrs().layer(i).name() != attr::kTangent)
            layers.push_back(&m.cornerAttrs().layer(i));
    std::span<const int32_t> material = m.faceAttrs().get<int32_t>(attr::kMaterial);
    const auto same = [&](Index a, Index b) {
        if (fans[a] != fans[b])
            return false;
        if (!material.empty() && material[m.cornerFace(a)] != material[m.cornerFace(b)])
            return false;
        for (const Attribute* l : layers)
            if (std::memcmp(l->raw(a), l->raw(b), l->stride()) != 0)
                return false;
        return true;
    };
    std::vector<Index> mine;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        mine.clear();
        m.forEachPointCorner(p, [&](Index c) {
            for (Index k : mine)
                if (same(w.corner[k], c))
                {
                    w.ofCorner[c] = k;
                    return;
                }
            const Index k = Index(w.corner.size());
            w.corner.push_back(c);
            w.point.push_back(p);
            w.fan.push_back(fans[c]);
            w.ofCorner[c] = k;
            mine.push_back(k);
        });
    }
    return w;
}

/** Symmetric 4x4 error quadric, upper triangle. */
struct Quadric
{
    std::array<double, 10> q{};

    void addPlane(const glm::dvec3& n, double d, double w)
    {
        const double v[4] = {n.x, n.y, n.z, d};
        int k = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = i; j < 4; ++j)
                q[k++] += w * v[i] * v[j];
    }
    void add(const Quadric& o)
    {
        for (int k = 0; k < 10; ++k)
            q[k] += o.q[k];
    }
    double eval(const glm::dvec3& p) const
    {
        const double x = p.x, y = p.y, z = p.z;
        return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y
             + q[7] * z * z + 2 * q[8] * z + q[9];
    }
};

/**
 * Half-edge collapse simplifier over wedges. A point merges into a neighbour, which keeps its position and attributes,
 * so nothing is interpolated. A point on a seam, hard edge or border may only slide along it, and never past where it
 * ends or branches. Every collapse is checked: the link condition (no pinched or doubled edges), and no triangle may
 * turn over, collapse to a sliver, or turn over in uv.
 */
class Collapser
{
public:
    Collapser(const Mesh& m, const Wedges& w, std::span<const glm::vec2> uv, const std::vector<uint8_t>& locked, bool keepBorders)
        : mMesh(m), mW(w), mUv(uv), mLocked(locked), mKeepBorders(keepBorders)
    {
        const Index np = m.pointCapacity();
        mPointTris.resize(np);
        mQ.resize(np);
        mVersion.assign(np, 0);
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            std::array<Index, 3> t{};
            int k = 0;
            m.forEachFaceCorner(f, [&](Index c) { t[k++] = w.ofCorner[c]; });
            const Index id = Index(mTris.size());
            mTris.push_back(t);
            mAlive.push_back(1);
            mStartNormal.push_back(triNormal(t));
            for (Index x : t)
                mPointTris[w.point[x]].push_back(id);
        }
        mAliveCount = Index(mTris.size());
        const std::vector<glm::vec3> smooth = pointNormals(m);
        mSmooth.assign(smooth.begin(), smooth.end());
        const double size = glm::length(m.bounds().size());
        mMinArea = 1e-12 * size * size;
        for (Index t = 0; t < Index(mTris.size()); ++t)
        {
            const glm::dvec3 n = triNormal(mTris[t]);
            const double len = glm::length(n);
            if (len <= 0.0)
                continue;
            const glm::dvec3 u = n / len;
            for (Index x : mTris[t])
            {
                const Index p = mW.point[x];
                mQ[p].addPlane(u, -glm::dot(u, pos(p)), 0.5 * len);
            }
        }
        // Planes standing on the feature edges keep seams, borders and hard lines straight as points slide along them.
        for (Index p = 0; p < np; ++p)
            for (Index o : neighbours(p))
                if (p < o && isFeature(p, o))
                {
                    const Index t = mPointTris[p].empty() ? kInvalid : firstShared(p, o);
                    if (t == kInvalid)
                        continue;
                    const glm::dvec3 e = pos(o) - pos(p);
                    glm::dvec3 n = glm::cross(e, glm::normalize(triNormal(mTris[t])));
                    const double len = glm::length(n);
                    if (len <= 0.0)
                        continue;
                    n /= len;
                    const double weight = 10.0 * glm::dot(e, e);
                    mQ[p].addPlane(n, -glm::dot(n, pos(p)), weight);
                    mQ[o].addPlane(n, -glm::dot(n, pos(o)), weight);
                }
    }

    void run(Index targetTris)
    {
        using Entry = std::tuple<double, Index, Index, uint32_t>; // cost, from, to, version
        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap;
        const auto push = [&](Index u) {
            Index to;
            const double c = bestCollapse(u, to);
            if (to != kInvalid)
                heap.push({c, u, to, mVersion[u]});
        };
        for (Index p = 0; p < Index(mPointTris.size()); ++p)
            push(p);
        while (mAliveCount > targetTris && !heap.empty())
        {
            const auto [cost, u, v, version] = heap.top();
            heap.pop();
            if (version != mVersion[u] || mPointTris[u].empty())
                continue;
            if (!canCollapse(u, v))
            {
                ++mVersion[u];
                push(u);
                continue;
            }
            collapse(u, v);
            mMaxCost = std::max(mMaxCost, cost);
            // u's neighbours all became v's, so v's ring is everything whose options changed.
            std::vector<Index> touched = neighbours(v);
            touched.push_back(v);
            for (Index p : touched)
            {
                ++mVersion[p];
                push(p);
            }
        }
    }

    /** Surviving triangles as wedge triples. */
    std::vector<std::array<Index, 3>> triangles() const
    {
        std::vector<std::array<Index, 3>> out;
        for (Index t = 0; t < Index(mTris.size()); ++t)
            if (mAlive[t])
                out.push_back(mTris[t]);
        return out;
    }

    double maxCost() const { return mMaxCost; }

private:
    const glm::dvec3& pos(Index p) const { return mMesh.position(p); }

    glm::dvec3 triNormal(const std::array<Index, 3>& t) const
    {
        const glm::dvec3 a = pos(mW.point[t[0]]), b = pos(mW.point[t[1]]), c = pos(mW.point[t[2]]);
        return glm::cross(b - a, c - a);
    }

    double uvArea(const std::array<Index, 3>& t) const
    {
        if (mUv.empty())
            return 0.0;
        const glm::dvec2 a = mUv[mW.corner[t[0]]], b = mUv[mW.corner[t[1]]], c = mUv[mW.corner[t[2]]];
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    int slot(Index t, Index p) const
    {
        for (int k = 0; k < 3; ++k)
            if (mW.point[mTris[t][k]] == p)
                return k;
        return -1;
    }

    std::vector<Index> neighbours(Index p) const
    {
        std::vector<Index> out;
        neighbours(p, out);
        return out;
    }

    void neighbours(Index p, std::vector<Index>& out) const
    {
        out.clear();
        for (Index t : mPointTris[p])
            for (Index x : mTris[t])
                if (const Index q = mW.point[x]; q != p && std::find(out.begin(), out.end(), q) == out.end())
                    out.push_back(q);
    }

    Index firstShared(Index a, Index b) const
    {
        for (Index t : mPointTris[a])
            if (slot(t, b) >= 0)
                return t;
        return kInvalid;
    }

    /**
     * Border, non-manifold, or the two faces disagree on a wedge at a (or, with both ends, at either end). A split
     * only at the far end, like the fan of uv corners at a sphere's pole, doesn't hold a back.
     */
    bool isFeature(Index a, Index b, bool bothEnds = true) const
    {
        Index shared[3];
        int n = 0;
        for (Index t : mPointTris[a])
            if (slot(t, b) >= 0)
            {
                if (n == 2)
                    return true;
                shared[n++] = t;
            }
        if (n != 2)
            return true;
        return mTris[shared[0]][slot(shared[0], a)] != mTris[shared[1]][slot(shared[1], a)]
            || (bothEnds && mTris[shared[0]][slot(shared[0], b)] != mTris[shared[1]][slot(shared[1], b)]);
    }

    bool isBorder(Index a, Index b) const
    {
        int n = 0;
        for (Index t : mPointTris[a])
            n += slot(t, b) >= 0;
        return n == 1;
    }

    double bestCollapse(Index u, Index& to) const
    {
        to = kInvalid;
        if (mLocked[u] || mPointTris[u].empty())
            return 0.0;
        std::vector<Index>& nb = mNb;
        neighbours(u, nb);
        Index features = 0, borders = 0;
        Index ends[2] = {kInvalid, kInvalid};
        for (Index o : nb)
            if (isFeature(u, o, false))
            {
                if (features < 2)
                    ends[features] = o;
                ++features;
                borders += isBorder(u, o);
            }
        if ((features != 0 && features != 2) || (borders && mKeepBorders) || borders == 1)
            return 0.0;
        // Where a feature line turns sharply the point is a corner of it, and stays.
        if (features == 2)
        {
            const glm::dvec3 a = pos(u) - pos(ends[0]), b = pos(ends[1]) - pos(u);
            if (glm::dot(a, b) < std::cos(glm::radians(45.0)) * glm::length(a) * glm::length(b))
                return 0.0;
        }
        mCandidates.clear();
        for (Index v : nb)
            if (features != 2 || v == ends[0] || v == ends[1])
                mCandidates.push_back({mQ[u].eval(pos(v)) + mQ[v].eval(pos(v)) + 1e-9 * glm::distance(pos(u), pos(v)), v});
        std::sort(mCandidates.begin(), mCandidates.end());
        for (const auto& [cost, v] : mCandidates)
            if (canCollapse(u, v))
            {
                to = v;
                return cost;
            }
        return 0.0;
    }

    /** The v wedge each u wedge becomes: the one the triangles across edge uv use on the same side. */
    bool wedgeMap(Index u, Index v, std::vector<std::pair<Index, Index>>& map) const
    {
        map.clear();
        for (Index t : mPointTris[u])
        {
            const int sv = slot(t, v);
            if (sv < 0)
                continue;
            const Index wu = mTris[t][slot(t, u)], wv = mTris[t][sv];
            auto it = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == wu; });
            if (it == map.end())
                map.push_back({wu, wv});
            else if (it->second != wv)
                return false;
        }
        for (Index t : mPointTris[u])
        {
            const Index wu = mTris[t][slot(t, u)];
            if (std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == wu; }) == map.end())
                return false;
        }
        return !map.empty();
    }

    bool canCollapse(Index u, Index v) const
    {
        std::vector<std::pair<Index, Index>>& map = mMap;
        if (!wedgeMap(u, v, map))
            return false;
        // Link condition: the only points u and v share are the far corners of the triangles on edge uv.
        std::vector<Index>& across = mAcross;
        across.clear();
        for (Index t : mPointTris[u])
            if (slot(t, v) >= 0)
                for (Index x : mTris[t])
                    if (const Index q = mW.point[x]; q != u && q != v)
                        across.push_back(q);
        std::vector<Index>&nu = mNu, &nv = mNv;
        neighbours(u, nu);
        neighbours(v, nv);
        for (Index q : nu)
            if (q != v && std::find(nv.begin(), nv.end(), q) != nv.end() && std::find(across.begin(), across.end(), q) == across.end())
                return false;
        if (across.empty())
            return false;
        for (Index t : mPointTris[u])
        {
            if (slot(t, v) >= 0)
                continue;
            std::array<Index, 3> moved = mTris[t];
            const int su = slot(t, u);
            moved[su] = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == moved[su]; })->second;
            const glm::dvec3 before = triNormal(mTris[t]), after = triNormal(moved);
            const double la = glm::length(after);
            if (la <= mMinArea || glm::dot(after, before) <= 0.2 * la * glm::length(before) || glm::dot(after, mStartNormal[t]) <= 0.0)
                return false;
            // Against the source's smooth normals too: a triangle that started out backwards (a folded quad) mustn't spread.
            const glm::dvec3 around = mSmooth[mW.point[moved[0]]] + mSmooth[mW.point[moved[1]]] + mSmooth[mW.point[moved[2]]];
            if (glm::dot(after, around) < -0.3 * la * glm::length(around))
                return false;
            const double uvBefore = uvArea(mTris[t]), uvAfter = uvArea(moved);
            if (uvBefore != 0.0 && (uvAfter == 0.0 || (uvAfter > 0.0) != (uvBefore > 0.0)))
                return false;
        }
        return true;
    }

    void collapse(Index u, Index v)
    {
        std::vector<std::pair<Index, Index>> map;
        wedgeMap(u, v, map);
        for (Index t : mPointTris[u])
        {
            if (slot(t, v) >= 0)
            {
                mAlive[t] = 0;
                --mAliveCount;
                for (Index x : mTris[t])
                    if (const Index q = mW.point[x]; q != u)
                        std::erase(mPointTris[q], t);
                continue;
            }
            const int su = slot(t, u);
            mTris[t][su] = std::find_if(map.begin(), map.end(), [&](const auto& e) { return e.first == mTris[t][su]; })->second;
            mPointTris[v].push_back(t);
        }
        mPointTris[u].clear();
        mQ[v].add(mQ[u]);
    }

    const Mesh& mMesh;
    const Wedges& mW;
    std::span<const glm::vec2> mUv;
    const std::vector<uint8_t>& mLocked;
    bool mKeepBorders;
    std::vector<std::array<Index, 3>> mTris;
    std::vector<uint8_t> mAlive;
    std::vector<glm::dvec3> mStartNormal;
    std::vector<glm::dvec3> mSmooth;
    // Scratch, so the hot checks don't allocate.
    mutable std::vector<Index> mNb, mNu, mNv, mAcross;
    mutable std::vector<std::pair<Index, Index>> mMap;
    mutable std::vector<std::pair<double, Index>> mCandidates;
    std::vector<std::vector<Index>> mPointTris;
    std::vector<Quadric> mQ;
    std::vector<uint32_t> mVersion;
    Index mAliveCount = 0;
    double mMinArea = 0.0;
    double mMaxCost = 0.0;
};

/** Seam flags as point pairs, which survive the faces being rebuilt. */
std::set<std::pair<Index, Index>> seamPairs(const Mesh& m)
{
    std::set<std::pair<Index, Index>> out;
    std::span<const bool> seam = m.edgeAttrs().get<bool>(attr::kSeam);
    for (Index e = 0; e < Index(seam.size()); ++e)
        if (m.edgeValid(e) && seam[e])
            out.insert(std::minmax(m.edgePoint(e, 0), m.edgePoint(e, 1)));
    return out;
}

/** Marks edges where the two faces' corners differ: hard where the smoothing fans part, seam where the uvs do. */
void markSplits(Mesh& m, const std::vector<Index>& fanOfCorner, bool hardFromFans, bool hadSeams)
{
    std::span<bool> hard = hardFromFans ? m.edgeAttrs().getOrAdd<bool>(attr::kHard) : std::span<bool>{};
    std::vector<std::span<const glm::vec2>> uvs;
    for (int i = 0; i < 8; ++i)
        if (auto uv = std::as_const(m).cornerAttrs().get<glm::vec2>(attr::uvName(i)); !uv.empty())
            uvs.push_back(uv);
    std::span<bool> seam = hadSeams ? m.edgeAttrs().getOrAdd<bool>(attr::kSeam) : std::span<bool>{};
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || !m.isManifoldEdge(e))
            continue;
        const Index c = m.edgeCorner(e), o = m.cornerRadial(c);
        const Index cn = m.cornerNext(c);
        const Index oAtC = m.cornerPoint(o) == m.cornerPoint(c) ? o : m.cornerNext(o);
        const Index oAtN = oAtC == o ? m.cornerNext(o) : o;
        if (!hard.empty())
            hard[e] = fanOfCorner[c] != fanOfCorner[oAtC] && fanOfCorner[cn] != fanOfCorner[oAtN];
        if (!seam.empty())
            for (const auto& uv : uvs)
                seam[e] = seam[e] || uv[c] != uv[oAtC] || uv[cn] != uv[oAtN];
    }
}

/** True when no face has points strictly on both sides of the plane. */
bool splitsCleanly(const Mesh& m, int axis, double tol)
{
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        bool neg = false, pos = false;
        m.forEachFaceCorner(f, [&](Index c) {
            const double x = m.position(m.cornerPoint(c))[axis];
            neg = neg || x < -tol;
            pos = pos || x > tol;
        });
        if (neg && pos)
            return false;
    }
    return true;
}

void joinQuads(Mesh& m)
{
    // Quads never join across a uv split, so the seam flags must cover every one while they run.
    const bool seamLayer = m.edgeAttrs().has(attr::kSeam);
    const std::set<std::pair<Index, Index>> userSeams = seamPairs(m);
    for (int i = 0; i < 8; ++i)
        if (m.cornerAttrs().has(attr::uvName(i)))
            markUvSeams(m, attr::uvName(i));
    QuadrangulateOptions qo;
    qo.maxFaceAngle = 30.0;
    quadrangulateFaces(m, liveFaces(m), qo);
    if (!seamLayer)
        m.edgeAttrs().remove(attr::kSeam);
    else
    {
        std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(attr::kSeam);
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e))
                seam[e] = userSeams.count(std::minmax(m.edgePoint(e, 0), m.edgePoint(e, 1))) > 0;
    }
    m.compact();
}
} // namespace

ReduceResult reduceMesh(const Mesh& mesh, const ReduceOptions& opts)
{
    LILA_ZONE();

    ReduceResult r;
    Mesh& m = r.mesh;
    m = mesh;
    m.compact();
    const double tol = std::max(0.0, opts.symmetryTolerance);
    bool symmetric = opts.symmetryAxis >= 0 && opts.symmetryAxis < 3;
    // The halves are found and mirrored about the origin, so the plane is moved there for the run.
    const double shift = symmetric ? opts.symmetryOffset : 0.0;
    auto shiftBy = [&](double d) {
        if (d != 0.0)
            for (glm::dvec3& p : m.positions())
                p[opts.symmetryAxis] += d;
    };
    shiftBy(-shift);
    if (symmetric && !splitsCleanly(m, opts.symmetryAxis, tol))
    {
        symmetric = false;
        r.symmetryIgnored = true;
    }
    if (symmetric)
    {
        std::vector<Index> negative;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            bool neg = false;
            m.forEachFaceCorner(f, [&](Index c) { neg = neg || m.position(m.cornerPoint(c))[opts.symmetryAxis] < -tol; });
            if (neg)
                negative.push_back(f);
        }
        deleteFaces(m, negative);
        m.compact();
    }

    Index sourceTris = 0;
    for (Index f : liveFaces(m))
        sourceTris += m.faceSize(f) - 2;
    triangulateFaces(m, liveFaces(m));
    m.compact();
    const bool hadSeams = m.edgeAttrs().has(attr::kSeam);
    const std::set<std::pair<Index, Index>> seamsBefore = seamPairs(m);

    const Wedges w = buildWedges(m, opts.keepHardEdges);
    std::vector<uint8_t> locked(m.pointCapacity(), 0);
    if (symmetric)
        for (Index p = 0; p < m.pointCapacity(); ++p)
            locked[p] = m.pointValid(p) && std::abs(m.position(p)[opts.symmetryAxis]) <= tol;

    const double wanted = opts.targetTriangles > 0 ? double(opts.targetTriangles) * (symmetric ? 0.5 : 1.0)
                                                   : double(sourceTris) * std::clamp(opts.ratio, 0.0, 1.0);
    Collapser collapser(m, w, m.cornerAttrs().get<glm::vec2>(attr::uvName(0)), locked, opts.keepBorders);
    collapser.run(Index(std::max(1.0, std::round(wanted))));
    const double size = std::max(1e-300, glm::length(m.bounds().size()));
    r.error = std::sqrt(std::max(0.0, collapser.maxCost())) / size;
    const std::vector<std::array<Index, 3>> tris = collapser.triangles();

    for (Index f : liveFaces(m))
        m.removeFace(f);
    std::vector<Index> fanOfCorner(m.cornerCapacity() + tris.size() * 3, kInvalid);
    for (const auto& t : tris)
    {
        const Index pts[3] = {w.point[t[0]], w.point[t[1]], w.point[t[2]]};
        const Index src[3] = {w.corner[t[0]], w.corner[t[1]], w.corner[t[2]]};
        const Index nf = m.addFace(pts, m.cornerFace(src[0]), src);
        if (nf == kInvalid)
            continue;
        int k = 0;
        m.forEachFaceCorner(nf, [&](Index c) {
            if (c >= fanOfCorner.size())
                fanOfCorner.resize(c + 1, kInvalid);
            fanOfCorner[c] = w.fan[t[k++]];
        });
    }
    fanOfCorner.resize(m.cornerCapacity(), kInvalid);
    markSplits(m, fanOfCorner, opts.keepHardEdges, hadSeams);
    if (hadSeams)
    {
        std::span<bool> seam = m.edgeAttrs().getOrAdd<bool>(attr::kSeam);
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && seamsBefore.count(std::minmax(m.edgePoint(e, 0), m.edgePoint(e, 1))))
                seam[e] = true;
    }
    m.removeAllLoose();
    m.compact();
    if (opts.quads)
        joinQuads(m);

    if (symmetric)
    {
        MirrorOptions mo;
        mo.plane = axisPlane(opts.symmetryAxis);
        mo.weld = true;
        mo.tolerance = std::max(tol, 1e-9);
        const Index firstNew = m.pointCapacity();
        mirrorMesh(m, mo);
        clearSymmetryKeys(m, firstNew);
        m.compact();
    }
    shiftBy(shift);
    return r;
}
} // namespace lila::geo
