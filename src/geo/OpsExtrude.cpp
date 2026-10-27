#include "geo/Ops.h"
#include "geo/OpsCommon.h"

#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>
#include <unordered_map>

namespace lila::geo
{
namespace
{
using namespace detail;

struct RegionMove
{
    double offset = 0.0;    // along normals, every region point
    double thickness = 0.0; // inward, boundary points only
    bool clamp = false;     // shrink the thickness so no face's border crosses itself
};

/** Clamped thickness leaves this share of the room where points of a face would cross inside it. */
constexpr double kClampMargin = 0.02;
/** Border points a clamped inset brings closer than this share of the thickness are welded. */
constexpr double kWeldShare = 1e-7;

struct Clamp
{
    double share = 1.0;     // of the thickness
    bool collapses = false; // some border edge shrinks to nothing at that share
};

/**
 * The share of the thickness, at most 1, at which the border points moving by dirs (per unit thickness) leave every
 * face simple. An edge whose ends would pass each other stops where they meet, as the sides of a face narrower than
 * twice the thickness meeting on its medial line; points that would cross inside a face stop short of it.
 */
Clamp clampThickness(const Mesh& mesh, std::span<const Index> faces, const std::unordered_map<Index, glm::dvec3>& dirs,
                     double thickness)
{
    LILA_ZONE();

    auto dirOf = [&](Index p) {
        auto it = dirs.find(p);
        return it == dirs.end() ? glm::dvec3(0.0) : it->second * thickness;
    };
    Clamp out;
    for (Index f : faces)
        mesh.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 a = mesh.position(mesh.cornerPoint(c)), b = mesh.position(mesh.cornerNextPoint(c));
            const double len = glm::distance(a, b);
            if (len <= 0.0)
                return;
            const double k = glm::dot(dirOf(mesh.cornerPoint(c)) - dirOf(mesh.cornerNextPoint(c)), (b - a) / len);
            if (k > 1e-12 && len / k < out.share)
            {
                out.share = len / k;
                out.collapses = true;
            }
        });
    const double tol = kWeldShare * std::abs(thickness);
    // Collapsed edges count once, and a face that collapses to a line or a point is simple.
    auto simpleAt = [&](Index f, double at) {
        const glm::dvec3 n = mesh.faceNormal(f);
        const glm::dvec3 ax = glm::normalize(glm::cross(n, std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0)));
        const glm::dvec3 ay = glm::cross(n, ax);
        std::vector<glm::dvec2> poly;
        mesh.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 q = mesh.position(mesh.cornerPoint(c)) + dirOf(mesh.cornerPoint(c)) * at;
            const glm::dvec2 uv{glm::dot(q, ax), glm::dot(q, ay)};
            if (poly.empty() || glm::distance(poly.back(), uv) > tol)
                poly.push_back(uv);
        });
        while (poly.size() > 1 && glm::distance(poly.back(), poly.front()) <= tol)
            poly.pop_back();
        return poly.size() < 3 || polygonIsSimple(poly);
    };
    std::vector<Index> crossing;
    for (Index f : faces)
        if (glm::length(mesh.faceNormal(f)) > 0.5 && !simpleAt(f, out.share) && simpleAt(f, out.share * 1e-6))
            crossing.push_back(f);
    if (crossing.empty())
        return out;
    double lo = 0.0, hi = out.share;
    for (int it = 0; it < 40; ++it)
    {
        const double mid = 0.5 * (lo + hi);
        const bool ok = std::all_of(crossing.begin(), crossing.end(), [&](Index f) { return simpleAt(f, mid); });
        (ok ? lo : hi) = mid;
    }
    return {lo * (1.0 - kClampMargin), false};
}

/**
 * Welds the border points a clamped inset brought together along the edges of the region's faces. Faces left with
 * under three points go, and result follows the faces that were rebuilt.
 */
void weldCollapsed(Mesh& mesh, ExtrudeResult& result, std::unordered_map<Index, glm::dvec3>& normalSum, double tol)
{
    LILA_ZONE();

    std::vector<Index> target(mesh.pointCapacity(), kInvalid);
    auto find = [&](Index p) {
        while (target[p] != kInvalid)
            p = target[p];
        return p;
    };
    bool any = false;
    for (Index f : result.faces)
        mesh.forEachFaceCorner(f, [&](Index c) {
            const Index a = find(mesh.cornerPoint(c)), b = find(mesh.cornerNextPoint(c));
            if (a != b && glm::distance(mesh.position(a), mesh.position(b)) <= tol)
            {
                target[std::max(a, b)] = std::min(a, b);
                any = true;
            }
        });
    if (!any)
        return;
    for (Index p = 0; p < Index(target.size()); ++p)
        if (target[p] != kInvalid)
        {
            target[p] = find(p);
            if (auto it = normalSum.find(p); it != normalSum.end())
            {
                normalSum[target[p]] += it->second;
                normalSum.erase(p);
            }
        }
    std::unordered_map<Index, Index> rebuilt;
    weldPoints(mesh, target, &rebuilt);
    auto remap = [&](std::vector<Index>& ids) {
        for (Index& f : ids)
            if (auto it = rebuilt.find(f); it != rebuilt.end())
                f = it->second;
        std::erase(ids, kInvalid);
    };
    remap(result.faces);
    remap(result.sides);
}

