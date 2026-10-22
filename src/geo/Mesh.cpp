#include "geo/Mesh.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <format>

namespace lila::geo
{
namespace
{
bool hasRepeats(std::span<const Index> pts)
{
    const size_t n = pts.size();
    if (n <= 16)
    {
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j)
                if (pts[i] == pts[j])
                    return true;
        return false;
    }
    std::vector<Index> sorted(pts.begin(), pts.end());
    std::sort(sorted.begin(), sorted.end());
    return std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
}
} // namespace

bool Mesh::hasDeleted() const
{
    return mNumPoints != pointCapacity() || mNumEdges != edgeCapacity() || mNumFaces != faceCapacity()
        || mNumCorners != cornerCapacity();
}

size_t Mesh::memoryBytes() const
{
    size_t n = sizeof(Mesh) + mPointPos.capacity() * sizeof(glm::dvec3) + mPointEdge.capacity() * sizeof(Index)
        + mPointDead.capacity() + mEdges.capacity() * sizeof(Edge) + mCorners.capacity() * sizeof(Corner)
        + mFaces.capacity() * sizeof(Face);
    for (const AttributeSet& set : mAttrs)
        for (size_t i = 0; i < set.layerCount(); ++i)
            n += set.layer(i).size() * set.layer(i).stride();
    for (const std::string& m : mMaterialNames)
        n += m.capacity();
    return n;
}

void Mesh::reserve(size_t points, size_t edges, size_t faces, size_t corners)
{
    mPointPos.reserve(points);
    mPointEdge.reserve(points);
    mPointDead.reserve(points);
    mEdges.reserve(edges);
    mFaces.reserve(faces);
    mCorners.reserve(corners);
}

// Topology primitives

void Mesh::diskInsert(Index e, Index p)
{
    Edge& ed = mEdges[e];
    const int s = side(e, p);
    const Index first = mPointEdge[p];
    if (first == kInvalid)
    {
        ed.next[s] = ed.prev[s] = e;
        mPointEdge[p] = e;
        return;
    }
    const int fs = side(first, p);
    const Index after = mEdges[first].next[fs];
    ed.prev[s] = first;
    ed.next[s] = after;
    mEdges[first].next[fs] = e;
    mEdges[after].prev[side(after, p)] = e;
}

void Mesh::diskRemove(Index e, Index p)
{
    Edge& ed = mEdges[e];
    const int s = side(e, p);
    const Index next = ed.next[s];
    const Index prev = ed.prev[s];
    if (next == e)
    {
        mPointEdge[p] = kInvalid;
    }
    else
    {
        mEdges[next].prev[side(next, p)] = prev;
        mEdges[prev].next[side(prev, p)] = next;
        if (mPointEdge[p] == e)
            mPointEdge[p] = next;
    }
    ed.next[s] = ed.prev[s] = kInvalid;
}

void Mesh::radialInsert(Index c, Index e)
{
    Corner& cr = mCorners[c];
    cr.edge = e;
    const Index first = mEdges[e].corner;
    if (first == kInvalid)
    {
        cr.radialNext = cr.radialPrev = c;
        mEdges[e].corner = c;
        return;
    }
    const Index after = mCorners[first].radialNext;
    cr.radialPrev = first;
    cr.radialNext = after;
    mCorners[first].radialNext = c;
    mCorners[after].radialPrev = c;
}

void Mesh::radialRemove(Index c)
{
    Corner& cr = mCorners[c];
    const Index e = cr.edge;
    if (cr.radialNext == c)
    {
        mEdges[e].corner = kInvalid;
    }
    else
    {
        mCorners[cr.radialNext].radialPrev = cr.radialPrev;
        mCorners[cr.radialPrev].radialNext = cr.radialNext;
        if (mEdges[e].corner == c)
            mEdges[e].corner = cr.radialNext;
    }
    cr.radialNext = cr.radialPrev = kInvalid;
}

void Mesh::killCorner(Index c)
{
    mCorners[c] = {kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid, kInvalid};
    --mNumCorners;
}

