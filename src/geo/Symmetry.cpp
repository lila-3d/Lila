#include "geo/Symmetry.h"

#include "geo/Bvh.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>

namespace lila::geo
{
namespace
{
struct CellHash
{
    size_t operator()(const glm::i64vec3& c) const
    {
        return size_t(c.x * 73856093ll ^ c.y * 19349663ll ^ c.z * 83492791ll);
    }
};

int planeAxis(const SymmetryPlane& plane)
{
    for (int k = 0; k < 3; ++k)
        if (plane.normal[k] == 1.0 && plane.normal[(k + 1) % 3] == 0.0 && plane.normal[(k + 2) % 3] == 0.0)
            return k;
    return -1;
}

/** Sides for a map whose pairs are known: the partner further along the normal is the positive one. */
std::vector<int8_t> sidesFromPositions(const Mesh& mesh, const SymmetryPlane& plane, const std::vector<Index>& mirror)
{
    std::vector<int8_t> side(mirror.size(), 0);
    for (Index p = 0; p < Index(mirror.size()); ++p)
    {
        const Index q = mirror[p];
        if (q == kInvalid || q == p || !mesh.pointValid(p) || !mesh.pointValid(q))
            continue;
        const double dp = plane.distance(mesh.position(p)), dq = plane.distance(mesh.position(q));
        side[p] = dp > dq || (dp == dq && p < q) ? 1 : -1;
    }
    return side;
}
} // namespace

SymmetryPlane axisPlane(int axis, double offset)
{
    SymmetryPlane plane;
    plane.normal = glm::dvec3(0.0);
    plane.normal[std::clamp(axis, 0, 2)] = 1.0;
    plane.offset = offset;
    return plane;
}

std::string symmetryLayerName(int axis)
{
    return std::string("symMirror") + char('X' + std::clamp(axis, 0, 2));
}

void clearSymmetryKeys(Mesh& mesh, Index firstPoint)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        std::span<int32_t> keys = mesh.pointAttrs().get<int32_t>(symmetryLayerName(axis));
        for (Index p = firstPoint; p < keys.size(); ++p)
            keys[p] = 0;
    }
}

SymmetryMap SymmetryMap::build(const Mesh& mesh, const SymmetryPlane& plane, double tolerance)
{
    LILA_ZONE();

    SymmetryMap map;
    map.mPlane = plane;
    map.mPlane.normal = glm::normalize(plane.normal);
    map.mTolerance = tolerance;
    const Index pc = mesh.pointCapacity();
    map.mMirror.assign(pc, kInvalid);
    map.mOnPlane.assign(pc, 0);

    const double cell = std::max(tolerance * 2.0, 1e-9);
    auto key = [&](const glm::dvec3& p) { return glm::i64vec3(glm::floor(p / cell)); };
    std::unordered_map<glm::i64vec3, std::vector<Index>, CellHash> grid;
    grid.reserve(mesh.numPoints());
    for (Index p = 0; p < pc; ++p)
        if (mesh.pointValid(p))
            grid[key(mesh.position(p))].push_back(p);

    tbb::parallel_for(tbb::blocked_range<Index>(0, pc, 4096), [&](const tbb::blocked_range<Index>& r) {
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!mesh.pointValid(p))
                continue;
            const glm::dvec3 pos = mesh.position(p);
            if (std::abs(map.mPlane.distance(pos)) <= tolerance)
            {
                map.mOnPlane[p] = 1;
                map.mMirror[p] = p;
                continue;
            }
            const glm::dvec3 target = map.mPlane.reflect(pos);
            const glm::i64vec3 k = key(target);
            double best = tolerance * tolerance;
            Index found = kInvalid;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        auto it = grid.find(k + glm::i64vec3(dx, dy, dz));
                        if (it == grid.end())
                            continue;
                        for (Index q : it->second)
                        {
                            const glm::dvec3 d = mesh.position(q) - target;
                            const double d2 = glm::dot(d, d);
                            if (d2 <= best && (d2 < best || q < found))
                            {
                                best = d2;
                                found = q;
                            }
                        }
                    }
            map.mMirror[p] = found;
        }
    });
    for (Index p = 0; p < pc; ++p)
        if (mesh.pointValid(p) && map.mMirror[p] == kInvalid)
            ++map.mUnmatched;
    map.mAxis = planeAxis(map.mPlane);
    map.mSide = sidesFromPositions(mesh, map.mPlane, map.mMirror);
    return map;
}

