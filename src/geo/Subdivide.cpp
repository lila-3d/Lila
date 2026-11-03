#include "geo/Ops.h"

#include "core/Profile.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <optional>

namespace lila::geo
{
namespace
{
constexpr Index kGrain = 2048;

/**
 * The subdivided mesh built in parallel: every face's quads laid out in source order and built at once, then their
 * attributes copied face by face. Null when the builder dropped a face, for the face-by-face build to handle.
 */
std::optional<Mesh> buildQuads(const Mesh& src, const std::vector<glm::dvec3>& pos, const std::vector<Index>& pId,
                               const std::vector<Index>& eId, const std::vector<Index>& fId, std::span<const float> crease,
                               SubdivideMap* map)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    // The first quad of each source face: one per corner.
    std::vector<Index> firstQuad(size_t(fc) + 1, 0);
    for (Index f = 0; f < fc; ++f)
        firstQuad[f + 1] = firstQuad[f] + (fId[f] != kInvalid ? Index(src.faceSize(f)) : 0);
    const Index quads = firstQuad[fc];
    std::vector<Index> sizes(quads, 4), corners(size_t(quads) * 4);
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (fId[f] == kInvalid)
                continue;
            Index* out = corners.data() + size_t(firstQuad[f]) * 4;
            const Index first = src.faceCorner(f);
            Index c = first;
            do
            {
                *out++ = pId[src.cornerPoint(c)];
                *out++ = eId[src.cornerEdge(c)];
                *out++ = fId[f];
                *out++ = eId[src.cornerEdge(src.cornerPrev(c))];
                c = src.cornerNext(c);
            } while (c != first);
        }
    });
    Mesh out = Mesh::fromPolygons(pos, sizes, corners);
    if (out.faceCapacity() != quads)
        return std::nullopt;
    for (size_t d = 0; d < size_t(Domain::Count); ++d)
        out.attributes(Domain(d)).mergeLayout(src.attributes(Domain(d)));
    out.materialNames() = src.materialNames();
    out.colorSetInfo() = src.colorSetInfo();

    AttributeSet& pa = out.pointAttrs();
    if (pa.layerCount())
    {
        tbb::parallel_for(Index(0), pc, [&](Index p) {
            if (pId[p] != kInvalid)
                pa.copyFrom(src.pointAttrs(), pId[p], p);
        });
        tbb::parallel_for(Index(0), ec, [&](Index e) {
            if (eId[e] == kInvalid)
                return;
            const Index s[2] = {src.edgePoint(e, 0), src.edgePoint(e, 1)};
            const float w[2] = {0.5f, 0.5f};
            pa.mixFrom(src.pointAttrs(), eId[e], s, w);
        });
        tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kGrain), [&](const tbb::blocked_range<Index>& r) {
            std::vector<Index> s;
            std::vector<float> w;
            for (Index f = r.begin(); f < r.end(); ++f)
                if (fId[f] != kInvalid)
                {
                    s = src.facePoints(f);
                    w.assign(s.size(), 1.0f / float(s.size()));
                    pa.mixFrom(src.pointAttrs(), fId[f], s, w);
                }
        });
    }
    if (map)
    {
        map->points = pId;
        map->facePoints = fId;
        map->edges.assign(out.edgeCapacity(), kInvalid);
        map->faces.assign(out.faceCapacity(), kInvalid);
    }
    const bool hasCornerAttrs = out.cornerAttrs().layerCount() > 0, hasEdgeAttrs = out.edgeAttrs().layerCount() > 0;
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kGrain), [&](const tbb::blocked_range<Index>& r) {
        std::vector<Index> faceCorners;
        std::vector<float> avg;
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (fId[f] == kInvalid)
                continue;
            faceCorners = src.faceCorners(f);
            const size_t n = faceCorners.size();
            avg.assign(n, 1.0f / float(n));
            for (size_t i = 0; i < n; ++i)
            {
                const Index c = faceCorners[i];
                const Index cp = faceCorners[(i + n - 1) % n];
                const Index cn = faceCorners[(i + 1) % n];
                const Index nf = firstQuad[f] + Index(i);
                out.faceAttrs().copyFrom(src.faceAttrs(), nf, f);
                const Index oc = out.faceCorner(nf);
                // Each half of a source edge sits in a quad of every face on that edge; the edge's own first face writes it.
                auto outer = [&](Index outEdge, Index srcEdge) {
                    if (src.cornerFace(src.edgeCorner(srcEdge)) != f)
                        return;
                    if (hasEdgeAttrs)
                        out.edgeAttrs().copyFrom(src.edgeAttrs(), outEdge, srcEdge);
                    if (map)
                        map->edges[outEdge] = srcEdge;
                };
                outer(out.cornerEdge(oc), src.cornerEdge(c));
                outer(out.cornerEdge(out.cornerPrev(oc)), src.cornerEdge(cp));
                if (map)
                    map->faces[nf] = f;
                if (hasCornerAttrs)
                {
                    out.cornerAttrs().copyFrom(src.cornerAttrs(), oc, c);
                    const Index a[2] = {c, cn};
                    const Index b[2] = {cp, c};
                    const float half[2] = {0.5f, 0.5f};
                    out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(oc), a, half);
                    out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(out.cornerNext(oc)), faceCorners, avg);
                    out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerPrev(oc), b, half);
                }
            }
        }
    });
    if (!crease.empty())
    {
        std::span<float> cr = out.edgeAttrs().get<float>(attr::kCrease);
        for (float& v : cr)
            v = std::max(0.0f, v - 1.0f);
    }
    return out;
}

