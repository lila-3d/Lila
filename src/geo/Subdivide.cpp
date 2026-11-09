#include "geo/Ops.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <bit>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>

namespace lila::geo
{
namespace
{
constexpr Index kRefineGrain = 2048;

/**
 * One uv layer refined like the positions, within each uv island: per source corner the uv at its point and at the
 * middle of its outgoing edge, and per face the uv at its centre. Seams, mesh borders and creases are island borders,
 * smoothed along themselves like a crease; a point whose island has only one face around it keeps its uv.
 */
struct UvRefine
{
    std::string name;
    std::vector<glm::vec2> point, edge, face;
};

/**
 * The layer whose islands a uv layer is refined within: its own, or for a layer named "<anything>@<set>" the Float2
 * layer <set>, so a layer derived from a uv set is smoothed exactly as that set is.
 */
const Attribute& islandLayer(const Mesh& src, const Attribute& layer)
{
    const std::string& name = layer.name();
    const size_t at = name.rfind('@');
    if (at == std::string::npos)
        return layer;
    const Attribute* set = src.cornerAttrs().find(std::string_view(name).substr(at + 1));
    return set && set->type() == AttrType::Float2 ? *set : layer;
}

template <class Sharpness>
UvRefine refineUvs(const Mesh& src, const Attribute& layer, const Attribute& islands, Sharpness&& sharpness, bool smoothCorners)
{
    LILA_ZONE();

    const std::span<const glm::vec2> uv = layer.values<glm::vec2>();
    UvRefine r{layer.name(), std::vector<glm::vec2>(src.cornerCapacity()), std::vector<glm::vec2>(src.cornerCapacity()),
               std::vector<glm::vec2>(src.faceCapacity())};
    const Index fc = src.faceCapacity(), pc = src.pointCapacity();
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& rg) {
        for (Index f = rg.begin(); f < rg.end(); ++f)
            if (src.faceValid(f))
            {
                glm::vec2 sum(0.0f);
                int n = 0;
                src.forEachFaceCorner(f, [&](Index c) {
                    sum += uv[c];
                    ++n;
                });
                r.face[f] = sum / float(n);
            }
    });
    const std::span<const glm::vec2> key = islands.values<glm::vec2>();
    auto same = [&](Index a, Index b) { return glm::all(glm::lessThanEqual(glm::abs(key[a] - key[b]), glm::vec2(1e-6f))); };
    // The corner at point p of the face holding corner c.
    auto cornerAt = [&](Index c, Index p) { return src.cornerPoint(c) == p ? c : src.cornerNext(c); };
    // Whether e joins its two faces' uvs, so it lies inside an island.
    auto joins = [&](Index e) {
        if (!src.isManifoldEdge(e) || sharpness(e) > 0.0)
            return false;
        const Index c = src.edgeCorner(e), rc = src.cornerRadial(c);
        const Index p0 = src.edgePoint(e, 0), p1 = src.edgePoint(e, 1);
        return same(cornerAt(c, p0), cornerAt(rc, p0)) && same(cornerAt(c, p1), cornerAt(rc, p1));
    };
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& rg) {
        for (Index f = rg.begin(); f < rg.end(); ++f)
            if (src.faceValid(f))
                src.forEachFaceCorner(f, [&](Index c) {
                    const Index e = src.cornerEdge(c);
                    const glm::vec2 mid = (uv[c] + uv[src.cornerNext(c)]) * 0.5f;
                    if (!joins(e))
                    {
                        r.edge[c] = mid;
                        return;
                    }
                    const Index ec = src.edgeCorner(e);
                    const Index f2 = src.cornerFace(ec) == f ? src.cornerFace(src.cornerRadial(ec)) : src.cornerFace(ec);
                    r.edge[c] = (mid + (r.face[f] + r.face[f2]) * 0.5f) * 0.5f;
                });
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kRefineGrain), [&](const tbb::blocked_range<Index>& rg) {
        std::vector<Index> corners, island, stack;
        std::vector<uint8_t> done;
        for (Index p = rg.begin(); p < rg.end(); ++p)
        {
            if (!src.pointValid(p))
                continue;
            corners.clear();
            src.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
            done.assign(corners.size(), 0);
            for (size_t start = 0; start < corners.size(); ++start)
            {
                if (done[start])
                    continue;
                // The corners at p in one island, reached across the edges at p that join their faces.
                island.clear();
                stack.assign(1, corners[start]);
                done[start] = 1;
                while (!stack.empty())
                {
                    const Index c = stack.back();
                    stack.pop_back();
                    island.push_back(c);
                    for (Index e : {src.cornerEdge(c), src.cornerEdge(src.cornerPrev(c))})
                    {
                        if (!joins(e))
                            continue;
                        const Index ec = src.edgeCorner(e);
                        const Index nb = cornerAt(src.cornerFace(ec) == src.cornerFace(c) ? src.cornerRadial(ec) : ec, p);
                        const auto it = std::ranges::find(corners, nb);
                        if (it != corners.end() && !done[size_t(it - corners.begin())])
                        {
                            done[size_t(it - corners.begin())] = 1;
                            stack.push_back(nb);
                        }
                    }
                }
                const glm::vec2 P = uv[island.front()];
                glm::vec2 F(0.0f), R(0.0f), rim(0.0f);
                int inner = 0, borders = 0;
                for (Index c : island)
                {
                    F += r.face[src.cornerFace(c)];
                    const Index cn = src.cornerNext(c), cp = src.cornerPrev(c);
                    for (auto [e, q] : {std::pair{src.cornerEdge(c), cn}, std::pair{src.cornerEdge(cp), cp}})
                        if (joins(e))
                        {
                            // Met from both of its faces, so each sighting adds half.
                            R += (P + uv[q]) * 0.25f;
                            ++inner;
                        }
                        else
                        {
                            rim += uv[q];
                            ++borders;
                        }
                }
                const float n = float(island.size());
                glm::vec2 result = P;
                if (borders == 0 && island.size() >= 3)
                    result = (F / n + 2.0f * R / (float(inner) * 0.5f) + (n - 3.0f) * P) / n;
                else if (borders == 2 && (island.size() >= 2 || smoothCorners))
                    result = (6.0f * P + rim) / 8.0f;
                for (Index c : island)
                    r.point[c] = result;
            }
        }
    });
    return r;
}