SymmetryMap SymmetryMap::buildTopological(const Mesh& mesh, Index seedEdge, const SymmetryPlane& plane)
{
    LILA_ZONE();

    SymmetryMap map;
    map.mPlane = plane;
    map.mPlane.normal = glm::normalize(plane.normal);
    if (!mesh.edgeValid(seedEdge) || !mesh.isManifoldEdge(seedEdge))
        return map;
    const Index pc = mesh.pointCapacity();
    std::vector<Index> mirror(pc, kInvalid);
    std::vector<Index> faceMirror(mesh.faceCapacity(), kInvalid);

    auto pair = [&](Index a, Index b) {
        if (mirror[a] == kInvalid && mirror[b] == kInvalid)
        {
            mirror[a] = b;
            mirror[b] = a;
            return true;
        }
        return mirror[a] == b && mirror[b] == a;
    };

    // Each entry pairs a corner walked forward with a corner of the mirror face walked backward.
    std::deque<std::pair<Index, Index>> queue;
    const Index r1 = mesh.edgeCorner(seedEdge);
    const Index r2 = mesh.cornerRadial(r1);
    if (mesh.cornerPoint(r1) == mesh.cornerPoint(r2))
        return map;
    queue.push_back({r1, mesh.cornerNext(r2)});
    bool ok = true;
    while (!queue.empty() && ok)
    {
        const auto [c0, d0] = queue.front();
        queue.pop_front();
        const Index f = mesh.cornerFace(c0);
        const Index g = mesh.cornerFace(d0);
        if (faceMirror[f] != kInvalid)
        {
            ok = faceMirror[f] == g;
            continue;
        }
        if (mesh.faceSize(f) != mesh.faceSize(g) || faceMirror[g] != kInvalid)
        {
            ok = false;
            break;
        }
        faceMirror[f] = g;
        faceMirror[g] = f;
        Index c = c0, d = d0;
        for (Index i = 0; i < mesh.faceSize(f); ++i)
        {
            if (!pair(mesh.cornerPoint(c), mesh.cornerPoint(d)))
            {
                ok = false;
                break;
            }
            const Index dn = mesh.cornerPrev(d);
            const Index e = mesh.cornerEdge(c);
            const Index em = mesh.cornerEdge(dn);
            if (mesh.isManifoldEdge(e) && mesh.isManifoldEdge(em))
            {
                const Index r = mesh.cornerRadial(c);
                const Index rm = mesh.cornerRadial(dn);
                if (faceMirror[mesh.cornerFace(r)] == kInvalid)
                    queue.push_back({r, mesh.cornerNext(rm)});
            }
            c = mesh.cornerNext(c);
            d = dn;
        }
    }
    if (!ok)
        return map;

    map.mMirror = std::move(mirror);
    map.mOnPlane.assign(pc, 0);
    for (Index p = 0; p < pc; ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        if (map.mMirror[p] == p)
            map.mOnPlane[p] = 1;
        else if (map.mMirror[p] == kInvalid)
            ++map.mUnmatched;
    }
    map.mAxis = planeAxis(map.mPlane);
    map.mSide = sidesFromPositions(mesh, map.mPlane, map.mMirror);
    return map;
}

Index SymmetryMap::mirrorEdge(const Mesh& mesh, Index e) const
{
    if (!mesh.edgeValid(e))
        return kInvalid;
    const Index a = mirrorPoint(mesh.edgePoint(e, 0));
    const Index b = mirrorPoint(mesh.edgePoint(e, 1));
    return a == kInvalid || b == kInvalid ? kInvalid : mesh.findEdge(a, b);
}

Index SymmetryMap::mirrorFace(const Mesh& mesh, Index f) const
{
    if (!mesh.faceValid(f))
        return kInvalid;
    const Index c = mesh.faceCorner(f);
    const Index e = mirrorEdge(mesh, mesh.cornerEdge(c));
    if (e == kInvalid)
        return kInvalid;
    Index found = kInvalid;
    mesh.forEachEdgeCorner(e, [&](Index r) {
        const Index g = mesh.cornerFace(r);
        if (found != kInvalid || mesh.faceSize(g) != mesh.faceSize(f))
            return;
        bool all = true;
        mesh.forEachFaceCorner(f, [&](Index fc) {
            const Index m = mirrorPoint(mesh.cornerPoint(fc));
            all &= m != kInvalid && mesh.findCorner(g, m) != kInvalid;
        });
        if (all)
            found = g;
    });
    return found;
}

std::vector<Index> SymmetryMap::mirrorPoints(std::span<const Index> points) const
{
    std::vector<Index> out;
    for (Index p : points)
        out.push_back(mirrorPoint(p));
    return out;
}

