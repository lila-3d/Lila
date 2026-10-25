#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Topology.h"
#include "retopo/Internal.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <optional>
#include <unordered_map>

namespace lila::retopo
{
using namespace detail;

namespace
{
class Stopwatch
{
public:
    explicit Stopwatch(std::vector<std::pair<std::string, double>>& out) : mOut(out) {}
    void lap(const char* name)
    {
        auto now = std::chrono::steady_clock::now();
        mOut.emplace_back(name, std::chrono::duration<double>(now - mLast).count());
        mLast = now;
        // Roughly where each stage ends in a typical run, for the progress bar.
        static const std::pair<const char*, double> kAt[] = {{"bvh", 0.03}, {"surface", 0.12}, {"density", 0.18}, {"cross field", 0.3},
                                                             {"seamless", 0.6}, {"local", 0.9}, {"finish", 1.0}};
        double at = mAt;
        for (auto [stage, f] : kAt)
            if (std::string_view(stage) == name)
                at = f;
        mAt = at;
        Budget::reached(name, at);
    }

private:
    std::vector<std::pair<std::string, double>>& mOut;
    std::chrono::steady_clock::time_point mLast = std::chrono::steady_clock::now();
    double mAt = 0.0;
};

std::vector<std::vector<Index>> pointNeighbours(const geo::Mesh& m)
{
    std::vector<std::vector<Index>> nb(m.pointCapacity());
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.forEachPointEdge(p, [&](Index e) { nb[p].push_back(m.edgeOtherPoint(e, p)); });
    return nb;
}

/** Snaps each point to its feature (corner, crease line) or the surface; demotes points whose feature isn't near. */
void projectPoints(geo::Mesh& m, std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, const FeatureLines& lines, double reach)
{
    LILA_ZONE();

    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p))
            return;
        glm::dvec3 at = m.position(p), q;
        if (feature[p] == FeatureKind::Corner)
        {
            if (lines.closestCorner(at, reach, q))
            {
                m.setPosition(p, q);
                return;
            }
            feature[p] = FeatureKind::Line;
        }
        if (feature[p] == FeatureKind::Line)
        {
            if (lines.closest(at, reach, q))
            {
                m.setPosition(p, q);
                return;
            }
            feature[p] = FeatureKind::None;
        }
        geo::BvhClosest c;
        if (bvh.closestPoint(at, c))
            m.setPosition(p, c.point);
    });
}

/** Tangential Laplacian relax that slides points along the surface and crease points along their creases. */
void relaxOnSurface(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, const FeatureLines& lines, double h,
                    int iterations)
{
    LILA_ZONE();

    auto nb = pointNeighbours(m);
    std::vector<glm::dvec3> next(m.pointCapacity());
    for (int it = 0; it < iterations; ++it)
    {
        Budget::checkpoint();
        std::vector<glm::vec3> normals = geo::pointNormals(m);
        tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
            if (!m.pointValid(p))
                return;
            glm::dvec3 at = m.position(p);
            next[p] = at;
            if (feature[p] == FeatureKind::Corner || nb[p].empty())
                return;
            glm::dvec3 avg(0.0);
            int count = 0;
            for (Index o : nb[p])
                if (feature[p] == FeatureKind::None || feature[o] != FeatureKind::None)
                {
                    avg += m.position(o);
                    ++count;
                }
            if (feature[p] == FeatureKind::Line)
            {
                // Slide only between the two crease neighbours.
                if (count != 2)
                    return;
                glm::dvec3 target = glm::mix(at, avg / double(count), 0.5), q;
                if (lines.closest(target, 0.5 * h, q))
                    next[p] = q;
                return;
            }
            glm::dvec3 n(normals[p]);
            glm::dvec3 d = avg / double(count) - at;
            d -= n * glm::dot(n, d);
            geo::BvhClosest c;
            if (bvh.closestPoint(at + 0.5 * d, c))
                next[p] = c.point;
        });
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                m.setPosition(p, next[p]);
    }
}

/** Faces facing against the source surface under them. */
std::vector<Index> foldedFaces(const geo::Mesh& m, const geo::Mesh& src, const geo::MeshBvh& bvh)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        geo::BvhClosest c;
        if (bvh.closestPoint(m.faceCentroid(f), c) && glm::dot(m.faceAreaVector(f), src.faceNormal(c.face)) < 0.0)
            out.push_back(f);
    }
    return out;
}

