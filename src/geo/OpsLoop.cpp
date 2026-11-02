#include "geo/Ops.h"
#include "geo/OpsCommon.h"

#include "geo/Knife.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <tuple>

namespace lila::geo
{
namespace
{
using namespace detail;

struct RingEdge
{
    Index edge;
    Index sideA; // the endpoint on the loop's "first" side
};

/** Ring through e with consistent sides, plus the quads between consecutive edges. */
bool walkRing(const Mesh& mesh, Index e, std::vector<RingEdge>& ring, std::vector<Index>& quads, bool& closed)
{
    ring.clear();
    quads.clear();
    closed = false;
    if (!mesh.edgeValid(e) || mesh.edgeCorner(e) == kInvalid)
        return false;
    std::vector<uint8_t> seen(mesh.edgeCapacity(), 0);

    // Steps across quad f from (edge, side) to the opposite edge and its matching side.
    auto step = [&](Index edge, Index side, Index f, RingEdge& next) {
        if (mesh.faceSize(f) != 4)
            return false;
        Index c = kInvalid;
        mesh.forEachFaceCorner(f, [&](Index x) {
            if (mesh.cornerEdge(x) == edge)
                c = x;
        });
        if (c == kInvalid)
            return false;
        const Index c2 = mesh.cornerNext(mesh.cornerNext(c));
        next.edge = mesh.cornerEdge(c2);
        // In x->y->z->w, x pairs with w and y with z.
        next.sideA = mesh.cornerPoint(c) == side ? mesh.cornerNextPoint(c2) : mesh.cornerPoint(c2);
        return true;
    };

    const Index start = mesh.edgePoint(e, 0);
    std::vector<RingEdge> fwd{{e, start}};
    std::vector<Index> fwdQuads;
    seen[e] = 1;
    Index f = mesh.cornerFace(mesh.edgeCorner(e));
    RingEdge cur{e, start};
    while (f != kInvalid)
    {
        RingEdge next;
        if (!step(cur.edge, cur.sideA, f, next))
            break;
        fwdQuads.push_back(f);
        if (next.edge == e)
        {
            closed = true;
            break;
        }
        if (seen[next.edge])
        {
            fwdQuads.pop_back();
            break;
        }
        seen[next.edge] = 1;
        fwd.push_back(next);
        f = mesh.oppositeFace(next.edge, f);
        cur = next;
    }
    if (closed)
    {
        ring = std::move(fwd);
        quads = std::move(fwdQuads);
        return true;
    }

    std::vector<RingEdge> back;
    std::vector<Index> backQuads;
    f = mesh.oppositeFace(e, mesh.cornerFace(mesh.edgeCorner(e)));
    cur = {e, start};
    while (f != kInvalid)
    {
        RingEdge next;
        if (!step(cur.edge, cur.sideA, f, next) || seen[next.edge])
            break;
        seen[next.edge] = 1;
        backQuads.push_back(f);
        back.push_back(next);
        f = mesh.oppositeFace(next.edge, f);
        cur = next;
    }
    std::reverse(back.begin(), back.end());
    std::reverse(backQuads.begin(), backQuads.end());
    ring = back;
    ring.insert(ring.end(), fwd.begin(), fwd.end());
    quads = backQuads;
    quads.insert(quads.end(), fwdQuads.begin(), fwdQuads.end());
    return true;
}

/** Splits e at the fractions (ascending, measured from sideA); returns the new points from sideA on. */
std::vector<Index> splitAt(Mesh& mesh, Index e, Index sideA, const std::vector<double>& fracs)
{
    std::vector<Index> out;
    const bool fromA = mesh.edgePoint(e, 0) == sideA;
    std::vector<double> f = fracs;
    if (!fromA)
    {
        for (double& x : f)
            x = 1.0 - x;
        std::reverse(f.begin(), f.end());
    }
    Index cur = e;
    double prev = 0.0;
    for (double x : f)
    {
        out.push_back(mesh.splitEdge(cur, (x - prev) / (1.0 - prev)));
        cur = mesh.lastSplitEdge();
        prev = x;
    }
    if (!fromA)
        std::reverse(out.begin(), out.end());
    return out;
}

Index sharedFace(const Mesh& mesh, Index a, Index b)
{
    Index found = kInvalid;
    mesh.forEachPointCorner(a, [&](Index c) {
        const Index f = mesh.cornerFace(c);
        if (found == kInvalid && mesh.findCorner(f, b) != kInvalid)
            found = f;
    });
    return found;
}

} // namespace

std::vector<double> edgeLoopFractions(const EdgeLoopOptions& opts)
{
    constexpr double lo = 1e-4, hi = 1.0 - 1e-4;
    const int count = std::max(1, opts.count);
    if (count == 1)
        return {std::clamp(opts.fraction, lo, hi)};
    // Neighbours stay at least this far apart, so no loop lands on another.
    constexpr double minGap = 1e-3;
    const double gap = std::max(minGap, std::max(0.0, opts.spacing) / (count + 1));
    const double span = std::min(gap * (count - 1), hi - lo);
    const double first = std::clamp(opts.fraction - span / 2.0, lo, hi - span);
    std::vector<double> fracs;
    for (int k = 0; k < count; ++k)
        fracs.push_back(first + span * k / (count - 1));
    return fracs;
}

std::vector<Index> insertEdgeLoop(Mesh& mesh, Index ringEdge, const EdgeLoopOptions& opts)
{
    LILA_ZONE();

    std::vector<RingEdge> ring;
    std::vector<Index> quads;
    bool closed = false;
    if (!walkRing(mesh, ringEdge, ring, quads, closed) || quads.empty())
        return {};

    const std::vector<double> fracs = edgeLoopFractions(opts);

    std::vector<std::vector<Index>> cuts;
    for (const RingEdge& r : ring)
        cuts.push_back(splitAt(mesh, r.edge, r.sideA, fracs));

    std::vector<Index> out;
    const size_t n = ring.size();
    for (size_t i = 0; i < quads.size(); ++i)
    {
        const std::vector<Index>& a = cuts[i];
        const std::vector<Index>& b = cuts[(i + 1) % n];
        for (size_t k = 0; k < a.size(); ++k)
        {
            const Index f = sharedFace(mesh, a[k], b[k]);
            if (f == kInvalid)
                continue;
            if (mesh.splitFace(f, mesh.findCorner(f, a[k]), mesh.findCorner(f, b[k])) != kInvalid)
                out.push_back(mesh.lastSplitEdge());
        }
    }
    return out;
}

std::vector<Index> splitRings(Mesh& mesh, std::span<const Index> ringEdges, int count, const SymmetryMap* symmetry)
{
    LILA_ZONE();

    const SymmetryMap* sym = symmetry && !symmetry->empty() ? symmetry : nullptr;
    std::vector<uint8_t> covered(mesh.edgeCapacity(), 0);
    auto cover = [&](Index e) {
        for (Index x : edgeRing(mesh, e))
            covered[x] = 1;
        covered[e] = 1;
    };
    std::vector<Index> reps;
    for (Index e : ringEdges)
    {
        if (!mesh.edgeValid(e) || covered[e])
            continue;
        reps.push_back(e);
        cover(e);
        if (sym)
            for (int k = 1; k < sym->order(); ++k)
                if (const Index me = sym->imageEdge(mesh, k, e); me != kInvalid && !covered[me])
                    cover(me);
    }

    // Splitting keeps every existing edge id, so the later rings' edges stay valid while earlier ones are cut.
    const EdgeLoopOptions opts{.fraction = 0.5, .count = std::max(1, count)};
    std::vector<Index> out;
    for (Index e : reps)
    {
        if (!mesh.edgeValid(e))
            continue;
        if (!sym)
        {
            std::ranges::copy(insertEdgeLoop(mesh, e, opts), std::back_inserter(out));
            continue;
        }
        KnifePlan plan;
        for (double t : edgeLoopFractions(opts))
            std::ranges::copy(knifeLoopPlan(mesh, e, t, sym).segments, std::back_inserter(plan.segments));
        std::ranges::copy(knifeApply(mesh, plan), std::back_inserter(out));
    }
    std::erase_if(out, [&](Index e) { return !mesh.edgeValid(e); });
    return out;
}

std::vector<Index> regionRingEdges(const Mesh& mesh, std::span<const Index> faces, bool across)
{
    LILA_ZONE();

    const std::vector<uint8_t> in = makeMask(mesh.faceCapacity(), faces);
    auto opposite = [&](Index f, Index e) {
        Index out = kInvalid;
        mesh.forEachFaceCorner(f, [&](Index c) {
            if (mesh.cornerEdge(c) == e)
                out = mesh.cornerEdge(mesh.cornerNext(mesh.cornerNext(c)));
        });
        return out;
    };
    // Edges of e's ring inside the region; a closed ring counts each of its edges twice, alike for all of them.
    auto inRegion = [&](Index e) {
        size_t n = 1;
        mesh.forEachEdgeCorner(e, [&](Index c) {
            Index edge = e, f = mesh.cornerFace(c);
            for (size_t guard = 0; guard < mesh.edgeCapacity() && f != kInvalid && in[f] && mesh.faceSize(f) == 4; ++guard)
            {
                edge = opposite(f, edge);
                if (edge == kInvalid || edge == e)
                    break;
                ++n;
                f = mesh.oppositeFace(edge, f);
            }
        });
        return n;
    };

    std::vector<Index> out;
    for (Index f : faces)
    {
        if (!mesh.faceValid(f) || mesh.faceSize(f) != 4)
            continue;
        const Index c = mesh.faceCorner(f);
        const Index e0 = mesh.cornerEdge(c), e1 = mesh.cornerEdge(mesh.cornerNext(c));
        const size_t n0 = inRegion(e0), n1 = inRegion(e1);
        bool pick0 = across ? n0 < n1 : n0 > n1;
        if (n0 == n1)
        {
            // A strip's short way crosses its long edges.
            const double l0 = mesh.edgeLength(e0), l1 = mesh.edgeLength(e1);
            pick0 = across ? l0 >= l1 : l0 < l1;
        }
        out.push_back(pick0 ? e0 : e1);
    }
    return out;
}
} // namespace lila::geo