std::vector<Index> SymmetryMap::mirrorEdges(const Mesh& mesh, std::span<const Index> edges) const
{
    std::vector<Index> out;
    for (Index e : edges)
        out.push_back(mirrorEdge(mesh, e));
    return out;
}

std::vector<Index> SymmetryMap::mirrorFaces(const Mesh& mesh, std::span<const Index> faces) const
{
    std::vector<Index> out;
    for (Index f : faces)
        out.push_back(mirrorFace(mesh, f));
    return out;
}

namespace
{
std::vector<Index> merged(std::span<const Index> a, const std::vector<Index>& b)
{
    std::vector<Index> out(a.begin(), a.end());
    for (Index i : b)
        if (i != kInvalid)
            out.push_back(i);
    return uniqueSorted(out);
}
} // namespace

std::vector<Index> SymmetryMap::expandPoints(std::span<const Index> points) const
{
    return merged(points, mirrorPoints(points));
}

std::vector<Index> SymmetryMap::expandEdges(const Mesh& mesh, std::span<const Index> edges) const
{
    return merged(edges, mirrorEdges(mesh, edges));
}

std::vector<Index> SymmetryMap::expandFaces(const Mesh& mesh, std::span<const Index> faces) const
{
    return merged(faces, mirrorFaces(mesh, faces));
}

void SymmetryMap::snapToPlane(Mesh& mesh, std::span<const Index> points) const
{
    for (Index p : points)
        if (mesh.pointValid(p) && std::abs(mPlane.distance(mesh.position(p))) <= mTolerance)
            mesh.setPosition(p, mPlane.project(mesh.position(p)));
}

void SymmetryMap::snapNewPoints(Mesh& mesh, Index firstPoint) const
{
    for (Index p = firstPoint; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && std::abs(mPlane.distance(mesh.position(p))) <= mTolerance)
            mesh.setPosition(p, mPlane.project(mesh.position(p)));
}

void applySymmetrySnap(Mesh& mesh, const SymmetryMap* symmetry, Index firstNewPoint)
{
    if (!symmetry)
        return;
    LILA_ZONE();

    std::vector<Index> onPlane;
    for (Index p = 0; p < std::min(firstNewPoint, mesh.pointCapacity()); ++p)
        if (symmetry->onPlane(p))
            onPlane.push_back(p);
    symmetry->snapToPlane(mesh, onPlane);
    symmetry->snapNewPoints(mesh, firstNewPoint);
}
glm::dvec3 SymmetryMap::reflected(const glm::dvec3& p) const
{
    if (mAxis < 0)
        return mPlane.reflect(p);
    glm::dvec3 r = p;
    r[mAxis] = 2.0 * mPlane.offset - r[mAxis];
    return r;
}

int SymmetryMap::side(Index p) const
{
    return p < mSide.size() ? mSide[p] : 0;
}

int SymmetryMap::majoritySide(std::span<const Index> points) const
{
    int64_t sum = 0;
    for (Index p : points)
        sum += side(p);
    return sum < 0 ? -1 : 1;
}

namespace
{
bool adjacent(const Mesh& mesh, Index a, Index b)
{
    return a == b || mesh.findEdge(a, b) != kInvalid;
}

std::vector<Index> neighbours(const Mesh& mesh, Index p)
{
    std::vector<Index> out;
    mesh.forEachPointEdge(p, [&](Index e) { out.push_back(mesh.edgeOtherPoint(e, p)); });
    return out;
}
} // namespace