/** Writes the refined uvs onto the quad of source corner c, whose first corner is oc. */
void writeUvs(Mesh& out, const Mesh& src, std::span<const UvRefine> uvs, Index oc, Index c)
{
    for (const UvRefine& u : uvs)
    {
        const std::span<glm::vec2> v = out.cornerAttrs().get<glm::vec2>(u.name);
        if (v.empty())
            continue;
        v[oc] = u.point[c];
        v[out.cornerNext(oc)] = u.edge[c];
        v[out.cornerNext(out.cornerNext(oc))] = u.face[src.cornerFace(c)];
        v[out.cornerPrev(oc)] = u.edge[src.cornerPrev(c)];
    }
}

/**
 * The subdivided mesh built in parallel: every face's quads laid out in source order and built at once, then their
 * attributes copied face by face. Null when the builder dropped a face, for the face-by-face build to handle.
 */
std::optional<Mesh> buildQuads(const Mesh& src, const std::vector<glm::dvec3>& pos, const std::vector<Index>& pId,
                               const std::vector<Index>& eId, const std::vector<Index>& fId, std::span<const float> crease,
                               std::span<const UvRefine> uvs, SubdivideMap* map)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    // The first quad of each source face: one per corner.
    std::vector<Index> firstQuad(size_t(fc) + 1, 0);
    for (Index f = 0; f < fc; ++f)
        firstQuad[f + 1] = firstQuad[f] + (fId[f] != kInvalid ? Index(src.faceSize(f)) : 0);
    const Index quads = firstQuad[fc];
    std::vector<Index> sizes(quads, 4), corners(size_t(quads) * 4);
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
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
        tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
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
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
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
                    writeUvs(out, src, uvs, oc, c);
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
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
            if (src.faceValid(f))
                pos[fId[f]] = facePt[f] = src.faceCentroid(f);
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, ec, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
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
    tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
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

    std::vector<UvRefine> uvs;
    for (size_t l = 0; l < src.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = src.cornerAttrs().layer(l); a.type() == AttrType::Float2)
            uvs.push_back(refineUvs(src, a, islandLayer(src, a), sharpness, opts.uvCorners == SubdivideUvCorners::Smooth));

    if (std::optional<Mesh> built = buildQuads(src, pos, pId, eId, fId, crease, uvs, map))
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
                writeUvs(out, src, uvs, oc, c);
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

namespace fit
{
using SpMat = Eigen::SparseMatrix<double>;

/**
 * A mesh's uv islands for one island layer: its corners grouped per point within an island (the points of the island's
 * own surface), each group on the island's border or inside it.
 */
struct Islands
{
    std::vector<Index> groupOfCorner;
    std::vector<uint8_t> border;
    std::vector<Index> islandOfGroup;
    std::vector<std::vector<Index>> groupCorners;
    std::vector<std::vector<Index>> faces; // per island
};

Islands islandsOf(const Mesh& m, const Attribute& layer)
{
    LILA_ZONE();

    const std::span<const glm::vec2> key = layer.values<glm::vec2>();
    auto same = [&](Index a, Index b) { return glm::all(glm::lessThanEqual(glm::abs(key[a] - key[b]), glm::vec2(1e-6f))); };
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    auto joins = [&](Index e) {
        if (!m.isManifoldEdge(e))
            return false;
        const Index c = m.edgeCorner(e), rc = m.cornerRadial(c);
        const Index p0 = m.edgePoint(e, 0), p1 = m.edgePoint(e, 1);
        return same(cornerAt(c, p0), cornerAt(rc, p0)) && same(cornerAt(c, p1), cornerAt(rc, p1));
    };
    Islands is;
    is.groupOfCorner.assign(m.cornerCapacity(), kInvalid);
    std::vector<Index> corners, stack;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        corners.clear();
        m.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
        for (Index start : corners)
        {
            if (is.groupOfCorner[start] != kInvalid)
                continue;
            const Index g = Index(is.border.size());
            is.border.push_back(0);
            is.groupCorners.emplace_back();
            is.groupOfCorner[start] = g;
            stack.assign(1, start);
            while (!stack.empty())
            {
                const Index c = stack.back();
                stack.pop_back();
                is.groupCorners[g].push_back(c);
                for (Index e : {m.cornerEdge(c), m.cornerEdge(m.cornerPrev(c))})
                {
                    if (!joins(e))
                    {
                        is.border[g] = 1;
                        continue;
                    }
                    const Index ec = m.edgeCorner(e);
                    const Index nb = cornerAt(m.cornerFace(ec) == m.cornerFace(c) ? m.cornerRadial(ec) : ec, p);
                    if (is.groupOfCorner[nb] == kInvalid)
                    {
                        is.groupOfCorner[nb] = g;
                        stack.push_back(nb);
                    }
                }
            }
        }
    }
    // Islands: groups joined through the faces they share.
    std::vector<Index> parent(is.border.size());
    for (Index g = 0; g < parent.size(); ++g)
        parent[g] = g;
    auto root = [&](Index g) {
        while (parent[g] != g)
            g = parent[g] = parent[parent[g]];
        return g;
    };
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const Index a = root(is.groupOfCorner[m.faceCorner(f)]);
            m.forEachFaceCorner(f, [&](Index c) {
                const Index b = root(is.groupOfCorner[c]);
                if (b != a)
                    parent[b] = a;
            });
        }
    std::vector<Index> id(parent.size(), kInvalid);
    is.islandOfGroup.resize(parent.size());
    for (Index g = 0; g < parent.size(); ++g)
    {
        Index& i = id[root(g)];
        if (i == kInvalid)
        {
            i = Index(is.faces.size());
            is.faces.emplace_back();
        }
        is.islandOfGroup[g] = i;
    }
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            is.faces[is.islandOfGroup[is.groupOfCorner[m.faceCorner(f)]]].push_back(f);
    return is;
}

