#include "curve/Carve.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Ops.h"
#include "scene/Materials.h"
#include "scene/MeshBoolean.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;
using geo::Index;

constexpr const char* kSlabSlot = "lila:carveSlab";

/** Points by position in buckets of a cell size, for snapping round-tripped points back onto their originals. */
class CarvePointGrid
{
public:
    explicit CarvePointGrid(double cell) : mCell(std::max(cell, 1e-9)) {}

    void add(const D3& p, Index id) { mCells[key(cellOf(p))].push_back({p, id}); }

    /** The nearest stored point within tol, or kInvalid. */
    Index nearest(const D3& p, double tol) const
    {
        const glm::i64vec3 c = cellOf(p);
        Index best = geo::kInvalid;
        double bestD = tol;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                {
                    auto it = mCells.find(key(c + glm::i64vec3(dx, dy, dz)));
                    if (it == mCells.end())
                        continue;
                    for (const auto& [q, id] : it->second)
                        if (double d = glm::distance(p, q); d <= bestD)
                        {
                            bestD = d;
                            best = id;
                        }
                }
        return best;
    }

private:
    glm::i64vec3 cellOf(const D3& p) const { return glm::i64vec3(glm::floor(p / mCell)); }
    static uint64_t key(const glm::i64vec3& c)
    {
        return (uint64_t(c.x) * 73856093ull) ^ (uint64_t(c.y) * 19349663ull) ^ (uint64_t(c.z) * 83492791ull);
    }
    double mCell;
    std::unordered_map<uint64_t, std::vector<std::pair<D3, Index>>> mCells;
};

geo::Bounds carveBounds(const geo::Mesh& m)
{
    geo::Bounds b;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            b.extend(m.position(p));
    return b;
}

bool carveOverlaps(const geo::Bounds& a, const geo::Bounds& b)
{
    return a.min.x <= b.max.x && b.min.x <= a.max.x && a.min.y <= b.max.y && b.min.y <= a.max.y && a.min.z <= b.max.z && b.min.z <= a.max.z;
}

/**
 * Closes an open patch into a solid: walls down from its border and a floor flat across below depth (along -up), on the
 * slab slot so they can be found and dropped after the boolean.
 */
void closeIntoSlab(geo::Mesh& slab, const D3& up, double depth, int32_t slabSlot)
{
    std::vector<Index> top;
    for (Index p = 0; p < slab.pointCapacity(); ++p)
        if (slab.pointValid(p))
            top.push_back(p);
    std::vector<Index> below(slab.pointCapacity(), geo::kInvalid);
    for (Index p : top)
    {
        const D3 q = slab.position(p);
        below[p] = slab.addPoint(q - up * (glm::dot(q, up) + depth));
    }
    std::vector<Index> faces;
    for (Index f = 0; f < slab.faceCapacity(); ++f)
        if (slab.faceValid(f))
            faces.push_back(f);
    // Border edges as the patch's faces run along them; the wall runs the other way.
    std::vector<std::pair<Index, Index>> border;
    for (Index e = 0; e < slab.edgeCapacity(); ++e)
        if (slab.edgeValid(e) && slab.isBoundaryEdge(e))
            if (const Index c = slab.edgeCorner(e); c != geo::kInvalid && slab.cornerValid(c))
                border.push_back({slab.cornerPoint(c), slab.cornerNextPoint(c)});
    std::vector<Index> made;
    for (Index f : faces)
    {
        std::vector<Index> pts = slab.facePoints(f);
        std::vector<Index> floor;
        for (auto it = pts.rbegin(); it != pts.rend(); ++it)
            floor.push_back(below[*it]);
        made.push_back(slab.addFace(floor));
    }
    for (auto [a, b] : border)
        made.push_back(slab.addFace({b, a, below[a], below[b]}));
    auto mat = slab.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    for (Index f : made)
        if (f != geo::kInvalid && f < mat.size())
            mat[f] = slabSlot;
}
} // namespace