SymmetryMap SymmetryMap::persistent(Mesh& mesh, int axis, double tolerance, SymmetrySync* sync, double offset)
{
    LILA_ZONE();

    SymmetryMap map;
    map.mAxis = std::clamp(axis, 0, 2);
    map.mPlane = axisPlane(map.mAxis, offset);
    map.mTolerance = tolerance;
    const Index pc = mesh.pointCapacity();
    map.mMirror.assign(pc, kInvalid);
    map.mOnPlane.assign(pc, 0);
    map.mSide.assign(pc, 0);
    SymmetrySync info;

    std::span<int32_t> keys = mesh.pointAttrs().getOrAdd<int32_t>(symmetryLayerName(map.mAxis));
    auto pos = [&](Index p) { return mesh.position(p); };
    auto pair = [&](Index a, Index b) {
        map.mMirror[a] = b;
        map.mMirror[b] = a;
    };
    auto setOnPlane = [&](Index p) {
        map.mMirror[p] = p;
        map.mOnPlane[p] = 1;
    };

    // A key held by several points on a side (an op copied it onto new points) pairs them by nearest reflection.
    struct Group
    {
        std::vector<Index> pos, neg;
    };
    std::unordered_map<int32_t, Group> groups;
    for (Index p = 0; p < pc; ++p)
    {
        if (!mesh.pointValid(p) || keys[p] == 0)
            continue;
        if (keys[p] == kSymmetryOnPlane)
        {
            setOnPlane(p);
            ++info.stored;
        }
        else if (keys[p] > 0)
            groups[keys[p]].pos.push_back(p);
        else
            groups[-keys[p]].neg.push_back(p);
    }
    for (auto& [key, g] : groups)
    {
        std::vector<uint8_t> used(g.neg.size(), 0);
        for (Index a : g.pos)
        {
            double best = std::numeric_limits<double>::max();
            size_t found = g.neg.size();
            for (size_t i = 0; i < g.neg.size(); ++i)
            {
                const glm::dvec3 d = map.reflected(pos(a)) - pos(g.neg[i]);
                if (!used[i] && (found == g.neg.size() || glm::dot(d, d) < best))
                {
                    best = glm::dot(d, d);
                    found = i;
                }
            }
            if (found == g.neg.size())
                continue;
            used[found] = 1;
            pair(a, g.neg[found]);
            map.mSide[a] = 1;
            map.mSide[g.neg[found]] = -1;
            info.stored += 2;
        }
    }

    // The rest are new or orphaned, so whatever key they hold means nothing any more.
    std::vector<Index> fresh;
    for (Index p = 0; p < pc; ++p)
        if (mesh.pointValid(p) && map.mMirror[p] == kInvalid)
        {
            keys[p] = 0;
            fresh.push_back(p);
        }

    // A point's partner neighbours the partners of its paired neighbours. Ops build both halves together, so this
    // finds new points' partners however far the old ones have drifted.
    auto matchTopologically = [&]() {
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (Index a : fresh)
            {
                if (map.mMirror[a] != kInvalid)
                    continue;
                std::vector<Index> anchors;
                for (Index n : neighbours(mesh, a))
                    if (map.mMirror[n] != kInvalid)
                        anchors.push_back(map.mMirror[n]);
                if (anchors.empty())
                    continue;
                std::vector<Index> candidates = neighbours(mesh, anchors[0]);
                candidates.push_back(a);
                const glm::dvec3 target = map.reflected(pos(a));
                double best = std::numeric_limits<double>::max();
                Index found = kInvalid;
                for (Index b : candidates)
                {
                    if (map.mMirror[b] != kInvalid)
                        continue;
                    bool ok = true;
                    if (b == a)
                        ok = std::abs(map.mPlane.distance(pos(a))) <= map.mTolerance;
                    for (Index m : anchors)
                        ok = ok && (b == a || m != b) && adjacent(mesh, b, m);
                    const glm::dvec3 d = pos(b) - target;
                    if (ok && glm::dot(d, d) < best)
                    {
                        best = glm::dot(d, d);
                        found = b;
                    }
                }
                if (found == kInvalid)
                    continue;
                if (found == a)
                    setOnPlane(a);
                else
                    pair(a, found);
                info.topological += found == a ? 1 : 2;
                changed = true;
            }
        }
    };

    auto matchByPosition = [&]() {
        const double tol = std::max(map.mTolerance, 1e-12);
        const double cell = tol * 2.0;
        auto key = [&](const glm::dvec3& p) { return glm::i64vec3(glm::floor(p / cell)); };
        std::unordered_map<glm::i64vec3, std::vector<Index>, CellHash> grid;
        for (Index p : fresh)
            if (map.mMirror[p] == kInvalid)
                grid[key(pos(p))].push_back(p);
        for (Index a : fresh)
        {
            if (map.mMirror[a] != kInvalid)
                continue;
            if (std::abs(map.mPlane.distance(pos(a))) <= tol)
            {
                setOnPlane(a);
                ++info.byPosition;
                continue;
            }
            const glm::dvec3 target = map.reflected(pos(a));
            const glm::i64vec3 k = key(target);
            double best = tol * tol;
            Index found = kInvalid;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        auto it = grid.find(k + glm::i64vec3(dx, dy, dz));
                        if (it == grid.end())
                            continue;
                        for (Index b : it->second)
                        {
                            if (b == a || map.mMirror[b] != kInvalid)
                                continue;
                            const glm::dvec3 d = pos(b) - target;
                            if (glm::dot(d, d) <= best)
                            {
                                best = glm::dot(d, d);
                                found = b;
                            }
                        }
                    }
            if (found != kInvalid)
            {
                pair(a, found);
                info.byPosition += 2;
            }
        }
    };

    if (!fresh.empty())
    {
        matchTopologically();
        matchByPosition();
        matchTopologically();

        for (Index a : fresh)
        {
            const Index b = map.mMirror[a];
            if (b == kInvalid || b == a || map.mSide[a] != 0)
                continue;
            const double da = map.mPlane.distance(pos(a)), db = map.mPlane.distance(pos(b));
            const Index plus = da > db || (da == db && a < b) ? a : b;
            map.mSide[plus] = 1;
            map.mSide[plus == a ? b : a] = -1;
        }

        int64_t next = 1;
        for (Index p = 0; p < pc; ++p)
            if (mesh.pointValid(p) && keys[p] != 0 && keys[p] != kSymmetryOnPlane)
                next = std::max<int64_t>(next, int64_t(std::abs(keys[p])) + 1);
        if (next + int64_t(fresh.size()) >= int64_t(INT32_MAX))
            map.store(mesh, map.mAxis); // out of keys: renumber densely
        else
            for (Index a : fresh)
            {
                const Index b = map.mMirror[a];
                if (b == a)
                    keys[a] = kSymmetryOnPlane;
                else if (b != kInvalid && map.mSide[a] > 0)
                {
                    keys[a] = int32_t(next);
                    keys[b] = int32_t(-next);
                    ++next;
                }
            }
    }
    for (Index p = 0; p < pc; ++p)
        if (mesh.pointValid(p) && map.mMirror[p] == kInvalid)
            ++map.mUnmatched;
    info.unmatched = map.mUnmatched;
    if (sync)
        *sync = info;
    return map;
}