/**
 * Smooths out folds the snapping and relaxing left: points of faces that face against the source move to the middle
 * of their neighbours, back on the surface (or their crease), until nothing is folded. Corners stay.
 */
void untangle(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::Mesh& src, const geo::MeshBvh& bvh,
              const FeatureLines& lines, double h)
{
    LILA_ZONE();

    for (int it = 0; it < 20; ++it)
    {
        Budget::checkpoint();
        const std::vector<Index> folded = foldedFaces(m, src, bvh);
        if (folded.empty())
            return;
        std::vector<Index> pts;
        for (Index f : folded)
            for (Index p : m.facePoints(f))
                if (feature[p] != FeatureKind::Corner)
                    pts.push_back(p);
        std::sort(pts.begin(), pts.end());
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        // Widen to the ring once it has had a few tries, so a fold pinned between corners can still open up.
        if (it >= 5)
        {
            const std::vector<Index> ring = pts;
            for (Index p : ring)
                m.forEachPointEdge(p, [&](Index e) {
                    const Index q = m.edgeOtherPoint(e, p);
                    if (feature[q] != FeatureKind::Corner)
                        pts.push_back(q);
                });
            std::sort(pts.begin(), pts.end());
            pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        }
        std::vector<glm::dvec3> next(pts.size());
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const Index p = pts[k];
            glm::dvec3 avg(0.0);
            int count = 0;
            m.forEachPointEdge(p, [&](Index e) {
                const Index q = m.edgeOtherPoint(e, p);
                // Along a crease only its own points pull.
                if (feature[p] == FeatureKind::None || feature[q] != FeatureKind::None)
                {
                    avg += m.position(q);
                    ++count;
                }
            });
            next[k] = m.position(p);
            if (count == 0)
                continue;
            const glm::dvec3 target = avg / double(count);
            glm::dvec3 q;
            geo::BvhClosest c;
            if (feature[p] == FeatureKind::Line && lines.closest(target, h, q))
                next[k] = q;
            else if (feature[p] != FeatureKind::Line && bvh.closestPoint(target, c))
                next[k] = c.point;
        }
        for (size_t k = 0; k < pts.size(); ++k)
            m.setPosition(pts[k], next[k]);
    }
}

/** Interior points with two edges just bend an edge: take them out of both faces. */
void removeValence2(geo::Mesh& m, std::vector<FeatureKind>& feature)
{
    LILA_ZONE();

    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p) || m.isBoundaryPoint(p) || m.pointValence(p) != 2)
            continue;
        std::vector<Index> faces = m.pointFaces(p);
        std::vector<std::vector<Index>> rebuilt;
        bool ok = true;
        for (Index f : faces)
        {
            std::vector<Index> pts;
            for (Index q : m.facePoints(f))
                if (q != p)
                    pts.push_back(q);
            ok &= pts.size() >= 3;
            rebuilt.push_back(std::move(pts));
        }
        if (!ok)
            continue;
        for (Index f : faces)
            m.removeFace(f);
        m.removePoint(p);
        for (const auto& pts : rebuilt)
            m.addFace(pts);
        feature[p] = FeatureKind::None;
    }
}

/**
 * Closes the holes extraction leaves where the field was too tangled to trace a face. Boundary edges are walked
 * against their faces, and a walk that comes back through a point it already passed is cut there, so a hole that
 * touches itself or another hole still comes out as simple loops. Loops on the source's own border stay open.
 */