/** One island's interior system: the cotan Laplacian over its interior groups and their couplings to its border. */
struct System
{
    std::vector<Index> interior; // groups, in unknown order
    std::vector<Index> border;   // groups
    SpMat a;                     // interior x interior
    SpMat b;                     // interior x border: rhs = b * border values
};

struct Factor
{
    Eigen::SimplicialLDLT<SpMat> ldlt;
    bool ok = false;
};

/** Factorisations by matrix content, so re-evaluating an unchanged surface (new uvs, more probe layers) skips them. */
class FactorCache
{
public:
    std::shared_ptr<const Factor> get(const SpMat& a)
    {
        const uint64_t key = hashOf(a);
        {
            std::lock_guard lock(mMutex);
            for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
                if (it->key == key && it->n == a.rows() && it->nnz == a.nonZeros())
                {
                    mEntries.splice(mEntries.begin(), mEntries, it);
                    return it->factor;
                }
        }
        auto f = std::make_shared<Factor>();
        f->ldlt.compute(a);
        f->ok = f->ldlt.info() == Eigen::Success;
        std::lock_guard lock(mMutex);
        mEntries.push_front({key, a.rows(), a.nonZeros(), f});
        mNonZeros += size_t(a.nonZeros());
        while (mEntries.size() > 1 && (mNonZeros > kBudget || mEntries.size() > kMaxEntries))
        {
            mNonZeros -= size_t(mEntries.back().nnz);
            mEntries.pop_back();
        }
        return f;
    }

private:
    static constexpr size_t kBudget = size_t(8) << 20;
    static constexpr size_t kMaxEntries = 4096;

    static uint64_t hashOf(const SpMat& a)
    {
        uint64_t h = 0x9e3779b97f4a7c15ull ^ uint64_t(a.rows());
        auto mix = [&](uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h *= 0xff51afd7ed558ccdull;
        };
        for (Eigen::Index k = 0; k <= a.outerSize(); ++k)
            mix(uint64_t(a.outerIndexPtr()[k]));
        for (Eigen::Index k = 0; k < a.nonZeros(); ++k)
        {
            mix(uint64_t(a.innerIndexPtr()[k]));
            mix(std::bit_cast<uint64_t>(a.valuePtr()[k]));
        }
        return h;
    }

    struct Entry
    {
        uint64_t key;
        Eigen::Index n, nnz;
        std::shared_ptr<const Factor> factor;
    };
    std::mutex mMutex;
    std::list<Entry> mEntries;
    size_t mNonZeros = 0;
};

FactorCache& factorCache()
{
    // Never destroyed: meshes may still be subdivided while statics shut down.
    static FactorCache& c = *new FactorCache();
    return c;
}

/** Cotan weights of the island's faces on the mesh's surface; a quad averages both of its diagonals. */
System systemOf(const Mesh& m, const Islands& is, Index island)
{
    System s;
    std::map<Index, Index> unknown, known;
    for (Index f : is.faces[island])
        m.forEachFaceCorner(f, [&](Index c) {
            const Index g = is.groupOfCorner[c];
            auto& to = is.border[g] ? known : unknown;
            if (to.emplace(g, Index(to.size())).second)
                (is.border[g] ? s.border : s.interior).push_back(g);
        });
    if (s.interior.empty() || s.border.empty())
        return s;
    std::vector<Eigen::Triplet<double>> ta, tb;
    auto half = [&](Index g, Index o, double w) {
        if (is.border[g])
            return;
        const int r = int(unknown[g]);
        ta.emplace_back(r, r, w);
        if (is.border[o])
            tb.emplace_back(r, int(known[o]), w);
        else
            ta.emplace_back(r, int(unknown[o]), -w);
    };
    auto addTri = [&](Index c0, Index c1, Index c2, double tw) {
        const Index cs[3] = {c0, c1, c2};
        for (int k = 0; k < 3; ++k)
        {
            const Index ci = cs[(k + 1) % 3], cj = cs[(k + 2) % 3];
            const glm::dvec3 pk = m.position(m.cornerPoint(cs[k]));
            const glm::dvec3 u = m.position(m.cornerPoint(ci)) - pk, v = m.position(m.cornerPoint(cj)) - pk;
            const double sn = glm::length(glm::cross(u, v));
            if (sn <= 1e-300)
                continue;
            const double w = 0.5 * tw * glm::dot(u, v) / sn;
            const Index gi = is.groupOfCorner[ci], gj = is.groupOfCorner[cj];
            half(gi, gj, w);
            half(gj, gi, w);
        }
    };
    std::vector<Index> fc;
    for (Index f : is.faces[island])
    {
        fc = m.faceCorners(f);
        if (fc.size() == 4)
        {
            addTri(fc[0], fc[1], fc[2], 0.5);
            addTri(fc[0], fc[2], fc[3], 0.5);
            addTri(fc[1], fc[2], fc[3], 0.5);
            addTri(fc[1], fc[3], fc[0], 0.5);
        }
        else
            for (size_t i = 1; i + 1 < fc.size(); ++i)
                addTri(fc[0], fc[i], fc[i + 1], 1.0);
    }
    const auto n = Eigen::Index(s.interior.size());
    s.a.resize(n, n);
    s.a.setFromTriplets(ta.begin(), ta.end());
    s.b.resize(n, Eigen::Index(s.border.size()));
    s.b.setFromTriplets(tb.begin(), tb.end());
    return s;
}

/** A layer's value at a group, whose corners agree up to float noise. */
glm::dvec2 groupValue(std::span<const glm::vec2> uv, const std::vector<Index>& corners)
{
    glm::dvec2 sum(0.0);
    for (Index c : corners)
        sum += glm::dvec2(uv[c]);
    return sum / double(corners.size());
}

/** A border group's new value: the blend of two border groups' values at t. */
struct BorderBlend
{
    Index group, a, b;
    double t;
};