void SymmetryMap::store(Mesh& mesh, int axis) const
{
    std::span<int32_t> keys = mesh.pointAttrs().getOrAdd<int32_t>(symmetryLayerName(axis));
    const std::vector<int8_t> sides = mSide.empty() ? sidesFromPositions(mesh, mPlane, mMirror) : mSide;
    int32_t next = 1;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        const Index q = mirrorPoint(p);
        if (q == kInvalid || !mesh.pointValid(q) || mirrorPoint(q) != p)
            keys[p] = 0;
        else if (q == p)
            keys[p] = kSymmetryOnPlane;
        else if (p < q)
        {
            const bool plus = p >= sides.size() || sides[p] >= 0;
            keys[p] = plus ? next : -next;
            keys[q] = plus ? -next : next;
            ++next;
        }
    }
}

void SymmetryMap::enforce(Mesh& mesh, std::span<const Index> points, int drive) const
{
    LILA_ZONE();

    drive = drive < 0 ? -1 : 1;
    for (Index p : points)
    {
        if (!mesh.pointValid(p))
            continue;
        const Index q = mirrorPoint(p);
        if (q == kInvalid || !mesh.pointValid(q))
            continue;
        if (q == p)
        {
            glm::dvec3 v = mesh.position(p);
            if (mAxis >= 0)
                v[mAxis] = mPlane.offset;
            else
                v = mPlane.project(v);
            mesh.setPosition(p, v);
            continue;
        }
        const bool pDrives = side(p) == drive || (side(p) == 0 && p < q);
        mesh.setPosition(pDrives ? q : p, reflected(mesh.position(pDrives ? p : q)));
    }
}

void SymmetryMap::symmetrize(Mesh& mesh, int drive) const
{
    std::vector<Index> all;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            all.push_back(p);
    enforce(mesh, all, drive);
}

SymmetryCheck SymmetryMap::check(const Mesh& mesh) const
{
    LILA_ZONE();

    SymmetryCheck out;
    std::vector<Index> unmatched;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        const Index q = mirrorPoint(p);
        if (q == kInvalid || !mesh.pointValid(q))
        {
            ++out.unmatched;
            unmatched.push_back(p);
            continue;
        }
        double err = 0.0;
        if (q == p)
        {
            ++out.onPlane;
            err = std::abs(mPlane.distance(mesh.position(p)));
        }
        else
        {
            out.pairs += p < q;
            err = glm::length(reflected(mesh.position(q)) - mesh.position(p));
        }
        if (err > out.maxAsymmetry)
        {
            out.maxAsymmetry = err;
            out.worstPoint = p;
        }
    }
    // A point without a partner is off by how far its reflection lies from the surface.
    if (!unmatched.empty() && mesh.numFaces() > 0)
    {
        const MeshBvh bvh(mesh);
        for (Index p : unmatched)
        {
            BvhClosest hit;
            if (bvh.closestPoint(reflected(mesh.position(p)), hit) && hit.distance > out.maxAsymmetry)
            {
                out.maxAsymmetry = hit.distance;
                out.worstPoint = p;
            }
        }
    }
    return out;
}

} // namespace lila::geo