void fillHoles(geo::Mesh& m, const std::vector<uint8_t>& sourceBoundary)
{
    LILA_ZONE();

    std::vector<std::vector<Index>> out(m.pointCapacity());
    Index count = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) == 1)
        {
            const Index c = m.edgeCorner(e);
            out[m.cornerNextPoint(c)].push_back(m.cornerPoint(c));
            ++count;
        }
    std::vector<std::vector<Index>> loops;
    std::vector<Index> walk, at(m.pointCapacity(), kInvalid);
    for (Index start = 0; start < m.pointCapacity() && count > 0; ++start)
        while (!out[start].empty())
        {
            walk.clear();
            Index p = start;
            for (;;)
            {
                if (const Index from = at[p]; from != kInvalid)
                {
                    // Back at a point of this walk: the stretch since then is one loop.
                    loops.emplace_back(walk.begin() + from, walk.end());
                    for (size_t k = from; k < walk.size(); ++k)
                        at[walk[k]] = kInvalid;
                    walk.resize(from);
                }
                if (out[p].empty())
                    break;
                at[p] = Index(walk.size());
                walk.push_back(p);
                const Index q = out[p].back();
                out[p].pop_back();
                --count;
                p = q;
            }
            for (Index q : walk)
                at[q] = kInvalid;
        }
    const auto onBorder = [&](Index p) { return p < sourceBoundary.size() && sourceBoundary[p]; };
    for (const std::vector<Index>& loop : loops)
    {
        if (loop.size() < 3)
            continue;
        std::vector<size_t> border;
        for (size_t k = 0; k < loop.size(); ++k)
            if (onBorder(loop[k]))
                border.push_back(k);
        if (border.size() < 2)
        {
            m.addFace(loop);
            continue;
        }
        // A hole that reaches the source's border is a bay in it: fill each run of inner points between two border
        // points, closing it along the border.
        for (size_t i = 0; i < border.size(); ++i)
        {
            const size_t a = border[i], b = border[(i + 1) % border.size()];
            std::vector<Index> bay;
            for (size_t k = a;; k = (k + 1) % loop.size())
            {
                bay.push_back(loop[k]);
                if (k == b && bay.size() > 1)
                    break;
                if (bay.size() > loop.size())
                    break;
            }
            if (bay.size() >= 3 && bay.front() != bay.back())
                m.addFace(bay);
        }
    }
}

/** Moving p to `to` leaves every face around it facing the way it did. */
bool movesCleanly(const geo::Mesh& m, Index p, const glm::dvec3& to)
{
    bool ok = true;
    m.forEachPointCorner(p, [&](Index c) {
        if (!ok)
            return;
        const Index f = m.cornerFace(c);
        const glm::dvec3 before = m.faceAreaVector(f);
        glm::dvec3 after(0.0);
        const Index c0 = m.faceCorner(f);
        const auto at = [&](Index k) { return m.cornerPoint(k) == p ? to : m.position(m.cornerPoint(k)); };
        const glm::dvec3 o = at(c0);
        for (Index k = m.cornerNext(c0); m.cornerNext(k) != c0; k = m.cornerNext(k))
            after += glm::cross(at(k) - o, at(m.cornerNext(k)) - o);
        ok = glm::dot(after, before) > 0.1 * glm::length(after) * glm::length(before);
    });
    return ok;
}

/**
 * Every source corner gets an output point. The nearest free point within reach moves onto it if no face turns over;
 * otherwise the nearest edge (a border edge, for a corner on the border) is split there. Either way it stays put after.
 */
void snapCorners(geo::Mesh& m, std::vector<FeatureKind>& feature, const Surface& s, double reach)
{
    LILA_ZONE();

    const double same = 1e-9 * (1.0 + reach);
    std::vector<Index> corners;
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (s.feature[v] == FeatureKind::Corner)
            corners.push_back(v);
    std::vector<uint8_t> taken(m.pointCapacity(), 0);
    std::vector<std::pair<double, Index>> near;
    for (Index v : corners)
    {
        const glm::dvec3 target = s.pos[v];
        const bool onBorder = s.onBoundary[v] != 0;
        near.clear();
        bool present = false;
        for (Index p = 0; p < m.pointCapacity() && !present; ++p)
        {
            if (!m.pointValid(p))
                continue;
            const double d = glm::distance(m.position(p), target);
            present = d < same;
            if (d < reach && !taken[p] && onBorder == m.isBoundaryPoint(p))
                near.push_back({d, p});
        }
        if (present)
            continue;
        std::sort(near.begin(), near.end());
        Index chosen = kInvalid;
        for (const auto& [d, p] : near)
            if (movesCleanly(m, p, target))
            {
                chosen = p;
                break;
            }
        if (chosen == kInvalid)
        {
            // A border corner has to be there however far the outline fell short; an inner one only nearby.
            Index edge = kInvalid;
            double bestD = onBorder ? 3.0 * reach : reach, bestT = 0.5;
            for (Index e = 0; e < m.edgeCapacity(); ++e)
            {
                if (!m.edgeValid(e) || (onBorder && !m.isBoundaryEdge(e)))
                    continue;
                const glm::dvec3 a = m.position(m.edgePoint(e, 0)), ab = m.position(m.edgePoint(e, 1)) - a;
                const double len2 = glm::dot(ab, ab);
                const double t = len2 > 0.0 ? std::clamp(glm::dot(target - a, ab) / len2, 0.1, 0.9) : 0.5;
                const double d = glm::distance(a + ab * t, target);
                if (d < bestD)
                {
                    bestD = d;
                    bestT = t;
                    edge = e;
                }
            }
            if (edge == kInvalid)
                continue;
            chosen = m.splitEdge(edge, bestT);
            feature.resize(m.pointCapacity(), FeatureKind::None);
            taken.resize(m.pointCapacity(), 0);
            if (!movesCleanly(m, chosen, target))
            {
                feature[chosen] = FeatureKind::None;
                continue;
            }
        }
        m.setPosition(chosen, target);
        feature[chosen] = FeatureKind::Corner;
        taken[chosen] = 1;
    }
}