Index Mesh::newEdge(Index a, Index b)
{
    const Index e = Index(mEdges.size());
    mEdges.push_back({{a, b}, {kInvalid, kInvalid}, {kInvalid, kInvalid}, kInvalid});
    edgeAttrs().resize(mEdges.size());
    diskInsert(e, a);
    diskInsert(e, b);
    ++mNumEdges;
    return e;
}

Index Mesh::newCorner(Index point, Index face)
{
    const Index c = Index(mCorners.size());
    mCorners.push_back({point, kInvalid, face, kInvalid, kInvalid, kInvalid, kInvalid});
    ++mNumCorners;
    return c;
}

// Queries

Index Mesh::findEdge(Index a, Index b) const
{
    const Index first = mPointEdge[a];
    if (first == kInvalid || a == b)
        return kInvalid;
    Index e = first;
    do
    {
        const Edge& ed = mEdges[e];
        if (ed.v[0] == b || ed.v[1] == b)
            return e;
        e = ed.next[ed.v[0] == a ? 0 : 1];
    } while (e != first);
    return kInvalid;
}

Index Mesh::findCorner(Index f, Index p) const
{
    Index found = kInvalid;
    forEachFaceCorner(f, [&](Index c) {
        if (mCorners[c].point == p)
            found = c;
    });
    return found;
}

Index Mesh::edgeFaceCount(Index e) const
{
    Index n = 0;
    forEachEdgeCorner(e, [&](Index) { ++n; });
    return n;
}

bool Mesh::isBoundaryEdge(Index e) const
{
    const Index c = mEdges[e].corner;
    return c != kInvalid && mCorners[c].radialNext == c;
}

bool Mesh::isManifoldEdge(Index e) const
{
    const Index c = mEdges[e].corner;
    if (c == kInvalid)
        return false;
    const Index r = mCorners[c].radialNext;
    return r != c && mCorners[r].radialNext == c;
}

bool Mesh::isBoundaryPoint(Index p) const
{
    bool boundary = false;
    forEachPointEdge(p, [&](Index e) {
        if (!isManifoldEdge(e))
            boundary = true;
    });
    return boundary;
}

Index Mesh::pointValence(Index p) const
{
    Index n = 0;
    forEachPointEdge(p, [&](Index) { ++n; });
    return n;
}

Index Mesh::oppositeFace(Index e, Index f) const
{
    if (!isManifoldEdge(e))
        return kInvalid;
    const Index c = mEdges[e].corner;
    const Index r = mCorners[c].radialNext;
    if (mCorners[c].face == f)
        return mCorners[r].face;
    if (mCorners[r].face == f)
        return mCorners[c].face;
    return kInvalid;
}

double Mesh::edgeLength(Index e) const
{
    return glm::length(mPointPos[mEdges[e].v[1]] - mPointPos[mEdges[e].v[0]]);
}

glm::dvec3 Mesh::faceCentroid(Index f) const
{
    glm::dvec3 sum(0.0);
    forEachFaceCorner(f, [&](Index c) { sum += mPointPos[mCorners[c].point]; });
    return sum / double(mFaces[f].size);
}

glm::dvec3 Mesh::faceAreaVector(Index f) const
{
    glm::dvec3 n(0.0);
    // Relative to the first point keeps precision for faces far from the origin.
    const glm::dvec3 origin = mPointPos[mCorners[mFaces[f].corner].point];
    forEachFaceCorner(f, [&](Index c) {
        const glm::dvec3 a = mPointPos[mCorners[c].point] - origin;
        const glm::dvec3 b = mPointPos[mCorners[mCorners[c].next].point] - origin;
        n += glm::cross(a, b);
    });
    return n;
}

glm::dvec3 Mesh::faceNormal(Index f) const
{
    const glm::dvec3 n = faceAreaVector(f);
    const double len = glm::length(n);
    return len > 0.0 ? n / len : glm::dvec3(0.0, 1.0, 0.0);
}

double Mesh::faceArea(Index f) const
{
    return glm::length(faceAreaVector(f)) * 0.5;
}