Mesh subdivideOnce(const Mesh& src, const SubdivideOptions& opts, SubdivideMap* map = nullptr)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    std::span<const float> crease = src.edgeAttrs().get<float>(attr::kCrease);
    auto sharpness = [&](Index e) -> double {
        if (!src.isManifoldEdge(e))
            return 1.0;
        return crease.empty() ? 0.0 : std::clamp(double(crease[e]), 0.0, 1.0);
    };

    // Dense ids: points, then edges, then faces.
    std::vector<Index> pId(pc, kInvalid), eId(ec, kInvalid), fId(fc, kInvalid);
    Index next = 0;
    for (Index p = 0; p < pc; ++p)
        if (src.pointValid(p))
            pId[p] = next++;
    for (Index e = 0; e < ec; ++e)
        if (src.edgeValid(e))
            eId[e] = next++;
    for (Index f = 0; f < fc; ++f)
        if (src.faceValid(f))
            fId[f] = next++;
    std::vector<glm::dvec3> pos(next);

    std::vector<glm::dvec3> facePt(fc);
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
            if (src.faceValid(f))
                pos[fId[f]] = facePt[f] = src.faceCentroid(f);
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, ec, kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index e = r.begin(); e < r.end(); ++e)
        {
            if (!src.edgeValid(e))
                continue;
            const glm::dvec3 mid = (src.position(src.edgePoint(e, 0)) + src.position(src.edgePoint(e, 1))) * 0.5;
            const double s = sharpness(e);
            glm::dvec3 smooth = mid;
            if (s < 1.0)
            {
                const Index c = src.edgeCorner(e);
                const glm::dvec3 faces = (facePt[src.cornerFace(c)] + facePt[src.cornerFace(src.cornerRadial(c))]) * 0.5;
                smooth = (mid + faces) * 0.5;
            }
            pos[eId[e]] = glm::mix(smooth, mid, s);
        }
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!src.pointValid(p))
                continue;
            const glm::dvec3 P = src.position(p);
            glm::dvec3 R(0.0), F(0.0), creaseSum(0.0);
            int n = 0, nf = 0, nSharp = 0;
            double sharpSum = 0.0;
            src.forEachPointEdge(p, [&](Index e) {
                const glm::dvec3 q = src.position(src.edgeOtherPoint(e, p));
                R += (P + q) * 0.5;
                ++n;
                const double s = sharpness(e);
                if (s > 0.0)
                {
                    ++nSharp;
                    sharpSum += s;
                    creaseSum += q;
                }
            });
            src.forEachPointCorner(p, [&](Index c) {
                F += facePt[src.cornerFace(c)];
                ++nf;
            });
            if (n == 0 || nf == 0)
            {
                pos[pId[p]] = P;
                continue;
            }
            const glm::dvec3 smooth = n >= 3 && nf == n ? (F / double(nf) + 2.0 * R / double(n) + double(n - 3) * P) / double(n) : P;
            glm::dvec3 result = smooth;
            const bool boundaryCorner = nf < n && n == 2;
            if (nSharp == 2 && !(boundaryCorner && opts.sharpBoundary))
                result = glm::mix(smooth, (6.0 * P + creaseSum) / 8.0, sharpSum / 2.0);
            else if (nSharp > 2 || (boundaryCorner && opts.sharpBoundary))
                result = glm::mix(smooth, P, nSharp ? std::min(1.0, sharpSum / nSharp) : 1.0);
            pos[pId[p]] = result;
        }
    });

    if (std::optional<Mesh> built = buildQuads(src, pos, pId, eId, fId, crease, map))
        return std::move(*built);

    Mesh out;
    for (size_t d = 0; d < size_t(Domain::Count); ++d)
        out.attributes(Domain(d)).mergeLayout(src.attributes(Domain(d)));
    out.materialNames() = src.materialNames();
    out.colorSetInfo() = src.colorSetInfo();
    out.reserve(next, size_t(src.numEdges()) * 2 + src.numCorners(), src.numCorners(), size_t(src.numCorners()) * 4);
    for (const glm::dvec3& p : pos)
        out.addPoint(p);

    AttributeSet& pa = out.pointAttrs();
    if (pa.layerCount())
    {
        for (Index p = 0; p < pc; ++p)
            if (pId[p] != kInvalid)
                pa.copyFrom(src.pointAttrs(), pId[p], p);
        for (Index e = 0; e < ec; ++e)
            if (eId[e] != kInvalid)
            {
                const Index s[2] = {src.edgePoint(e, 0), src.edgePoint(e, 1)};
                const float w[2] = {0.5f, 0.5f};
                pa.mixFrom(src.pointAttrs(), eId[e], s, w);
            }
        std::vector<Index> s;
        std::vector<float> w;
        for (Index f = 0; f < fc; ++f)
            if (fId[f] != kInvalid)
            {
                s = src.facePoints(f);
                w.assign(s.size(), 1.0f / float(s.size()));
                pa.mixFrom(src.pointAttrs(), fId[f], s, w);
            }
    }

    std::span<const float> srcCrease = crease;
    const bool hasCornerAttrs = out.cornerAttrs().layerCount() > 0;
    if (map)
    {
        map->points = pId;
        map->facePoints = fId;
        map->edges.clear();
        map->faces.clear();
    }
    std::vector<Index> corners;
    std::vector<float> avg;
    for (Index f = 0; f < fc; ++f)
    {
        if (fId[f] == kInvalid)
            continue;
        corners = src.faceCorners(f);
        const size_t n = corners.size();
        avg.assign(n, 1.0f / float(n));
        for (size_t i = 0; i < n; ++i)
        {
            const Index c = corners[i];
            const Index cp = corners[(i + n - 1) % n];
            const Index cn = corners[(i + 1) % n];
            const Index quad[4] = {pId[src.cornerPoint(c)], eId[src.cornerEdge(c)], fId[f], eId[src.cornerEdge(cp)]};
            const Index edgeCap = out.edgeCapacity();
            const Index nf = out.addFace(quad);
            if (nf == kInvalid)
                continue;
            out.faceAttrs().copyFrom(src.faceAttrs(), nf, f);
            Index oc = out.faceCorner(nf);
            // Outer sub-edges come from the original edges; the two inner ones are new and plain.
            const Index e0 = out.cornerEdge(oc);
            const Index e3 = out.cornerEdge(out.cornerPrev(oc));
            if (map)
            {
                map->edges.resize(out.edgeCapacity(), kInvalid);
                map->edges[e0] = src.cornerEdge(c);
                map->edges[e3] = src.cornerEdge(cp);
                map->faces.resize(out.faceCapacity(), kInvalid);
                map->faces[nf] = f;
            }
            if (e0 >= edgeCap)
                out.edgeAttrs().copyFrom(src.edgeAttrs(), e0, src.cornerEdge(c));
            if (e3 >= edgeCap)
                out.edgeAttrs().copyFrom(src.edgeAttrs(), e3, src.cornerEdge(cp));
            if (hasCornerAttrs)
            {
                out.cornerAttrs().copyFrom(src.cornerAttrs(), oc, c);
                const Index a[2] = {c, cn};
                const Index b[2] = {cp, c};
                const float half[2] = {0.5f, 0.5f};
                out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(oc), a, half);
                out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(out.cornerNext(oc)), corners, avg);
                out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerPrev(oc), b, half);
            }
        }
    }
    if (!srcCrease.empty())
    {
        std::span<float> cr = out.edgeAttrs().get<float>(attr::kCrease);
        for (float& v : cr)
            v = std::max(0.0f, v - 1.0f);
    }
    return out;
}
} // namespace