/** Output border points go onto the source's border, so open edges keep their outline. */
void snapBorder(geo::Mesh& m, std::vector<FeatureKind>& feature, const FeatureLines& border, double reach)
{
    LILA_ZONE();

    if (border.empty())
        return;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p) || !m.isBoundaryPoint(p) || feature[p] == FeatureKind::Corner)
            continue;
        glm::dvec3 q;
        if (border.closest(m.position(p), reach, q))
        {
            m.setPosition(p, q);
            feature[p] = FeatureKind::Line;
        }
    }
}

/**
 * Splits every n-gon into n quads around its centroid (no smoothing), so any polygon mix comes out all quads. Edge
 * midpoints along a crease stay on it.
 */
geo::Mesh splitToQuads(const geo::Mesh& m, std::vector<FeatureKind>& feature, std::vector<uint8_t>& boundary, const FeatureLines& lines,
                       double h)
{
    LILA_ZONE();

    std::vector<glm::dvec3> pts;
    std::vector<FeatureKind> outFeature;
    std::vector<uint8_t> outBoundary;
    std::vector<Index> pointId(m.pointCapacity(), kInvalid);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            pointId[p] = Index(pts.size());
            pts.push_back(m.position(p));
            outFeature.push_back(feature[p]);
            outBoundary.push_back(boundary[p]);
        }
    std::vector<Index> edgeId(m.edgeCapacity(), kInvalid);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
            glm::dvec3 mid = 0.5 * (m.position(a) + m.position(b));
            FeatureKind k = FeatureKind::None;
            if (feature[a] != FeatureKind::None && feature[b] != FeatureKind::None)
            {
                glm::dvec3 q, t;
                glm::dvec3 dir = glm::normalize(m.position(b) - m.position(a));
                if (lines.closest(mid, 0.3 * h, q, &t) && std::abs(glm::dot(t, dir)) > 0.8)
                    k = FeatureKind::Line;
            }
            edgeId[e] = Index(pts.size());
            pts.push_back(mid);
            outFeature.push_back(k);
            outBoundary.push_back(m.isBoundaryEdge(e));
        }
    std::vector<Index> sizes, idx;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        Index centre = Index(pts.size());
        pts.push_back(m.faceCentroid(f));
        outFeature.push_back(FeatureKind::None);
        outBoundary.push_back(0);
        std::vector<Index> cs = m.faceCorners(f);
        for (size_t k = 0; k < cs.size(); ++k)
        {
            Index c = cs[k], prev = cs[(k + cs.size() - 1) % cs.size()];
            sizes.push_back(4);
            idx.insert(idx.end(), {pointId[m.cornerPoint(c)], edgeId[m.cornerEdge(c)], centre, edgeId[m.cornerEdge(prev)]});
        }
    }
    geo::Mesh out = geo::Mesh::fromPolygons(pts, sizes, idx);
    feature = std::move(outFeature);
    boundary = std::move(outBoundary);
    return out;
}

/** Compacts the mesh and the per-point tags along with it. */
void compactTagged(geo::Mesh& m, std::vector<FeatureKind>& feature, std::vector<uint8_t>& boundary)
{
    geo::MeshRemap remap;
    m.compact(&remap);
    std::vector<FeatureKind> f(m.pointCapacity(), FeatureKind::None);
    std::vector<uint8_t> b(m.pointCapacity(), 0);
    for (Index p = 0; p < Index(remap.points.size()); ++p)
        if (remap.points[p] != kInvalid)
        {
            f[remap.points[p]] = p < feature.size() ? feature[p] : FeatureKind::None;
            b[remap.points[p]] = p < boundary.size() ? boundary[p] : 0;
        }
    feature = std::move(f);
    boundary = std::move(b);
}
} // namespace