/** A cap corner on the region border, with its face's corner triangle before the thickness moved it. */
struct CapCorner
{
    Index c, prev, next;
    glm::dvec3 o, a, b;
};

/**
 * Uvs of the moved border corners extrapolated from their corner triangles, so the cap shrinks in uv space as it
 * does in the face and the sides between get the uv band it gave up. The sides' top corners follow the cap.
 */
void insetCapUvs(Mesh& mesh, std::span<const CapCorner> caps, std::span<const Index> sides, Index firstDup)
{
    AttributeSet& ca = mesh.cornerAttrs();
    for (size_t l = 0; l < ca.layerCount(); ++l)
    {
        if (ca.layer(l).type() != AttrType::Float2)
            continue;
        std::span<glm::vec2> uv = ca.layer(l).values<glm::vec2>();
        std::vector<glm::vec2> moved;
        for (const CapCorner& cc : caps)
        {
            const glm::dvec3 d = mesh.position(mesh.cornerPoint(cc.c)) - cc.o;
            const double aa = glm::dot(cc.a, cc.a), ab = glm::dot(cc.a, cc.b), bb = glm::dot(cc.b, cc.b);
            const double det = aa * bb - ab * ab;
            if (std::abs(det) <= 1e-12 * aa * bb)
            {
                moved.push_back(uv[cc.c]);
                continue;
            }
            const double al = (bb * glm::dot(cc.a, d) - ab * glm::dot(cc.b, d)) / det;
            const double be = (aa * glm::dot(cc.b, d) - ab * glm::dot(cc.a, d)) / det;
            moved.push_back(uv[cc.c] + float(al) * (uv[cc.prev] - uv[cc.c]) + float(be) * (uv[cc.next] - uv[cc.c]));
        }
        for (size_t i = 0; i < caps.size(); ++i)
            uv[caps[i].c] = moved[i];
        for (Index sf : sides)
            mesh.forEachFaceCorner(sf, [&](Index sc) {
                const Index r = mesh.cornerRadial(sc);
                if (r == sc || mesh.cornerPoint(sc) < firstDup || mesh.cornerNextPoint(sc) < firstDup)
                    return;
                uv[sc] = uv[mesh.cornerNext(r)];
                uv[mesh.cornerNext(sc)] = uv[r];
            });
    }
}