/** Border edges whose spacing proxy is shorter than this share of their surface length are bunched. */
constexpr double kBunched = 0.2;
/** A bunched run takes in its neighbours until the run is at most 1 - this share of the window's surface length. */
constexpr double kRespaced = 0.6;
/** Margin edges per window edge over which a respaced window eases into the spacing beside it. */
constexpr double kRespaceTaper = 1.0;
/** Border edges on each side of a slit's tip that are spaced by their own uv length. */
constexpr size_t kSlitTipReach = 4;

/**
 * Where an island's border bunches up in uv (a bevel's corner points all carry the one cage uv of the corner they
 * replaced, and subdivision keeps uv corners while it rounds the surface), the run and enough of its neighbours spread
 * along the same uv polyline by their length on the surface. Runs, windows and where each point lands on the polyline
 * are measured in the spacing proxy (see addSpacingProxies), so the blends are the same for any layout of the cage
 * (except next to a slit's tip) and a uv tool's map of the export stays exact after it writes. Every layer using the
 * island layer is blended alike.
 */
std::vector<BorderBlend> respaceBorder(const Mesh& m, const Islands& is, Index island, std::span<const glm::vec2> proxy,
                                       std::span<const glm::vec2> set)
{
    std::vector<BorderBlend> out;
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    std::unordered_map<Index, Index> next;
    std::unordered_map<Index, double> spacingFrom;
    std::unordered_set<Index> slitFrom;
    std::vector<Index> fc;
    double a3 = 0.0, auv = 0.0;
    for (Index f : is.faces[island])
    {
        fc = m.faceCorners(f);
        const glm::dvec3 p0 = m.position(m.cornerPoint(fc[0]));
        const glm::dvec2 u0 = set[fc[0]];
        for (size_t k = 1; k + 1 < fc.size(); ++k)
        {
            a3 += 0.5 * glm::length(glm::cross(m.position(m.cornerPoint(fc[k])) - p0, m.position(m.cornerPoint(fc[k + 1])) - p0));
            const glm::dvec2 u1 = glm::dvec2(set[fc[k]]) - u0, u2 = glm::dvec2(set[fc[k + 1]]) - u0;
            auv += 0.5 * (u1.x * u2.y - u1.y * u2.x);
        }
        for (Index c : fc)
        {
            const Index e = m.cornerEdge(c), cn = m.cornerNext(c);
            bool joined = false, slit = false;
            if (m.isManifoldEdge(e))
            {
                const Index ec = m.edgeCorner(e), rc = m.cornerFace(ec) == f ? m.cornerRadial(ec) : ec;
                joined = is.groupOfCorner[cornerAt(rc, m.cornerPoint(c))] == is.groupOfCorner[c] &&
                         is.groupOfCorner[cornerAt(rc, m.cornerPoint(cn))] == is.groupOfCorner[cn];
                slit = is.islandOfGroup[is.groupOfCorner[rc]] == island;
            }
            if (joined)
                continue;
            if (slit)
                slitFrom.insert(is.groupOfCorner[c]);
            // A group with two borders leaving it pinches the island; its loops are left alone.
            if (!next.emplace(is.groupOfCorner[c], is.groupOfCorner[cn]).second)
                return out;
            spacingFrom[is.groupOfCorner[c]] = glm::length(glm::dvec2(proxy[cn]) - glm::dvec2(proxy[c]));
        }
    }
    const double density = std::sqrt(std::abs(auv) / std::max(a3, 1e-300));
    auto pos = [&](Index g) { return m.position(m.cornerPoint(is.groupCorners[g].front())); };
    std::unordered_map<Index, uint8_t> seen;
    std::vector<Index> loop;
    std::vector<double> l, u, r;
    std::vector<uint8_t> slit, ownSpacing;
    std::vector<size_t> dist;
    for (const auto& [first, unused] : next)
    {
        if (seen.count(first))
            continue;
        loop.clear();
        for (Index g = first; !seen.count(g);)
        {
            seen[g] = 1;
            loop.push_back(g);
            const auto it = next.find(g);
            if (it == next.end())
                break;
            g = it->second;
        }
        const size_t n = loop.size();
        if (n < 4 || next.find(loop.back()) == next.end() || next.at(loop.back()) != first)
            continue;
        l.resize(n);
        u.resize(n);
        r.resize(n);
        slit.resize(n);
        bool known = true;
        for (size_t i = 0; i < n; ++i)
        {
            const Index a = loop[i], b = loop[(i + 1) % n];
            l[i] = glm::length(pos(b) - pos(a));
            u[i] = spacingFrom[a];
            known = known && std::isfinite(u[i]);
            r[i] = glm::length(groupValue(set, is.groupCorners[b]) - groupValue(set, is.groupCorners[a]));
            slit[i] = slitFrom.count(a) != 0;
        }
        if (!known)
            continue;
        // A slit's tip (a cut ending inside the island) opens as the layout has it; next to it spacing follows the uvs.
        ownSpacing.assign(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t ip = (i + n - 1) % n, in = (i + 1) % n;
            if (slit[ip] && slit[i] && m.cornerPoint(is.groupCorners[loop[ip]].front()) == m.cornerPoint(is.groupCorners[loop[in]].front()))
                for (size_t k = 0; k < 2 * kSlitTipReach; ++k)
                    ownSpacing[(i + n - kSlitTipReach + k) % n] = 1;
        }
        auto bad = [&](size_t i) { return u[i] < kBunched * l[i]; };
        // Start on the edge farthest from any run, so no window wraps past the scan's start.
        const size_t far = n + 1;
        dist.assign(n, far);
        for (int pass = 0; pass < 2; ++pass)
            for (size_t k = 0; k < 2 * n; ++k)
            {
                const size_t i = pass == 0 ? k % n : (2 * n - 1 - k) % n, j = pass == 0 ? (i + n - 1) % n : (i + 1) % n;
                dist[i] = bad(i) ? 0 : std::min(dist[i], dist[j] == far ? far : dist[j] + 1);
            }
        size_t start = 0;
        for (size_t i = 1; i < n; ++i)
            if (dist[i] > dist[start])
                start = i;
        if (dist[start] == 0 || dist[start] == far)
            continue;
        // Edges are counted from start from here on.
        auto at = [&](size_t k) { return (start + k) % n; };
        std::vector<uint8_t> covered(n, 0);
        covered[0] = 1;
        std::vector<std::pair<size_t, size_t>> windows;
        for (size_t k = 1; k < n; ++k)
        {
            if (covered[k] || !bad(at(k)))
                continue;
            // Edges lo..hi, grown both ways over uncovered edges until spaced enough.
            size_t lo = k, hi = k;
            double L = l[at(k)], U = u[at(k)];
            while (U < kRespaced * L)
            {
                const bool back = lo > 1 && !covered[lo - 1], fwd = hi + 1 < n && !covered[hi + 1];
                if (!back && !fwd)
                    break;
                if (back)
                {
                    --lo;
                    L += l[at(lo)];
                    U += u[at(lo)];
                }
                if (fwd)
                {
                    ++hi;
                    L += l[at(hi)];
                    U += u[at(hi)];
                }
            }
            for (size_t j = lo; j <= hi; ++j)
                covered[j] = 1;
            windows.push_back({lo, hi});
        }
        std::sort(windows.begin(), windows.end());
        std::vector<double> share, targets, param;
        for (size_t w = 0; w < windows.size(); ++w)
        {
            // Offsets shifted by n, so margins can run back past the start edge (the window before is the last one).
            const size_t lo = windows[w].first + n, hi = windows[w].second + n;
            const size_t prevHi = w > 0 ? windows[w - 1].second + n : windows.back().second;
            const size_t nextLo = w + 1 < windows.size() ? windows[w + 1].first + n : windows.front().first + 2 * n;
            // Each window eases into the spacing beside it over margins of the free edges it shares with its neighbours.
            const size_t width = size_t(double(hi - lo + 1) * kRespaceTaper);
            const size_t mL = std::min(width, (lo - prevHi - 1) / 2), mR = std::min(width, (nextLo - hi) / 2);
            const size_t a = lo - mL, b = hi + mR;
            auto ease = [](double x) { return x * x * (3.0 - 2.0 * x); };
            share.assign(b - a + 1, 1.0);
            for (size_t q = 0; q < mL; ++q)
                share[q] = ease(double(q + 1) / double(mL + 1));
            for (size_t q = 0; q < mR; ++q)
                share[b - a - q] = ease(double(q + 1) / double(mR + 1));
            // The window's edges and each margin edge's share take one spacing p; the rest keep their own spacing.
            param.resize(b - a + 1);
            for (size_t j = a; j <= b; ++j)
                param[j - a] = ownSpacing[at(j)] ? r[at(j)] : density * u[at(j)];
            double U = 0.0, kept = 0.0, eased = 0.0;
            for (size_t j = a; j <= b; ++j)
            {
                U += param[j - a];
                kept += (1.0 - share[j - a]) * param[j - a];
                eased += share[j - a] * l[at(j)];
            }
            if (U <= 0.0 || eased <= 0.0)
                continue;
            const double p = (U - kept) / eased;
            // Vertices a+1..b move to their new distance along the region's uv polyline.
            targets.assign(b - a + 2, 0.0);
            for (size_t v = a + 1; v <= b; ++v)
            {
                const double s = share[v - 1 - a];
                targets[v - a] = targets[v - 1 - a] + (1.0 - s) * param[v - 1 - a] + s * p * l[at(v - 1)];
            }
            targets[b + 1 - a] = U;
            double segStart = 0.0;
            size_t seg = a;
            for (size_t v = a + 1; v <= b; ++v)
            {
                const double target = targets[v - a];
                while (seg < b && segStart + param[seg - a] < target)
                {
                    segStart += param[seg - a];
                    ++seg;
                }
                const double len = param[seg - a];
                const double t = len > 0.0 ? std::clamp((target - segStart) / len, 0.0, 1.0) : 0.0;
                out.push_back({loop[at(v)], loop[at(seg)], loop[at(seg + 1)], t});
            }
        }
    }
    return out;
}