namespace
{
/**
 * Caps the lattice spacing where the surface comes close to itself across a gap, like a ball sitting in a pocket. A lattice
 * cell wider than the gap bridges it and the result collapses into it. At most twice as fine. The face count grows by
 * what the gaps need. The cap then grows out
 * gradually so the spacing still varies slowly.
 */
void fitGaps(const Surface& s, const geo::Mesh& src, const geo::MeshBvh& bvh, std::vector<double>& scale)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    std::vector<double> cap(n, std::numeric_limits<double>::max());
    tbb::parallel_for(Index(0), n, [&](Index v) {
        const glm::dvec3 nrm = s.normal[v];
        if (glm::dot(nrm, nrm) < 0.5)
            return;
        // Out along the normal to a surface facing back: a gap across air. Hitting the far side of a thin wall from
        // inside isn't one, and the two sides of a wall don't bridge.
        const double eps = 1e-3 * scale[v];
        geo::BvhHit hit;
        // The far side has to face straight back and be a fair part of a cell away: noise bumps next door are hit close
        // by or at a glancing angle.
        if (bvh.raycast(s.pos[v] + eps * nrm, nrm, hit, 2.0 * scale[v], false) && hit.t > 0.2 * scale[v] && hit.frontFacing &&
            hit.face != geo::kInvalid && glm::dot(src.faceNormal(hit.face), nrm) < -0.7)
            cap[v] = std::min(cap[v], std::max(0.8 * (hit.t + eps), scale[v] / 2.0));
    });
    // The caps fade out over a few cells around where they were found, so the spacing still varies slowly.
    for (int it = 0; it < 30; ++it)
    {
        bool changed = false;
        for (Index v = 0; v < n; ++v)
            for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
            {
                Index w = s.adj[k];
                double limit = cap[w] + 0.25 * glm::length(s.pos[v] - s.pos[w]);
                if (cap[v] > limit)
                {
                    cap[v] = limit;
                    changed = true;
                }
            }
        if (!changed)
            break;
    }
    for (Index v = 0; v < n; ++v)
        scale[v] = std::min(scale[v], cap[v]);
}

/** The working surface and lattice spacing everything after it shares. Not movable: the bvh points into src. */
struct Work
{
    geo::Mesh src;
    std::optional<geo::MeshBvh> bvh;
    Surface s;
    bool symmetric = false;
    geo::SymmetryPlane plane;
    double h = 0.0, cf = 1.0;
    std::vector<double> scale;
    FieldOptions fo;
    std::string error;

    Work() = default;
    Work(const Work&) = delete;
};