std::vector<Index> Mesh::facePoints(Index f) const
{
    std::vector<Index> out;
    out.reserve(mFaces[f].size);
    forEachFaceCorner(f, [&](Index c) { out.push_back(mCorners[c].point); });
    return out;
}

std::vector<Index> Mesh::faceCorners(Index f) const
{
    std::vector<Index> out;
    out.reserve(mFaces[f].size);
    forEachFaceCorner(f, [&](Index c) { out.push_back(c); });
    return out;
}

std::vector<Index> Mesh::pointFaces(Index p) const
{
    std::vector<Index> out;
    forEachPointCorner(p, [&](Index c) { out.push_back(mCorners[c].face); });
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Bounds Mesh::bounds() const
{
    LILA_ZONE();

    return tbb::parallel_reduce(
        tbb::blocked_range<size_t>(0, mPointPos.size(), 1 << 14), Bounds{},
        [&](const tbb::blocked_range<size_t>& r, Bounds b) {
            for (size_t i = r.begin(); i < r.end(); ++i)
                if (!mPointDead[i])
                    b.extend(mPointPos[i]);
            return b;
        },
        [](Bounds a, const Bounds& b) {
            a.extend(b);
            return a;
        });
}

Bounds Mesh::faceBounds(std::span<const Index> faces) const
{
    Bounds b;
    for (Index f : faces)
        forEachFaceCorner(f, [&](Index c) { b.extend(mPointPos[mCorners[c].point]); });
    return b;
}

// Editing

Index Mesh::addPoint(const glm::dvec3& pos)
{
    const Index p = Index(mPointPos.size());
    mPointPos.push_back(pos);
    mPointEdge.push_back(kInvalid);
    mPointDead.push_back(0);
    pointAttrs().resize(mPointPos.size());
    ++mNumPoints;
    return p;
}

Index Mesh::addEdge(Index a, Index b)
{
    if (a == b)
        return kInvalid;
    const Index e = findEdge(a, b);
    return e != kInvalid ? e : newEdge(a, b);
}

Index Mesh::addFace(std::span<const Index> points)
{
    const size_t n = points.size();
    if (n < 3 || hasRepeats(points))
        return kInvalid;
    for (Index p : points)
        if (!pointValid(p))
            return kInvalid;

    const Index f = Index(mFaces.size());
    const Index first = Index(mCorners.size());
    mFaces.push_back({first, Index(n)});
    for (size_t i = 0; i < n; ++i)
        newCorner(points[i], f);
    for (size_t i = 0; i < n; ++i)
    {
        Corner& c = mCorners[first + i];
        c.next = first + Index((i + 1) % n);
        c.prev = first + Index((i + n - 1) % n);
        radialInsert(first + Index(i), addEdge(points[i], points[(i + 1) % n]));
    }
    cornerAttrs().resize(mCorners.size());
    faceAttrs().resize(mFaces.size());
    ++mNumFaces;
    return f;
}

Index Mesh::addFace(std::span<const Index> points, Index srcFace, std::span<const Index> srcCorners)
{
    const Index f = addFace(points);
    if (f == kInvalid)
        return f;
    if (srcFace != kInvalid)
        faceAttrs().copy(f, srcFace);
    if (!srcCorners.empty())
    {
        const Index first = mFaces[f].corner;
        for (size_t i = 0; i < points.size() && i < srcCorners.size(); ++i)
            if (srcCorners[i] != kInvalid)
                cornerAttrs().copy(first + i, srcCorners[i]);
    }
    return f;
}

void Mesh::removeFace(Index f)
{
    if (!faceValid(f))
        return;
    Index c = mFaces[f].corner;
    for (Index i = 0, n = mFaces[f].size; i < n; ++i)
    {
        const Index next = mCorners[c].next;
        radialRemove(c);
        killCorner(c);
        c = next;
    }
    mFaces[f] = {kInvalid, 0};
    --mNumFaces;
}

void Mesh::removeEdge(Index e)
{
    if (!edgeValid(e))
        return;
    while (mEdges[e].corner != kInvalid)
        removeFace(mCorners[mEdges[e].corner].face);
    diskRemove(e, mEdges[e].v[0]);
    diskRemove(e, mEdges[e].v[1]);
    mEdges[e] = {{kInvalid, kInvalid}, {kInvalid, kInvalid}, {kInvalid, kInvalid}, kInvalid};
    --mNumEdges;
}

void Mesh::removePoint(Index p)
{
    if (!pointValid(p))
        return;
    while (mPointEdge[p] != kInvalid)
        removeEdge(mPointEdge[p]);
    mPointDead[p] = 1;
    --mNumPoints;
}

void Mesh::removeLoose(std::span<const Index> edges, std::span<const Index> points)
{
    for (Index e : edges)
        if (edgeValid(e) && mEdges[e].corner == kInvalid)
            removeEdge(e);
    for (Index p : points)
        if (pointValid(p) && mPointEdge[p] == kInvalid)
            removePoint(p);
}

void Mesh::removeAllLoose()
{
    LILA_ZONE();

    for (Index e = 0; e < edgeCapacity(); ++e)
        if (edgeValid(e) && mEdges[e].corner == kInvalid)
            removeEdge(e);
    for (Index p = 0; p < pointCapacity(); ++p)
        if (pointValid(p) && mPointEdge[p] == kInvalid)
            removePoint(p);
}

Index Mesh::splitEdge(Index e, double t)
{
    const Index a = mEdges[e].v[0];
    const Index b = mEdges[e].v[1];
    const Index m = addPoint(glm::mix(mPointPos[a], mPointPos[b], t));
    {
        const Index src[2] = {a, b};
        const float w[2] = {float(1.0 - t), float(t)};
        pointAttrs().mix(m, src, w);
    }

    std::vector<Index> radial;
    forEachEdgeCorner(e, [&](Index c) { radial.push_back(c); });

    // e becomes a-m, the new edge m-b.
    diskRemove(e, b);
    mEdges[e].v[1] = m;
    diskInsert(e, m);
    const Index e2 = newEdge(m, b);
    edgeAttrs().copy(e2, e);
    mLastSplitEdge = e2;

    for (Index c : radial)
    {
        const Index f = mCorners[c].face;
        const Index next = mCorners[c].next;
        const Index n = newCorner(m, f);
        cornerAttrs().resize(mCorners.size());
        mCorners[n].prev = c;
        mCorners[n].next = next;
        mCorners[c].next = n;
        mCorners[next].prev = n;
        ++mFaces[f].size;

        const float tc = mCorners[c].point == a ? float(t) : float(1.0 - t);
        const Index src[2] = {c, next};
        const float w[2] = {1.0f - tc, tc};
        cornerAttrs().mix(n, src, w);

        if (mCorners[c].point == a)
        {
            radialInsert(n, e2);
        }
        else
        {
            radialRemove(c);
            radialInsert(c, e2);
            radialInsert(n, e);
        }
    }
    return m;
}

Index Mesh::splitFace(Index f, Index ca, Index cb)
{
    if (ca == cb || mCorners[ca].face != f || mCorners[cb].face != f || mCorners[ca].next == cb
        || mCorners[cb].next == ca)
        return kInvalid;

    const Index pa = mCorners[ca].point;
    const Index pb = mCorners[cb].point;
    const Index ne = addEdge(pa, pb);
    mLastSplitEdge = ne;
    const Index f2 = Index(mFaces.size());
    mFaces.push_back({cb, 0});
    faceAttrs().resize(mFaces.size());
    faceAttrs().copy(f2, f);
    ++mNumFaces;

    const Index na = newCorner(pa, f2);
    const Index nb = newCorner(pb, f);
    cornerAttrs().resize(mCorners.size());
    cornerAttrs().copy(na, ca);
    cornerAttrs().copy(nb, cb);

    const Index prevA = mCorners[ca].prev;
    const Index prevB = mCorners[cb].prev;

    // f: ca .. prevB, nb(pb -> pa)
    mCorners[prevB].next = nb;
    mCorners[nb].prev = prevB;
    mCorners[nb].next = ca;
    mCorners[ca].prev = nb;
    // f2: cb .. prevA, na(pa -> pb)
    mCorners[prevA].next = na;
    mCorners[na].prev = prevA;
    mCorners[na].next = cb;
    mCorners[cb].prev = na;

    radialInsert(na, ne);
    radialInsert(nb, ne);

    Index size2 = 0;
    for (Index c = cb;; c = mCorners[c].next)
    {
        mCorners[c].face = f2;
        ++size2;
        if (c == na)
            break;
    }
    mFaces[f2].size = size2;
    mFaces[f].size = mFaces[f].size + 2 - size2;
    mFaces[f].corner = ca;
    return f2;
}

Index Mesh::joinFaces(Index e)
{
    if (!isManifoldEdge(e))
        return kInvalid;
    const Index c1 = mEdges[e].corner;
    const Index c2 = mCorners[c1].radialNext;
    const Index f1 = mCorners[c1].face;
    const Index f2 = mCorners[c2].face;
    if (f1 == f2 || mCorners[c1].point == mCorners[c2].point)
        return kInvalid;

    // The merged loop must not visit a point twice.
    std::vector<Index> pts;
    for (Index c = mCorners[c1].next; c != c1; c = mCorners[c].next)
        pts.push_back(mCorners[c].point);
    for (Index c = mCorners[mCorners[c2].next].next; c != c2; c = mCorners[c].next)
        pts.push_back(mCorners[c].point);
    if (hasRepeats(pts))
        return kInvalid;

    const Index n1 = mCorners[c1].next;
    const Index n2 = mCorners[c2].next;
    const Index p1 = mCorners[c1].prev;
    const Index p2 = mCorners[c2].prev;
    mCorners[p1].next = n2;
    mCorners[n2].prev = p1;
    mCorners[p2].next = n1;
    mCorners[n1].prev = p2;
    for (Index c = n2; c != n1; c = mCorners[c].next)
        mCorners[c].face = f1;

    mFaces[f1].size += mFaces[f2].size - 2;
    mFaces[f1].corner = n1;
    mFaces[f2] = {kInvalid, 0};
    --mNumFaces;
    radialRemove(c1);
    radialRemove(c2);
    killCorner(c1);
    killCorner(c2);
    removeEdge(e);
    return f1;
}

bool Mesh::setFacePoints(Index f, std::span<const Index> points)
{
    if (!faceValid(f) || points.size() != mFaces[f].size || hasRepeats(points))
        return false;
    for (Index p : points)
        if (!pointValid(p))
            return false;
    const std::vector<Index> corners = faceCorners(f);
    const size_t n = corners.size();
    for (Index c : corners)
        radialRemove(c);
    for (size_t i = 0; i < n; ++i)
        mCorners[corners[i]].point = points[i];
    for (size_t i = 0; i < n; ++i)
        radialInsert(corners[i], addEdge(points[i], points[(i + 1) % n]));
    return true;
}

void Mesh::flipFace(Index f)
{
    std::vector<Index> corners = faceCorners(f);
    const size_t n = corners.size();
    std::vector<Index> edges(n);
    for (size_t i = 0; i < n; ++i)
    {
        edges[i] = mCorners[corners[i]].edge;
        radialRemove(corners[i]);
    }
    for (size_t i = 0; i < n; ++i)
    {
        Corner& c = mCorners[corners[i]];
        c.next = corners[(i + n - 1) % n];
        c.prev = corners[(i + 1) % n];
        radialInsert(corners[i], edges[(i + n - 1) % n]);
    }
}

void Mesh::clear()
{
    *this = Mesh();
}

void Mesh::compact(MeshRemap* remap)
{
    LILA_ZONE();

    MeshRemap local;
    MeshRemap& r = remap ? *remap : local;
    auto build = [](Index cap, auto&& alive, std::vector<Index>& oldToNew, std::vector<Index>& newToOld) {
        oldToNew.assign(cap, kInvalid);
        newToOld.clear();
        for (Index i = 0; i < cap; ++i)
            if (alive(i))
            {
                oldToNew[i] = Index(newToOld.size());
                newToOld.push_back(i);
            }
    };
    std::vector<Index> pOld, eOld, fOld, cOld;
    build(pointCapacity(), [&](Index i) { return pointValid(i); }, r.points, pOld);
    build(edgeCapacity(), [&](Index i) { return edgeValid(i); }, r.edges, eOld);
    build(faceCapacity(), [&](Index i) { return faceValid(i); }, r.faces, fOld);

    r.corners.assign(cornerCapacity(), kInvalid);
    cOld.reserve(mNumCorners);
    for (Index f : fOld)
        forEachFaceCorner(f, [&](Index c) {
            r.corners[c] = Index(cOld.size());
            cOld.push_back(c);
        });

    auto mapIdx = [](const std::vector<Index>& m, Index i) { return i == kInvalid ? kInvalid : m[i]; };

    std::vector<glm::dvec3> pos(pOld.size());
    std::vector<Index> pEdge(pOld.size());
    for (size_t i = 0; i < pOld.size(); ++i)
    {
        pos[i] = mPointPos[pOld[i]];
        pEdge[i] = mapIdx(r.edges, mPointEdge[pOld[i]]);
    }

    std::vector<Edge> edges(eOld.size());
    for (size_t i = 0; i < eOld.size(); ++i)
    {
        const Edge& o = mEdges[eOld[i]];
        Edge& n = edges[i];
        for (int s = 0; s < 2; ++s)
        {
            n.v[s] = r.points[o.v[s]];
            n.next[s] = r.edges[o.next[s]];
            n.prev[s] = r.edges[o.prev[s]];
        }
        n.corner = mapIdx(r.corners, o.corner);
    }

    std::vector<Face> faces(fOld.size());
    std::vector<Corner> corners(cOld.size());
    for (size_t i = 0; i < fOld.size(); ++i)
        faces[i] = {r.corners[mFaces[fOld[i]].corner], mFaces[fOld[i]].size};
    for (size_t i = 0; i < cOld.size(); ++i)
    {
        const Corner& o = mCorners[cOld[i]];
        corners[i] = {r.points[o.point], r.edges[o.edge], r.faces[o.face], r.corners[o.next], r.corners[o.prev],
                      r.corners[o.radialNext], r.corners[o.radialPrev]};
    }

    mPointPos = std::move(pos);
    mPointEdge = std::move(pEdge);
    mPointDead.assign(mPointPos.size(), 0);
    mEdges = std::move(edges);
    mFaces = std::move(faces);
    mCorners = std::move(corners);
    pointAttrs().reorder(pOld);
    edgeAttrs().reorder(eOld);
    faceAttrs().reorder(fOld);
    cornerAttrs().reorder(cOld);
    mLastSplitEdge = kInvalid;
}

Index Mesh::append(const Mesh& other)
{
    LILA_ZONE();

    for (size_t d = 0; d < size_t(Domain::Count); ++d)
        mAttrs[d].mergeLayout(other.mAttrs[d]);
    if (mColorSetInfo.current.empty())
        mColorSetInfo.current = other.mColorSetInfo.current;
    for (const std::string& h : other.mColorSetInfo.half)
        if (std::find(mColorSetInfo.half.begin(), mColorSetInfo.half.end(), h) == mColorSetInfo.half.end())
            mColorSetInfo.half.push_back(h);

    // Material ids are remapped by name.
    std::vector<int32_t> matMap(other.mMaterialNames.size());
    for (size_t i = 0; i < other.mMaterialNames.size(); ++i)
    {
        auto it = std::find(mMaterialNames.begin(), mMaterialNames.end(), other.mMaterialNames[i]);
        if (it == mMaterialNames.end())
        {
            mMaterialNames.push_back(other.mMaterialNames[i]);
            it = mMaterialNames.end() - 1;
        }
        matMap[i] = int32_t(it - mMaterialNames.begin());
    }

    const Index offset = pointCapacity();
    std::vector<Index> pmap(other.pointCapacity(), kInvalid);
    for (Index p = 0; p < other.pointCapacity(); ++p)
        if (other.pointValid(p))
        {
            pmap[p] = addPoint(other.mPointPos[p]);
            pointAttrs().copyFrom(other.pointAttrs(), pmap[p], p);
        }

    const Index firstNewFace = faceCapacity();
    std::vector<Index> pts;
    for (Index f = 0; f < other.faceCapacity(); ++f)
    {
        if (!other.faceValid(f))
            continue;
        pts.clear();
        other.forEachFaceCorner(f, [&](Index c) { pts.push_back(pmap[other.mCorners[c].point]); });
        const Index nf = addFace(pts);
        if (nf == kInvalid)
            continue;
        faceAttrs().copyFrom(other.faceAttrs(), nf, f);
        Index c = mFaces[nf].corner;
        other.forEachFaceCorner(f, [&](Index oc) {
            cornerAttrs().copyFrom(other.cornerAttrs(), c, oc);
            c = mCorners[c].next;
        });
    }
    for (Index e = 0; e < other.edgeCapacity(); ++e)
    {
        if (!other.edgeValid(e))
            continue;
        const Index ne = addEdge(pmap[other.mEdges[e].v[0]], pmap[other.mEdges[e].v[1]]);
        if (ne != kInvalid)
            edgeAttrs().copyFrom(other.edgeAttrs(), ne, e);
    }

    if (!matMap.empty())
    {
        std::span<int32_t> mat = faceAttrs().get<int32_t>(attr::kMaterial);
        for (Index f = firstNewFace; f < faceCapacity() && !mat.empty(); ++f)
            if (mat[f] >= 0 && size_t(mat[f]) < matMap.size())
                mat[f] = matMap[mat[f]];
    }
    return offset;
}

std::vector<std::string> Mesh::validate(uint32_t flags) const
{
    LILA_ZONE();

    std::vector<std::string> out;
    auto bad = [&](std::string s) {
        if (out.size() < 200)
            out.push_back(std::move(s));
    };
    const Index pc = pointCapacity();
    const Index ec = edgeCapacity();
    const Index cc = cornerCapacity();

    Index livePoints = 0, liveEdges = 0, liveFaces = 0, liveCorners = 0;
    for (Index p = 0; p < pc; ++p)
    {
        if (!pointValid(p))
            continue;
        ++livePoints;
        const Index e = mPointEdge[p];
        if (e != kInvalid && (!edgeValid(e) || (mEdges[e].v[0] != p && mEdges[e].v[1] != p)))
            bad(std::format("point {}: edge {} does not contain it", p, e));
        else if (e == kInvalid && (flags & kValidateLoose))
            bad(std::format("point {}: loose", p));
    }

    for (Index e = 0; e < ec; ++e)
    {
        if (!edgeValid(e))
            continue;
        ++liveEdges;
        const Edge& ed = mEdges[e];
        if (!pointValid(ed.v[0]) || !pointValid(ed.v[1]) || ed.v[0] == ed.v[1])
        {
            bad(std::format("edge {}: bad points {} {}", e, ed.v[0], ed.v[1]));
            continue;
        }
        for (int s = 0; s < 2; ++s)
        {
            const Index p = ed.v[s];
            const Index n = ed.next[s];
            if (!edgeValid(n) || (mEdges[n].v[0] != p && mEdges[n].v[1] != p) || mEdges[n].prev[side(n, p)] != e)
            {
                bad(std::format("edge {}: broken disk link at point {}", e, p));
                continue;
            }
            bool found = false;
            Index steps = 0;
            forEachPointEdge(p, [&](Index x) {
                found |= x == e;
                ++steps;
            });
            if (!found || steps > ec)
                bad(std::format("edge {}: not in disk cycle of point {}", e, p));
        }
        if (ed.corner == kInvalid)
        {
            if (flags & kValidateLoose)
                bad(std::format("edge {}: loose", e));
            continue;
        }
        Index count = 0;
        bool ok = true;
        Index c = ed.corner;
        do
        {
            if (!cornerValid(c) || mCorners[c].edge != e || mCorners[mCorners[c].radialNext].radialPrev != c)
            {
                ok = false;
                break;
            }
            c = mCorners[c].radialNext;
        } while (c != ed.corner && ++count <= cc);
        if (!ok || count > cc)
            bad(std::format("edge {}: broken radial cycle", e));
        else if ((flags & kValidateManifold) && count + 1 > 2)
            bad(std::format("edge {}: non-manifold, {} faces", e, count + 1));
        else if ((flags & kValidateWinding) && count + 1 == 2)
        {
            const Index r = mCorners[ed.corner].radialNext;
            if (mCorners[r].point == mCorners[ed.corner].point)
                bad(std::format("edge {}: faces {} and {} wound inconsistently", e, mCorners[ed.corner].face,
                                mCorners[r].face));
        }
        if ((flags & kValidateDegenerate) && edgeLength(e) <= 0.0)
            bad(std::format("edge {}: zero length", e));
    }

    for (Index f = 0; f < faceCapacity(); ++f)
    {
        if (!faceValid(f))
            continue;
        ++liveFaces;
        const Index n = mFaces[f].size;
        Index count = 0;
        Index c = mFaces[f].corner;
        bool ok = true;
        std::vector<Index> pts;
        do
        {
            if (!cornerValid(c) || mCorners[c].face != f || mCorners[mCorners[c].next].prev != c)
            {
                ok = false;
                break;
            }
            const Corner& cr = mCorners[c];
            const Index np = mCorners[cr.next].point;
            if (!edgeValid(cr.edge)
                || !((mEdges[cr.edge].v[0] == cr.point && mEdges[cr.edge].v[1] == np)
                     || (mEdges[cr.edge].v[1] == cr.point && mEdges[cr.edge].v[0] == np)))
            {
                bad(std::format("face {}: corner {} edge does not join its points", f, c));
            }
            pts.push_back(cr.point);
            c = cr.next;
        } while (c != mFaces[f].corner && ++count <= cc);
        if (!ok || count + 1 != n)
        {
            bad(std::format("face {}: broken corner loop (size {})", f, n));
            continue;
        }
        if (n < 3)
            bad(std::format("face {}: only {} corners", f, n));
        if (hasRepeats(pts))
            bad(std::format("face {}: repeated point", f));
        if ((flags & kValidateDegenerate) && faceArea(f) <= 1e-12)
            bad(std::format("face {}: zero area", f));
    }

    for (Index c = 0; c < cc; ++c)
        if (cornerValid(c))
            ++liveCorners;

    if (livePoints != mNumPoints || liveEdges != mNumEdges || liveFaces != mNumFaces || liveCorners != mNumCorners)
        bad(std::format("counts out of sync: points {}/{} edges {}/{} faces {}/{} corners {}/{}", livePoints,
                        mNumPoints, liveEdges, mNumEdges, liveFaces, mNumFaces, liveCorners, mNumCorners));
    for (size_t d = 0; d < size_t(Domain::Count); ++d)
    {
        const size_t want = d == 0 ? pc : d == 1 ? ec : d == 2 ? faceCapacity() : cc;
        if (mAttrs[d].size() != want)
            bad(std::format("{} attributes sized {} for {} elements", domainName(Domain(d)), mAttrs[d].size(), want));
    }
    return out;
}

Mesh Mesh::fromPolygons(std::span<const glm::dvec3> points, std::span<const Index> faceSizes,
                        std::span<const Index> faceIndices)
{
    LILA_ZONE();

    Mesh m;
    m.reserve(points.size(), faceIndices.size(), faceSizes.size(), faceIndices.size());
    for (const glm::dvec3& p : points)
        m.addPoint(p);
    size_t at = 0;
    for (Index n : faceSizes)
    {
        if (at + n > faceIndices.size())
            break;
        m.addFace(faceIndices.subspan(at, n));
        at += n;
    }
    return m;
}
} // namespace lila::geo