/** An island corner's new value as plain weights of its quad's other three points (probing can't follow u mixed into v). */
struct CornerApex
{
    Index group, a, b, d;
    double wa, wb, wd;
};

/** How much wider than on the surface an island corner may open in uv before it is rebuilt, radians. */
constexpr double kCornerSlack = 1.0;
/** The rebuilt corner rises this share of the surface's height, so it stays close to the border it leaves. */
constexpr double kApexHeight = 0.5;

/**
 * Island corners (one face of the island at the point) that open much wider in uv than on the surface, such as a corner
 * on a straight stretch of border: pinned there, no fill can open the face. Each becomes the apex of the triangle its two
 * border neighbours make with it on the surface, scaled into uv along their edge, written as weights of those two and the
 * quad's opposite point.
 */
std::vector<CornerApex> openCorners(const Mesh& m, const Islands& is, Index island, std::span<const glm::vec2> set)
{
    std::vector<CornerApex> out;
    double area = 0.0;
    for (Index f : is.faces[island])
    {
        const std::vector<Index> fc = m.faceCorners(f);
        for (size_t i = 1; i + 1 < fc.size(); ++i)
        {
            const glm::dvec2 u0 = set[fc[0]], u1 = glm::dvec2(set[fc[i]]) - u0, u2 = glm::dvec2(set[fc[i + 1]]) - u0;
            area += u1.x * u2.y - u1.y * u2.x;
        }
    }
    const double side = area < 0.0 ? -1.0 : 1.0;
    for (Index f : is.faces[island])
        m.forEachFaceCorner(f, [&](Index c) {
            const Index g = is.groupOfCorner[c];
            if (!is.border[g] || is.groupCorners[g].size() != 1 || m.faceSize(m.cornerFace(c)) != 4)
                return;
            const Index ca = m.cornerPrev(c), cb = m.cornerNext(c);
            const glm::dvec3 pa = m.position(m.cornerPoint(ca)), pc = m.position(m.cornerPoint(c)), pb = m.position(m.cornerPoint(cb));
            const glm::dvec3 ab = pb - pa;
            const double ab2 = glm::dot(ab, ab);
            const double ea = glm::length(pa - pc), eb = glm::length(pb - pc);
            if (ab2 <= 0.0 || ea <= 0.0 || eb <= 0.0)
                return;
            const double angle3 = std::acos(std::clamp(glm::dot(pa - pc, pb - pc) / (ea * eb), -1.0, 1.0));
            // The face's angle at the corner in uv, measured inside the island.
            const glm::dvec2 ua = glm::dvec2(set[ca]) - glm::dvec2(set[c]), ub = glm::dvec2(set[cb]) - glm::dvec2(set[c]);
            double angleUv = std::atan2(side * (ub.x * ua.y - ub.y * ua.x), glm::dot(ua, ub));
            if (angleUv < 0.0)
                angleUv += 2.0 * std::numbers::pi;
            if (angleUv <= angle3 + kCornerSlack)
                return;
            const double t = glm::dot(pc - pa, ab) / ab2, h = glm::length(glm::cross(pc - pa, ab)) / ab2 * kApexHeight;
            const Index cd = m.cornerNext(cb);
            const glm::dvec2 a = set[ca], b = set[cb], d = set[cd], e = b - a;
            const glm::dvec2 apex = a + e * t + glm::dvec2(e.y, -e.x) * (side * h);
            // Barycentric weights of the apex in the triangle a, b, d; d must stand clear of the edge to carry them.
            const double det = e.x * (d.y - a.y) - e.y * (d.x - a.x);
            if (std::abs(det) < 1e-3 * glm::dot(e, e))
                return;
            const glm::dvec2 q = apex - a;
            const double wb = (q.x * (d.y - a.y) - q.y * (d.x - a.x)) / det, wd = (e.x * q.y - e.y * q.x) / det;
            out.push_back({g, is.groupOfCorner[ca], is.groupOfCorner[cb], is.groupOfCorner[cd], 1.0 - wb - wd, wb, wd});
        });
    return out;
}

} // namespace fit
} // namespace