void prepareWork(Work& w, const geo::Mesh& source, const QuadRemeshOptions& opts, Stopwatch& clock)
{
    LILA_ZONE();

    if (source.numFaces() == 0)
    {
        w.error = "source has no faces";
        return;
    }
    w.src = source;
    w.src.compact();
    w.bvh.emplace(w.src);
    const geo::MeshBvh& bvh = *w.bvh;
    clock.lap("bvh");

    Surface& s = w.s;
    s = surfaceFromMesh(w.src, opts.useHardEdges);
    s.pins = opts.pinnedPoints;
    w.symmetric = opts.symmetryAxis >= 0 && opts.symmetryAxis < 3;
    const bool symmetric = w.symmetric;
    if (symmetric)
    {
        w.plane.normal = glm::dvec3(0.0);
        w.plane.normal[opts.symmetryAxis] = 1.0;
        clipSurface(s, w.plane, opts.symmetryAxis);
        if (s.tris.empty())
        {
            w.error = "nothing on the positive side of the symmetry plane";
            return;
        }
    }
    const double area = s.totalArea();
    const double faces = std::max(8.0, double(opts.targetFaces) * (symmetric ? 0.5 : 1.0));
    const double h0 = opts.targetEdgeLength > 0.0 ? opts.targetEdgeLength : std::sqrt(area / faces);
    const double cf = opts.pureQuads ? 2.0 : 1.0;
    w.cf = cf;
    double workLength = cf * h0 / 4.0;
    const double workTris = std::max(64.0, area / (0.433 * workLength * workLength));
    bool decimated = double(s.tris.size()) > 1.5 * workTris;
    if (decimated)
        decimateSurface(s, Index(workTris));
    refineSurface(s, 1.5 * workLength);
    auto prepare = [&]() {
        s.buildTopology();
        s.buildGeometry();
        s.detectFeatures(opts.sharpAngle, 75.0, 2.0 * h0);
    };
    prepare();
    if (decimated)
    {
        // Simplification leaves slivers that make the fields noisy.
        smoothSurface(s, bvh, 8);
        prepare();
    }
    clock.lap("surface");

    // Gaps across air only bridge on a closed shape; an open sheet's folds remesh fine as they are.
    bool closedSource = true;
    for (Index e = 0; e < w.src.edgeCapacity() && closedSource; ++e)
        closedSource = !w.src.edgeValid(e) || !w.src.isBoundaryEdge(e);

    // Lattice spacing per vertex from painted density and curvature, normalized to the face budget.
    std::vector<double>& scale = w.scale;
    double h = h0;
    for (int pass = 0; pass < 2; ++pass)
    {
        const Index n = s.vertexCount();
        Curvature curv = estimateCurvature(s);
        std::vector<double> rho(n);
        for (Index v = 0; v < n; ++v)
        {
            double r = s.density[v];
            if (opts.adaptivity > 0.0)
                r *= std::pow(1.0 + 2.0 * curv.magnitude[v] * h0, opts.adaptivity);
            rho[v] = std::clamp(r, 1.0 / 8.0, 8.0);
        }
        // Spread far enough that the spacing changes over several lattice cells; extraction needs it slowly varying.
        const int spread = int(std::clamp(3.0 * cf * h0 / std::max(workLength, 1e-12), 6.0, 60.0));
        for (int it = 0; it < spread; ++it)
        {
            std::vector<double> next(n);
            for (Index v = 0; v < n; ++v)
            {
                double sum = rho[v];
                for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
                    sum += rho[s.adj[k]];
                next[v] = sum / double(1 + s.adjStart[v + 1] - s.adjStart[v]);
            }
            rho = std::move(next);
        }
        if (opts.targetEdgeLength <= 0.0)
        {
            double weighted = 0.0;
            for (Index v = 0; v < n; ++v)
                weighted += s.area[v] * rho[v];
            h = std::sqrt(weighted / faces);
        }
        scale.resize(n);
        for (Index v = 0; v < n; ++v)
            scale[v] = cf * h / std::sqrt(rho[v]);
        if (opts.resolveGaps && closedSource)
            fitGaps(s, w.src, bvh, scale);
        double minScale = std::numeric_limits<double>::max();
        for (Index v = 0; v < n; ++v)
            minScale = std::min(minScale, scale[v]);
        if (pass == 1 || minScale >= 2.5 * workLength)
            break;
        // Denser spots need a finer working mesh to resolve their lattice.
        workLength = minScale / 2.5;
        refineSurface(s, 1.5 * workLength);
        prepare();
    }
    w.h = h;
    clock.lap("density");

    {
        std::vector<double> sorted = scale;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        w.fo.scale = sorted[sorted.size() / 2];
    }
    w.fo.guides = opts.guides;
    w.fo.softCreaseAngle = opts.softCreaseAngle;
    s.thinCorners(scale);
    Budget::note(std::to_string(s.tris.size()) + " working triangles for about " + std::to_string(int(faces)) + " faces");
}

/** Each pole's triangle on the working surface: the one with the nearest centre. Poles behind the symmetry plane drop. */
std::vector<FieldSingularity> polesOnSurface(const Work& w, const std::vector<Pole>& poles)
{
    std::vector<FieldSingularity> out;
    for (const Pole& p : poles)
    {
        if (p.valence == 4 || (w.symmetric && w.plane.distance(p.position) < 0.0))
            continue;
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (size_t t = 0; t < w.s.tris.size(); ++t)
        {
            const auto& tri = w.s.tris[t];
            glm::dvec3 c = (w.s.pos[tri[0]] + w.s.pos[tri[1]] + w.s.pos[tri[2]]) / 3.0;
            double d = glm::dot(c - p.position, c - p.position);
            if (d < bestD)
            {
                bestD = d;
                best = Index(t);
            }
        }
        if (best != kInvalid)
            out.push_back({best, 4 - p.valence});
    }
    return out;
}

/** The cross field, turning exactly at opts.poles when given. */
std::vector<glm::dvec3> crossField(Work& w, const QuadRemeshOptions& opts, std::string& error)
{
    if (opts.poles)
    {
        PrescribedTwist pt = prescribeSingularities(w.s, polesOnSurface(w, *opts.poles));
        if (!pt.error.empty())
        {
            error = pt.error;
            return {};
        }
        w.fo.twist = std::move(pt.twist);
    }
    return solveCrossField(w.s, w.fo);
}