ExtrudeResult extrudeRegion(Mesh& mesh, const std::vector<Index>& faces, const RegionMove& move)
{
    LILA_ZONE();

    ExtrudeResult result;
    // faces is sorted; a lookup keeps per-face runs from allocating a mask the size of the mesh.
    auto in = [&](Index f) { return std::binary_search(faces.begin(), faces.end(), f) ? 1 : 0; };
    std::unordered_map<Index, glm::dvec3> faceN;
    for (Index f : faces)
        faceN[f] = mesh.faceNormal(f);

    // Boundary corners: corners of selected faces whose edge has no other selected face.
    struct BoundaryCorner
    {
        Index corner;
        Index a, b;
        Index edge;
    };
    std::vector<BoundaryCorner> boundary;
    std::vector<Index> oldEdge;
    std::vector<Index> selCorners;
    for (Index f : faces)
        mesh.forEachFaceCorner(f, [&](Index c) {
            selCorners.push_back(c);
            oldEdge.push_back(mesh.cornerEdge(c));
            int selected = 0;
            mesh.forEachEdgeCorner(mesh.cornerEdge(c), [&](Index r) { selected += in(mesh.cornerFace(r)); });
            if (selected == 1)
                boundary.push_back({c, mesh.cornerPoint(c), mesh.cornerNextPoint(c), mesh.cornerEdge(c)});
        });

    std::unordered_map<Index, Index> dup;
    const Index firstDup = mesh.pointCapacity();
    for (const BoundaryCorner& bc : boundary)
        for (Index p : {bc.a, bc.b})
            if (!dup.count(p))
            {
                const Index np = mesh.addPoint(mesh.position(p));
                mesh.pointAttrs().copy(np, p);
                dup[p] = np;
            }
    auto mapped = [&](Index p) {
        auto it = dup.find(p);
        return it == dup.end() ? p : it->second;
    };

    const Index edgeCap = mesh.edgeCapacity();
    std::vector<Index> pts;
    for (Index f : faces)
    {
        pts.clear();
        mesh.forEachFaceCorner(f, [&](Index c) { pts.push_back(mapped(mesh.cornerPoint(c))); });
        mesh.setFacePoints(f, pts);
    }
    for (size_t i = 0; i < selCorners.size(); ++i)
    {
        const Index e = mesh.cornerEdge(selCorners[i]);
        if (e >= edgeCap)
            mesh.edgeAttrs().copy(e, oldEdge[i]);
    }

    // Side edges going up from the base inherit hardness from the region border at their foot.
    std::span<const bool> hardSpan = std::as_const(mesh).edgeAttrs().get<bool>(attr::kHard);
    std::unordered_map<Index, bool> footHard;
    if (!hardSpan.empty())
        for (const BoundaryCorner& bc : boundary)
            if (hardSpan[bc.edge])
                footHard[bc.a] = footHard[bc.b] = true;

    for (const BoundaryCorner& bc : boundary)
    {
        const Index c = bc.corner;
        const Index n = mesh.cornerNext(c);
        const Index quad[4] = {bc.a, bc.b, dup[bc.b], dup[bc.a]};
        const Index src[4] = {c, n, n, c};
        const Index sf = mesh.addFace(quad, mesh.cornerFace(c), src);
        if (sf != kInvalid)
            result.sides.push_back(sf);
    }
    // The sides share the cap's uvs, so a seam on the region border stays on the base edge only.
    if (std::span<bool> seam = mesh.edgeAttrs().get<bool>(attr::kSeam); !seam.empty())
        for (const BoundaryCorner& bc : boundary)
            if (const Index e = mesh.findEdge(dup[bc.a], dup[bc.b]); e != kInvalid && e >= edgeCap)
                seam[e] = false;
    if (!footHard.empty())
    {
        std::span<bool> hard = mesh.edgeAttrs().get<bool>(attr::kHard);
        for (const auto& [p, h] : footHard)
        {
            const Index e = mesh.findEdge(p, dup[p]);
            if (e != kInvalid)
                hard[e] = h;
        }
    }

    // Region points in their final ids, with the normals of the selected faces around them.
    std::unordered_map<Index, glm::dvec3> normalSum;
    for (Index f : faces)
        mesh.forEachFaceCorner(f, [&](Index c) { normalSum[mesh.cornerPoint(c)] += faceN[f]; });

    std::vector<CapCorner> capCorners;
    if (move.thickness != 0.0)
        for (Index f : faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                if (mesh.cornerPoint(c) < firstDup)
                    return;
                const Index pc = mesh.cornerPrev(c), nc = mesh.cornerNext(c);
                const glm::dvec3 o = mesh.position(mesh.cornerPoint(c));
                capCorners.push_back({c, pc, nc, o, mesh.position(mesh.cornerPoint(pc)) - o, mesh.position(mesh.cornerPoint(nc)) - o});
            });

    Clamp clamp;
    if (move.thickness != 0.0)
    {
        std::unordered_map<Index, std::pair<glm::dvec3, int>> perp;
        std::unordered_map<Index, glm::dvec3> dirs;
        for (const BoundaryCorner& bc : boundary)
        {
            const glm::dvec3 d = mesh.position(bc.b) - mesh.position(bc.a);
            const glm::dvec3 inward = glm::normalize(glm::cross(faceN[mesh.cornerFace(bc.corner)], d));
            for (Index p : {dup[bc.a], dup[bc.b]})
            {
                auto& s = perp[p];
                s.first += inward;
                ++s.second;
            }
        }
        for (const auto& [p, s] : perp)
        {
            const glm::dvec3 sum = s.first;
            const double len = glm::length(sum);
            if (len < 1e-12)
                continue;
            const glm::dvec3 m = sum / len;
            // Miter: for two edges the offset along m is thickness over the cosine to each edge normal.
            const double cosHalf = s.second == 2 ? std::max(len * 0.5, 0.1) : 1.0;
            dirs[p] = m / cosHalf;
        }
        clamp = move.clamp ? clampThickness(mesh, faces, dirs, move.thickness) : Clamp{};
        for (const auto& [p, d] : dirs)
            mesh.setPosition(p, mesh.position(p) + d * (move.thickness * clamp.share));
    }
    if (!capCorners.empty())
        insetCapUvs(mesh, capCorners, result.sides, firstDup);
    result.faces = faces;
    if (clamp.collapses)
        weldCollapsed(mesh, result, normalSum, kWeldShare * std::abs(move.thickness));
    if (move.offset != 0.0)
        for (const auto& [p, sum] : normalSum)
        {
            const double len = glm::length(sum);
            if (len > 1e-12)
                mesh.setPosition(p, mesh.position(p) + sum / len * move.offset);
        }

    mesh.removeLoose(oldEdge, {});
    return result;
}