CarveResult carve(const geo::Mesh& surface, const glm::dmat4& toWorld, const geo::Mesh& cutterWorld, const CarveOptions& o)
{
    LILA_ZONE();

    CarveResult r;
    r.mesh = surface;
    if (cutterWorld.numFaces() == 0 || surface.numFaces() == 0)
        return r;
    const glm::dmat4 toLocal = glm::inverse(toWorld);
    geo::Mesh cutter = cutterWorld;
    for (D3& p : cutter.positions())
        p = D3(toLocal * glm::dvec4(p, 1.0));
    if (!o.material.empty())
        scene::materials::assign(cutter, o.material, {});
    D3 up = glm::dmat3(toLocal) * (glm::length(o.up) > 1e-12 ? o.up : D3(0, 1, 0));
    up = glm::length(up) > 1e-12 ? glm::normalize(up) : D3(0, 1, 0);

    geo::Bounds cb = carveBounds(cutter);
    const double diag = glm::length(cb.size());
    const D3 pad(diag * 1e-6 + 1e-6);
    cb.min -= pad;
    cb.max += pad;

    // The patch: faces near the cutter that face its way.
    const double facing = std::cos(glm::radians(std::clamp(o.facingAngle, 0.0, 180.0)));
    std::vector<Index> patchFaces, otherFaces;
    for (Index f = 0; f < surface.faceCapacity(); ++f)
    {
        if (!surface.faceValid(f))
            continue;
        geo::Bounds fb;
        surface.forEachFaceCorner(f, [&](Index c) { fb.extend(surface.position(surface.cornerPoint(c))); });
        (carveOverlaps(fb, cb) && glm::dot(surface.faceNormal(f), up) >= facing ? patchFaces : otherFaces).push_back(f);
    }
    r.patchFaces = patchFaces.size();
    if (patchFaces.empty())
        return r;

    geo::Mesh slab = surface;
    geo::deleteFaces(slab, otherFaces);
    if (slab.materialNames().empty())
        slab.materialNames().push_back(scene::materials::kDefaultSlot);
    slab.faceAttrs().getOrAdd<int32_t>(geo::attr::kMaterial);
    const int32_t slabSlot = int32_t(slab.materialNames().size());
    slab.materialNames().push_back(kSlabSlot);

    // The floor goes below the patch and the cutter both, so the cut can't reach through it.
    double depth = -std::numeric_limits<double>::max();
    geo::Bounds pb;
    for (Index p = 0; p < slab.pointCapacity(); ++p)
        if (slab.pointValid(p))
        {
            depth = std::max(depth, -glm::dot(slab.position(p), up));
            pb.extend(slab.position(p));
        }
    for (const D3& p : cutter.positions())
        depth = std::max(depth, -glm::dot(p, up));
    depth += diag * 0.05 + 1.0;
    std::vector<std::pair<D3, Index>> originals;
    for (Index p = 0; p < slab.pointCapacity(); ++p)
        if (slab.pointValid(p))
            originals.push_back({slab.position(p), p});
    closeIntoSlab(slab, up, depth, slabSlot);

    // Re-centred on the cutter, so its neighbourhood keeps full precision however far it is from the origin.
    const D3 centre = cb.center();
    const glm::dmat4 toCentre = glm::translate(glm::dmat4(1.0), -centre);
    scene::BooleanOperand ops[] = {{&slab, toCentre}, {&cutter, toCentre}};
    scene::BooleanOptions bo;
    bo.op = scene::BooleanOp::Difference;
    bo.hardAngle = o.hardAngle;
    scene::BooleanResult br = scene::meshBoolean(ops, glm::inverse(toCentre), bo);
    if (!br.ok())
    {
        r.error = (br.badOperand == 1 ? "the cutter " : br.badOperand == 0 ? "the surface near the cut " : "") + br.error;
        return r;
    }
    geo::Mesh& cut = br.mesh;
    {
        auto mat = cut.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        const auto& names = cut.materialNames();
        const int32_t slot = int32_t(std::find(names.begin(), names.end(), std::string(kSlabSlot)) - names.begin());
        std::vector<Index> drop;
        for (Index f = 0; f < cut.faceCapacity(); ++f)
            if (cut.faceValid(f) && f < mat.size() && mat[f] == slot)
                drop.push_back(f);
        geo::deleteFaces(cut, drop);
        cut.compact();
    }

    // Snap round-tripped points back onto the originals, then weld the patch's border into the rest of the surface.
    const double tol = std::max(glm::length(pb.size()) * 2e-6, 1e-5);
    CarvePointGrid grid(tol * 4.0);
    for (const auto& [p, id] : originals)
        grid.add(p, id);
    std::vector<Index> partner(cut.pointCapacity(), geo::kInvalid);
    for (Index p = 0; p < cut.pointCapacity(); ++p)
        if (cut.pointValid(p))
            if (Index q = grid.nearest(cut.position(p), tol); q != geo::kInvalid)
            {
                partner[p] = q;
                cut.setPosition(p, surface.position(q));
            }

    geo::Mesh out = surface;
    geo::deleteFaces(out, patchFaces);
    const Index off = out.append(cut);
    std::vector<geo::WeldPair> pairs;
    for (Index p = 0; p < cut.pointCapacity(); ++p)
        if (cut.pointValid(p) && cut.isBoundaryPoint(p))
        {
            // A partner gone from the rest was on the surface's own open border, where there is nothing to weld to.
            if (partner[p] == geo::kInvalid)
                ++r.unwelded;
            else if (out.pointValid(partner[p]))
                pairs.push_back({p + off, partner[p]});
        }
    geo::targetWeld(out, pairs);
    out.compact();
    scene::materials::pruneSlots(out);
    r.mesh = std::move(out);
    return r;
}
} // namespace lila::curve