QuadRemeshResult quadRemeshOnce(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    QuadRemeshResult result;
    Stopwatch clock(result.timings);
    Work w;
    prepareWork(w, source, opts, clock);
    if (!w.error.empty())
    {
        result.error = w.error;
        return result;
    }
    const geo::Mesh& src = w.src;
    const geo::MeshBvh& bvh = *w.bvh;
    Surface& s = w.s;
    const bool symmetric = w.symmetric;
    const geo::SymmetryPlane& plane = w.plane;
    const double h = w.h, cf = w.cf;
    const std::vector<double>& scale = w.scale;
    const FieldOptions& fo = w.fo;

    std::vector<glm::dvec3> q = crossField(w, opts, result.error);
    if (!result.ok())
        return result;
    Budget::note(std::to_string(fieldSingularities(s, q).size()) + " poles");
    clock.lap("cross field");

    // Extraction and cleanup for one lattice layout.
    auto build = [&](const std::vector<glm::dvec3>& o) {
        QuadRemeshResult out;
        Extracted ex = extractMesh(s, q, o, scale);
        if (ex.mesh.numFaces() == 0)
        {
            out.error = "extraction produced no faces";
            return out;
        }
        clock.lap("extract");

        FeatureLines lines;
        lines.build(s, fo.scale * 0.5);
        geo::Mesh& m = ex.mesh;
        std::vector<FeatureKind>& feature = ex.feature;
        std::vector<uint8_t>& boundary = ex.boundary;
        removeValence2(m, feature);
        geo::conformNormals(m);
        fillHoles(m, boundary);
        {
            std::vector<Index> tris;
            for (Index f = 0; f < m.faceCapacity(); ++f)
                if (m.faceValid(f) && m.faceSize(f) == 3)
                    tris.push_back(f);
            geo::QuadrangulateOptions qo;
            qo.maxFaceAngle = 60.0;
            geo::quadrangulateFaces(m, tris, qo);
        }
        removeValence2(m, feature);
        feature.resize(m.pointCapacity(), FeatureKind::None);
        boundary.resize(m.pointCapacity(), 0);
        compactTagged(m, feature, boundary);
        geo::conformNormals(m);
        const double coarseH = cf * h;
        FeatureLines border;
        border.build(s, fo.scale * 0.5, true);
        snapCorners(m, feature, s, 1.25 * coarseH);
        boundary.resize(m.pointCapacity(), 0);
        snapBorder(m, feature, border, 1.5 * coarseH);
        projectPoints(m, feature, bvh, lines, 0.6 * coarseH);
        relaxOnSurface(m, feature, bvh, lines, coarseH, std::max(1, opts.relaxIterations / 2));
        clock.lap("cleanup");

        if (opts.pureQuads)
        {
            m = splitToQuads(m, feature, boundary, lines, coarseH);
            snapBorder(m, feature, border, coarseH);
            projectPoints(m, feature, bvh, lines, 0.4 * coarseH);
        }
        relaxOnSurface(m, feature, bvh, lines, h, opts.relaxIterations);
        untangle(m, feature, src, bvh, lines, h);
        clock.lap("refine");

        if (symmetric)
        {
            // Seam points sit exactly on the plane so the mirror welds them and the halves match bit for bit.
            const int axis = opts.symmetryAxis;
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p) && m.isBoundaryPoint(p) && std::abs(plane.distance(m.position(p))) < 0.35 * h)
                {
                    glm::dvec3 at = m.position(p);
                    at[axis] = 0.0;
                    m.setPosition(p, at);
                }
            geo::MirrorOptions mo;
            mo.plane = plane;
            mo.weld = true;
            mo.tolerance = std::max(1e-9, h * 1e-6);
            geo::mirrorMesh(m, mo);
            m.compact();
        }
        if (opts.sharpAngle > 0.0 && !lines.empty())
            geo::setHardEdgesByAngle(m, opts.sharpAngle);
        out.metrics = measureQuality(m, &bvh);
        out.mesh = std::move(m);
        return out;
    };

    // The seamless layout only turns at the poles, but rounding can tangle it near tight features; keep whichever of it
    // and the local position field comes out with fewer irregular points.
    std::vector<glm::dvec3> o = opts.seamless ? solveSeamlessPositions(s, q, scale) : std::vector<glm::dvec3>{};
    std::optional<QuadRemeshResult> seamless;
    if (!o.empty())
        seamless = build(o);
    clock.lap("seamless");
    QuadRemeshResult best = build(solvePositionField(s, q, scale));
    clock.lap("local");
    // Pins only hold exactly in the seamless layout.
    const bool preferSeamless = !opts.pinnedPoints.empty();
    if (seamless && seamless->ok() && (!best.ok() || preferSeamless || seamless->metrics.irregular <= best.metrics.irregular))
        best = std::move(*seamless);
    if (!best.ok())
    {
        result.error = best.error;
        return result;
    }
    result.mesh = std::move(best.mesh);
    result.metrics = best.metrics;
    clock.lap("finish");
    return result;
}