struct FitFill::Impl
{
    fit::SpMat b; // interior x border
    std::shared_ptr<const fit::Factor> factor;
};

namespace
{
namespace fit
{
thread_local FitRecorder* tRecorder = nullptr;

/** The island's record for uv tools: its groups, its border groups as blends of the values before the fit, its fill. */
FitIslandRecord recordIsland(const Islands& is, std::span<const BorderBlend> blends, std::span<const CornerApex> apexes, const System& s,
                             std::shared_ptr<const Factor> factor)
{
    FitIslandRecord r;
    std::unordered_map<Index, Index> local;
    auto id = [&](Index g) {
        auto [it, added] = local.emplace(g, Index(r.groups.size()));
        if (added)
            r.groups.push_back(is.groupCorners[g]);
        return it->second;
    };
    for (Index g : s.border)
        id(g);
    for (Index g : s.interior)
        id(g);
    // Respacing reads the values before it, the corners read respaced ones.
    std::unordered_map<Index, std::vector<std::pair<Index, double>>> from;
    auto value = [&](Index g) {
        auto it = from.find(g);
        return it != from.end() ? it->second : std::vector<std::pair<Index, double>>{{id(g), 1.0}};
    };
    std::vector<std::pair<Index, std::vector<std::pair<Index, double>>>> moved;
    for (const BorderBlend& b : blends)
        moved.push_back({b.group, {{id(b.a), 1.0 - b.t}, {id(b.b), b.t}}});
    for (auto& [g, v] : moved)
        from[g] = std::move(v);
    moved.clear();
    for (const CornerApex& x : apexes)
    {
        std::vector<std::pair<Index, double>> v;
        for (auto [g, w] : {std::pair{x.a, x.wa}, std::pair{x.b, x.wb}, std::pair{x.d, x.wd}})
            for (auto [q, wq] : value(g))
                v.push_back({q, w * wq});
        moved.push_back({x.group, std::move(v)});
    }
    for (auto& [g, v] : moved)
        from[g] = std::move(v);
    for (Index g : s.border)
    {
        r.border.push_back(id(g));
        r.borderFrom.push_back(value(g));
    }
    // An island with no interior lists no border groups in its system, but its moved groups still move.
    for (const auto& [g, v] : from)
        if (std::find(s.border.begin(), s.border.end(), g) == s.border.end())
        {
            r.border.push_back(id(g));
            r.borderFrom.push_back(v);
        }
    if (factor && !s.interior.empty())
    {
        for (Index g : s.interior)
            r.interior.push_back(id(g));
        auto impl = std::make_shared<FitFill::Impl>();
        impl->b = s.b;
        impl->factor = std::move(factor);
        r.fill = std::make_shared<const FitFill>(std::move(impl));
    }
    return r;
}

/** Cage border edges with less uv than this share of their surface length at the island's density count by their uv. */
constexpr double kCageThin = 0.5;

/** Per island, uv length per surface length: the square root of its uv area over its surface area. */
std::vector<double> islandDensities(const Mesh& m, const Islands& is, std::span<const glm::vec2> uv)
{
    std::vector<double> out(is.faces.size(), 0.0);
    std::vector<Index> fc;
    for (size_t i = 0; i < is.faces.size(); ++i)
    {
        double a3 = 0.0, auv = 0.0;
        for (Index f : is.faces[i])
        {
            fc = m.faceCorners(f);
            const glm::dvec3 p0 = m.position(m.cornerPoint(fc[0]));
            const glm::dvec2 u0 = uv[fc[0]];
            for (size_t k = 1; k + 1 < fc.size(); ++k)
            {
                a3 += 0.5 * glm::length(glm::cross(m.position(m.cornerPoint(fc[k])) - p0, m.position(m.cornerPoint(fc[k + 1])) - p0));
                const glm::dvec2 u1 = glm::dvec2(uv[fc[k]]) - u0, u2 = glm::dvec2(uv[fc[k + 1]]) - u0;
                auv += 0.5 * (u1.x * u2.y - u1.y * u2.x);
            }
        }
        out[i] = std::sqrt(std::abs(auv) / std::max(a3, 1e-300));
    }
    return out;
}

/** Prefix of the layers that carry each uv island border's surface length through the subdivision. */
constexpr std::string_view kSpacingProxy = "__fitSpacing@";

/**
 * Adds, per island layer, a layer that puts each island border loop's points on a circle as long as the loop is on the
 * surface, at their distance along it (thin cage edges by their uv). Subdivided within the same islands by the same rules,
 * its spacing along the border is the island layer's for an even layout, so respacing finds where the border bunches
 * without reading how the cage is laid out.
 */
void addSpacingProxies(Mesh& m)
{
    std::set<std::string> islandLayers;
    for (size_t l = 0; l < m.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = m.cornerAttrs().layer(l); a.type() == AttrType::Float2)
            islandLayers.insert(islandLayer(m, a).name());
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    for (const std::string& name : islandLayers)
    {
        const Islands is = islandsOf(m, *m.cornerAttrs().find(name));
        const std::span<const glm::vec2> keyValues = m.cornerAttrs().find(name)->values<glm::vec2>();
        const std::vector<glm::vec2> key(keyValues.begin(), keyValues.end());
        const std::vector<double> density = islandDensities(m, is, key);
        std::span<glm::vec2> proxy = m.cornerAttrs().getOrAdd<glm::vec2>(std::string(kSpacingProxy) + name);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (Index g = 0; g < is.groupCorners.size(); ++g)
            for (Index c : is.groupCorners[g])
                proxy[c] = is.border[g] ? glm::vec2(nan) : glm::vec2(0.0f);
        std::unordered_map<Index, Index> next;
        std::unordered_set<Index> pinched;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.forEachFaceCorner(f, [&](Index c) {
                    const Index e = m.cornerEdge(c), cn = m.cornerNext(c);
                    if (m.isManifoldEdge(e))
                    {
                        const Index ec = m.edgeCorner(e), rc = m.cornerFace(ec) == f ? m.cornerRadial(ec) : ec;
                        if (is.groupOfCorner[cornerAt(rc, m.cornerPoint(c))] == is.groupOfCorner[c] &&
                            is.groupOfCorner[cornerAt(rc, m.cornerPoint(cn))] == is.groupOfCorner[cn])
                            return;
                    }
                    if (!next.emplace(is.groupOfCorner[c], is.groupOfCorner[cn]).second)
                        pinched.insert(is.groupOfCorner[c]);
                });
        auto pos = [&](Index g) { return m.position(m.cornerPoint(is.groupCorners[g].front())); };
        std::unordered_set<Index> seen;
        std::vector<Index> loop;
        std::vector<double> at, ll, lu;
        for (const auto& [first, unused] : next)
        {
            if (seen.count(first))
                continue;
            loop.clear();
            bool closed = false, clean = true;
            for (Index g = first;;)
            {
                seen.insert(g);
                loop.push_back(g);
                clean = clean && !pinched.count(g);
                const auto it = next.find(g);
                if (it == next.end())
                    break;
                g = it->second;
                closed = g == first;
                if (closed || seen.count(g))
                    break;
            }
            if (!closed || !clean || loop.size() < 3)
                continue;
            double total = 0.0;
            const size_t nl = loop.size();
            at.resize(nl);
            ll.resize(nl);
            lu.resize(nl);
            for (size_t i = 0; i < nl; ++i)
            {
                const Index a = loop[i], b = loop[(i + 1) % nl];
                ll[i] = glm::length(pos(b) - pos(a));
                lu[i] = glm::length(glm::dvec2(key[is.groupCorners[b].front()]) - glm::dvec2(key[is.groupCorners[a].front()]));
            }
            for (size_t i = 0; i < nl; ++i)
            {
                at[i] = total;
                // A thin edge (a bevel's corner points share their corner's uv) is bunched before subdividing too.
                total += std::min(ll[i], lu[i] / (kCageThin * density[is.islandOfGroup[loop[i]]]));
            }
            if (!(total > 0.0))
                continue;
            const double r = total / (2.0 * std::numbers::pi);
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const double a = 2.0 * std::numbers::pi * at[i] / total;
                for (Index c : is.groupCorners[loop[i]])
                    proxy[c] = glm::vec2(float(r * std::cos(a)), float(r * std::sin(a)));
            }
        }
    }
}

/** Replaces each uv island's interior with the harmonic fill of its border, in every layer using those islands. */
void fitSurface(Mesh& m)
{
    LILA_ZONE();

    std::map<std::string, std::vector<glm::vec2>> proxies;
    for (size_t l = 0; l < m.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = m.cornerAttrs().layer(l); a.name().starts_with(kSpacingProxy))
        {
            const std::span<const glm::vec2> v = a.values<glm::vec2>();
            proxies[a.name().substr(kSpacingProxy.size())].assign(v.begin(), v.end());
        }
    for (const auto& [name, unused] : proxies)
        m.cornerAttrs().remove(std::string(kSpacingProxy) + name);
    std::map<std::string, std::vector<std::string>> byIslands;
    for (size_t l = 0; l < m.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = m.cornerAttrs().layer(l); a.type() == AttrType::Float2)
            byIslands[islandLayer(m, a).name()].push_back(a.name());
    for (const auto& [islandName, layers] : byIslands)
    {
        const Islands is = islandsOf(m, *m.cornerAttrs().find(islandName));
        std::vector<std::span<glm::vec2>> spans;
        for (const std::string& l : layers)
            spans.push_back(m.cornerAttrs().get<glm::vec2>(l));
        const std::span<const glm::vec2> set = m.cornerAttrs().find(islandName)->values<glm::vec2>();
        const auto proxyIt = proxies.find(islandName);
        if (proxyIt == proxies.end())
            continue;
        const std::vector<glm::vec2>& proxy = proxyIt->second;
        FitRecord* record = nullptr;
        if (tRecorder)
        {
            record = &tRecorder->records.emplace_back();
            record->islandLayer = islandName;
            record->cornerCapacity = m.cornerCapacity();
            record->islands.resize(is.faces.size());
        }
        // Islands share no corners, so each writes only its own groups while the others read theirs.
        tbb::parallel_for(Index(0), Index(is.faces.size()), [&](Index island) {
            const std::vector<BorderBlend> blends = respaceBorder(m, is, island, proxy, set);
            std::vector<glm::vec2> moved(blends.size());
            // Recording leaves the uvs alone, so corners are found on a respaced copy of the island layer.
            std::vector<glm::vec2> respacedSet;
            std::span<const glm::vec2> cornerSet = set;
            if (record && !blends.empty())
            {
                respacedSet.assign(set.begin(), set.end());
                for (const BorderBlend& b : blends)
                {
                    const glm::vec2 v(glm::mix(groupValue(set, is.groupCorners[b.a]), groupValue(set, is.groupCorners[b.b]), b.t));
                    for (Index c : is.groupCorners[b.group])
                        respacedSet[c] = v;
                }
                cornerSet = respacedSet;
            }
            if (!record)
                for (std::span<glm::vec2> uv : spans)
                {
                    for (size_t k = 0; k < blends.size(); ++k)
                        moved[k] = glm::vec2(glm::mix(groupValue(uv, is.groupCorners[blends[k].a]), groupValue(uv, is.groupCorners[blends[k].b]), blends[k].t));
                    for (size_t k = 0; k < blends.size(); ++k)
                        for (Index c : is.groupCorners[blends[k].group])
                            uv[c] = moved[k];
                }
            const std::vector<CornerApex> apexes = openCorners(m, is, island, cornerSet);
            const System s = systemOf(m, is, island);
            std::shared_ptr<const Factor> f;
            if (!s.interior.empty() && !s.border.empty())
                if (f = factorCache().get(s.a); !f->ok)
                    f.reset();
            if (record)
            {
                record->islands[island] = recordIsland(is, blends, apexes, s, f);
                return;
            }
            moved.resize(apexes.size());
            for (std::span<glm::vec2> uv : spans)
            {
                for (size_t k = 0; k < apexes.size(); ++k)
                {
                    const CornerApex& x = apexes[k];
                    moved[k] = glm::vec2(groupValue(uv, is.groupCorners[x.a]) * x.wa + groupValue(uv, is.groupCorners[x.b]) * x.wb +
                                         groupValue(uv, is.groupCorners[x.d]) * x.wd);
                }
                for (size_t k = 0; k < apexes.size(); ++k)
                    for (Index c : is.groupCorners[apexes[k].group])
                        uv[c] = moved[k];
            }
            if (!f)
                return;
            const auto nb = Eigen::Index(s.border.size()), cols = Eigen::Index(spans.size()) * 2;
            Eigen::MatrixXd xb(nb, cols);
            for (size_t l = 0; l < spans.size(); ++l)
                for (Eigen::Index k = 0; k < nb; ++k)
                {
                    const glm::dvec2 v = groupValue(spans[l], is.groupCorners[s.border[size_t(k)]]);
                    xb(k, Eigen::Index(l) * 2) = v.x;
                    xb(k, Eigen::Index(l) * 2 + 1) = v.y;
                }
            const Eigen::MatrixXd rhs = s.b * xb;
            const Eigen::MatrixXd xi = f->ldlt.solve(rhs);
            if (!xi.allFinite())
                return;
            for (size_t l = 0; l < spans.size(); ++l)
                for (size_t k = 0; k < s.interior.size(); ++k)
                {
                    const auto r = Eigen::Index(k), col = Eigen::Index(l) * 2;
                    const glm::vec2 v(float(xi(r, col)), float(xi(r, col + 1)));
                    for (Index c : is.groupCorners[s.interior[k]])
                        spans[l][c] = v;
                }
        });
    }
}
} // namespace fit
} // namespace