ExtrudeResult runRegions(Mesh& mesh, std::span<const Index> faces, bool individual, const SymmetryMap* symmetry,
                         const RegionMove& move)
{
    std::vector<Index> sel = symmetry ? symmetry->expandFaces(mesh, faces) : uniqueSorted(faces);
    sel.erase(std::remove_if(sel.begin(), sel.end(), [&](Index f) { return !mesh.faceValid(f); }), sel.end());
    const Index firstNew = mesh.pointCapacity();
    ExtrudeResult out;
    if (individual)
    {
        for (Index f : sel)
        {
            ExtrudeResult r = extrudeRegion(mesh, {f}, move);
            out.faces.insert(out.faces.end(), r.faces.begin(), r.faces.end());
            out.sides.insert(out.sides.end(), r.sides.begin(), r.sides.end());
        }
    }
    else if (!sel.empty())
        out = extrudeRegion(mesh, sel, move);
    applySymmetrySnap(mesh, symmetry, firstNew);
    return out;
}
} // namespace

ExtrudeResult extrudeFaces(Mesh& mesh, std::span<const Index> faces, const ExtrudeOptions& opts)
{
    LILA_ZONE();

    return runRegions(mesh, faces, opts.individual, opts.symmetry, {opts.offset, opts.inset});
}

EdgeExtrudeResult extrudeEdges(Mesh& mesh, std::span<const Index> edges, const EdgeExtrudeOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> sel = opts.symmetry ? opts.symmetry->expandEdges(mesh, edges) : uniqueSorted(edges);
    std::erase_if(sel, [&](Index e) { return !mesh.edgeValid(e) || mesh.edgeCorner(e) == kInvalid; });
    EdgeExtrudeResult out;
    if (sel.empty())
        return out;

    // Per point: the summed normals of the faces its selected edges border, and their summed outward directions.
    struct Push
    {
        glm::dvec3 normal{0.0}, out{0.0};
        int count = 0;
    };
    std::unordered_map<Index, Push> push;
    std::unordered_map<Index, Index> dup;
    const Index firstNew = mesh.pointCapacity();
    struct Strip
    {
        Index corner, a, b;
    };
    std::vector<Strip> strips;
    for (Index e : sel)
    {
        const Index c = mesh.edgeCorner(e);
        const Index a = mesh.cornerPoint(c), b = mesh.cornerNextPoint(c);
        const glm::dvec3 n = mesh.faceNormal(mesh.cornerFace(c));
        const glm::dvec3 d = mesh.position(b) - mesh.position(a);
        const double len = glm::length(glm::cross(d, n));
        const glm::dvec3 outward = len > 1e-12 ? glm::cross(d, n) / len : glm::dvec3(0.0);
        for (Index p : {a, b})
        {
            Push& s = push[p];
            s.normal += n;
            s.out += outward;
            ++s.count;
            if (!dup.count(p))
            {
                const Index np = mesh.addPoint(mesh.position(p));
                mesh.pointAttrs().copy(np, p);
                dup[p] = np;
            }
        }
        strips.push_back({c, a, b});
    }

    for (const Strip& s : strips)
    {
        // Wound against the face the edge borders, so the strip continues its surface.
        const Index n = mesh.cornerNext(s.corner);
        const Index quad[4] = {s.b, s.a, dup[s.a], dup[s.b]};
        const Index src[4] = {n, s.corner, s.corner, n};
        if (const Index f = mesh.addFace(quad, mesh.cornerFace(s.corner), src); f != kInvalid)
            out.faces.push_back(f);
        if (const Index e = mesh.findEdge(dup[s.a], dup[s.b]); e != kInvalid)
            out.edges.push_back(e);
    }

    for (const auto& [p, s] : push)
    {
        glm::dvec3 move(0.0);
        if (const double nl = glm::length(s.normal); nl > 1e-12)
            move += s.normal / nl * opts.thickness;
        if (const double ol = glm::length(s.out); ol > 1e-12)
        {
            // Miter: two edges meeting at an angle push further so the strip keeps its width.
            const double cosHalf = s.count == 2 ? std::max(ol * 0.5, 0.1) : 1.0;
            move += s.out / ol * (opts.offset / cosHalf);
        }
        mesh.setPosition(dup[p], mesh.position(p) + move);
    }
    applySymmetrySnap(mesh, opts.symmetry, firstNew);
    return out;
}

ExtrudeResult insetFaces(Mesh& mesh, std::span<const Index> faces, const InsetOptions& opts)
{
    LILA_ZONE();

    return runRegions(mesh, faces, opts.individual, opts.symmetry, {opts.depth, opts.thickness, opts.clamp});
}
} // namespace lila::geo