/** Folds, slivers, pinched edges, or holes in a closed source: what makes a lattice too coarse for the shape. */
bool isClean(const geo::Mesh& m, const geo::Mesh& src, const geo::MeshBvh& bvh, bool sourceClosed)
{
    if (!foldedFaces(m, src, bvh).empty() || !m.validate(geo::kValidateManifold).empty())
        return false;
    const double size = glm::length(src.bounds().size());
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceArea(f) <= 1e-9 * size * size)
            return false;
    if (sourceClosed)
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && m.isBoundaryEdge(e))
                return false;
    return true;
}
} // namespace

static PoleResult findPolesWithin(const geo::Mesh& source, const QuadRemeshOptions& opts);
static QuadRemeshResult quadRemeshWithin(const geo::Mesh& source, const QuadRemeshOptions& opts);

PoleResult findPoles(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    progress::Scope scope("Finding poles", 1.0, progress::Cancellable);
    Budget budget(opts.timeout, &scope);
    try
    {
        return findPolesWithin(source, opts);
    }
    catch (const Interrupted& e)
    {
        PoleResult out;
        out.error = e.what();
        return out;
    }
}

static PoleResult findPolesWithin(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    PoleResult out;
    std::vector<std::pair<std::string, double>> timings;
    Stopwatch clock(timings);
    Work w;
    prepareWork(w, source, opts, clock);
    if (!w.error.empty())
    {
        out.error = w.error;
        return out;
    }
    std::vector<glm::dvec3> q = crossField(w, opts, out.error);
    if (!out.ok())
        return out;
    for (const FieldSingularity& f : fieldSingularities(w.s, q))
    {
        const auto& tri = w.s.tris[f.tri];
        glm::dvec3 c = (w.s.pos[tri[0]] + w.s.pos[tri[1]] + w.s.pos[tri[2]]) / 3.0;
        out.poles.push_back({c, 4 - f.index});
        if (w.symmetric && std::abs(w.plane.distance(c)) > 1e-9)
        {
            c[opts.symmetryAxis] = -c[opts.symmetryAxis];
            out.poles.push_back({c, 4 - f.index});
        }
    }
    for (int total : shellIndexTotals(w.s).indexTotal)
        out.shellTotals.push_back(total == INT_MIN ? std::optional<int>() : std::optional<int>(total));
    return out;
}

QuadRemeshResult quadRemesh(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    progress::Scope scope("Quad remesh", 1.0, progress::Cancellable);
    Budget budget(opts.timeout, &scope);
    try
    {
        return quadRemeshWithin(source, opts);
    }
    catch (const Interrupted& e)
    {
        QuadRemeshResult out;
        out.error = e.what();
        return out;
    }
}

static QuadRemeshResult quadRemeshWithin(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    QuadRemeshResult result = quadRemeshOnce(source, opts);
    // Pinned points tie the lattice to the source's own spacing; a finer retry would no longer meet them.
    if (source.numFaces() == 0 || !opts.pinnedPoints.empty())
        return result;
    geo::Mesh src = source;
    src.compact();
    const geo::MeshBvh bvh(src);
    bool closed = true;
    for (Index e = 0; e < src.edgeCapacity() && closed; ++e)
        closed = !src.edgeValid(e) || !src.isBoundaryEdge(e);
    if (result.ok() && isClean(result.mesh, src, bvh, closed))
        return result;
    // Too coarse for a thin strip or a tight spot: a finer lattice fits. Keep the first usable result to fall back on.
    double area = 0.0;
    for (Index f = 0; f < src.faceCapacity(); ++f)
        if (src.faceValid(f))
            area += src.faceArea(f);
    QuadRemeshOptions finer = opts;
    double length = opts.targetEdgeLength > 0.0 ? opts.targetEdgeLength : std::sqrt(area / std::max(8, opts.targetFaces));
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        length *= 0.7;
        finer.targetEdgeLength = length;
        QuadRemeshResult again = quadRemeshOnce(source, finer);
        if (again.ok() && isClean(again.mesh, src, bvh, closed))
            return again;
        if (!result.ok() && again.ok())
            result = std::move(again);
    }
    return result;
}
} // namespace lila::retopo