Mesh subdivide(const Mesh& mesh, const SubdivideOptions& opts, SubdivideMap* map)
{
    LILA_ZONE();

    Mesh cur = mesh;
    for (int i = 0; i < opts.levels; ++i)
    {
        if (!map)
        {
            cur = subdivideOnce(cur, opts);
            continue;
        }
        SubdivideMap once;
        cur = subdivideOnce(cur, opts, &once);
        if (i == 0)
        {
            *map = std::move(once);
            continue;
        }
        // Compose: the cage's points and face centres stay put, and each new piece keeps its parent's cage edge or face.
        for (Index& p : map->points)
            if (p != kInvalid)
                p = once.points[p];
        for (Index& p : map->facePoints)
            if (p != kInvalid)
                p = once.points[p];
        for (Index& e : once.edges)
            if (e != kInvalid)
                e = map->edges[e];
        for (Index& f : once.faces)
            if (f != kInvalid)
                f = map->faces[f];
        map->edges = std::move(once.edges);
        map->faces = std::move(once.faces);
    }
    if (!opts.keepHardEdges && opts.levels > 0)
    {
        cur.edgeAttrs().remove(attr::kHard);
        cur.cornerAttrs().remove(attr::kNormal);
    }
    if (map)
    {
        map->edges.resize(cur.edgeCapacity(), kInvalid);
        map->faces.resize(cur.faceCapacity(), kInvalid);
    }
    return cur;
}