FitFill::FitFill(std::shared_ptr<const Impl> impl) : mImpl(std::move(impl)) {}
FitFill::~FitFill() = default;

size_t FitFill::borderCount() const
{
    return size_t(mImpl->b.cols());
}

size_t FitFill::interiorCount() const
{
    return size_t(mImpl->b.rows());
}

void FitFill::solve(std::span<const double> border, std::span<double> interior, size_t columns) const
{
    const auto nb = mImpl->b.cols(), ni = mImpl->b.rows(), k = Eigen::Index(columns);
    const Eigen::Map<const Eigen::MatrixXd> x(border.data(), nb, k);
    Eigen::Map<Eigen::MatrixXd> y(interior.data(), ni, k);
    y = mImpl->factor->ldlt.solve(mImpl->b * x);
}

void FitFill::solveTransposed(std::span<const double> g, std::span<double> border, size_t columns) const
{
    const auto nb = mImpl->b.cols(), ni = mImpl->b.rows(), k = Eigen::Index(columns);
    const Eigen::Map<const Eigen::MatrixXd> x(g.data(), ni, k);
    Eigen::Map<Eigen::MatrixXd> y(border.data(), nb, k);
    // The system is symmetric, so the transpose of A^-1 B is B^T A^-1.
    y = mImpl->b.transpose() * mImpl->factor->ldlt.solve(x);
}

FitRecorder::FitRecorder() : mOuter(fit::tRecorder)
{
    fit::tRecorder = this;
}

FitRecorder::~FitRecorder()
{
    fit::tRecorder = mOuter;
}

Mesh subdivide(const Mesh& mesh, const SubdivideOptions& opts, SubdivideMap* map)
{
    LILA_ZONE();

    const bool fitting = opts.uvs == SubdivideUvs::FitSurface && opts.levels > 0;
    Mesh cur = mesh;
    if (fitting)
        fit::addSpacingProxies(cur);
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
    if (fitting)
        fit::fitSurface(cur);
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
    tbb::parallel_for(tbb::blocked_range<Index>(0, rows(), kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
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
