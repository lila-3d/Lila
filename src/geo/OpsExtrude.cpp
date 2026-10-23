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
};

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

    if (move.thickness != 0.0)
    {
        std::unordered_map<Index, std::pair<glm::dvec3, int>> perp;
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
            mesh.setPosition(p, mesh.position(p) + m * (move.thickness / cosHalf));
        }
    }
    if (!capCorners.empty())
        insetCapUvs(mesh, capCorners, result.sides, firstDup);
    if (move.offset != 0.0)
        for (const auto& [p, sum] : normalSum)
        {
            const double len = glm::length(sum);
            if (len > 1e-12)
                mesh.setPosition(p, mesh.position(p) + sum / len * move.offset);
        }

    mesh.removeLoose(oldEdge, {});
    result.faces = faces;
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
            out.faces.push_back(f);
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

ExtrudeResult insetFaces(Mesh& mesh, std::span<const Index> faces, const InsetOptions& opts)
{
    LILA_ZONE();

    return runRegions(mesh, faces, opts.individual, opts.symmetry, {opts.depth, opts.thickness});
}
} // namespace lila::geo