std::vector<glm::dvec3> PointStencil::apply(std::span<const glm::dvec3> in) const
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(rows());
    tbb::parallel_for(tbb::blocked_range<Index>(0, rows(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index i = r.begin(); i < r.end(); ++i)
        {
            glm::dvec3 sum(0.0);
            for (Index k = offsets[i]; k < offsets[i + 1]; ++k)
                sum += in[cols[k]] * weights[k];
            out[i] = sum;
        }
    });
    return out;
}

namespace
{
/** subdivideOnce's position rules as weights instead of positions. Must stay in step with it. */
PointStencil stencilOnce(const Mesh& src, const SubdivideOptions& opts)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    std::span<const float> crease = src.edgeAttrs().get<float>(attr::kCrease);
    auto sharpness = [&](Index e) -> double {
        if (!src.isManifoldEdge(e))
            return 1.0;
        return crease.empty() ? 0.0 : std::clamp(double(crease[e]), 0.0, 1.0);
    };

    using Terms = std::vector<std::pair<Index, double>>;
    auto addFace = [&](Terms& t, Index f, double w) {
        const double each = w / double(src.faceSize(f));
        src.forEachFaceCorner(f, [&](Index c) { t.push_back({src.cornerPoint(c), each}); });
    };

    std::vector<Terms> rows;
    rows.reserve(size_t(src.numPoints()) + src.numEdges() + src.numFaces());
    for (Index p = 0; p < pc; ++p)
    {
        if (!src.pointValid(p))
            continue;
        Terms t;
        int n = 0, nf = 0, nSharp = 0;
        double sharpSum = 0.0;
        Terms creaseNbrs;
        src.forEachPointEdge(p, [&](Index e) {
            ++n;
            const double s = sharpness(e);
            if (s > 0.0)
            {
                ++nSharp;
                sharpSum += s;
                creaseNbrs.push_back({src.edgeOtherPoint(e, p), 1.0});
            }
        });
        src.forEachPointCorner(p, [&](Index) { ++nf; });
        if (n == 0 || nf == 0)
        {
            rows.push_back({{p, 1.0}});
            continue;
        }
        // smooth = (F/nf + 2R/n + (n-3)P) / n, R the edge midpoints' sum and F the face points' sum.
        Terms smooth;
        if (n >= 3 && nf == n)
        {
            const double inv = 1.0 / double(n);
            src.forEachPointCorner(p, [&](Index c) { addFace(smooth, src.cornerFace(c), inv / double(nf)); });
            src.forEachPointEdge(p, [&](Index e) {
                smooth.push_back({p, inv * inv});
                smooth.push_back({src.edgeOtherPoint(e, p), inv * inv});
            });
            smooth.push_back({p, double(n - 3) * inv});
        }
        else
            smooth.push_back({p, 1.0});
        const bool boundaryCorner = nf < n && n == 2;
        double mix = 0.0;
        Terms sharp;
        if (nSharp == 2 && !(boundaryCorner && opts.sharpBoundary))
        {
            mix = sharpSum / 2.0;
            sharp.push_back({p, 6.0 / 8.0});
            for (auto& [q, w] : creaseNbrs)
                sharp.push_back({q, 1.0 / 8.0});
        }
        else if (nSharp > 2 || (boundaryCorner && opts.sharpBoundary))
        {
            mix = nSharp ? std::min(1.0, sharpSum / nSharp) : 1.0;
            sharp.push_back({p, 1.0});
        }
        for (auto& [q, w] : smooth)
            t.push_back({q, w * (1.0 - mix)});
        for (auto& [q, w] : sharp)
            t.push_back({q, w * mix});
        rows.push_back(std::move(t));
    }
    for (Index e = 0; e < ec; ++e)
    {
        if (!src.edgeValid(e))
            continue;
        const double s = sharpness(e);
        Terms t{{src.edgePoint(e, 0), 0.5 * s}, {src.edgePoint(e, 1), 0.5 * s}};
        if (s < 1.0)
        {
            const double k = 1.0 - s;
            t.push_back({src.edgePoint(e, 0), 0.25 * k});
            t.push_back({src.edgePoint(e, 1), 0.25 * k});
            const Index c = src.edgeCorner(e);
            addFace(t, src.cornerFace(c), 0.25 * k);
            addFace(t, src.cornerFace(src.cornerRadial(c)), 0.25 * k);
        }
        rows.push_back(std::move(t));
    }
    for (Index f = 0; f < fc; ++f)
        if (src.faceValid(f))
        {
            Terms t;
            addFace(t, f, 1.0);
            rows.push_back(std::move(t));
        }

    PointStencil out;
    out.inputs = pc;
    for (Terms& t : rows)
    {
        std::sort(t.begin(), t.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < t.size();)
        {
            size_t j = i;
            double w = 0.0;
            for (; j < t.size() && t[j].first == t[i].first; ++j)
                w += t[j].second;
            if (w != 0.0)
            {
                out.cols.push_back(t[i].first);
                out.weights.push_back(w);
            }
            i = j;
        }
        out.offsets.push_back(Index(out.cols.size()));
    }
    return out;
}
} // namespace

std::vector<PointStencil> subdivideStencils(const Mesh& mesh, const SubdivideOptions& opts, Mesh* result)
{
    LILA_ZONE();

    std::vector<PointStencil> out;
    Mesh cur = mesh;
    for (int i = 0; i < opts.levels; ++i)
    {
        out.push_back(stencilOnce(cur, opts));
        cur = subdivideOnce(cur, opts);
    }
    if (result)
        *result = std::move(cur);
    return out;
}
} // namespace lila::geo
