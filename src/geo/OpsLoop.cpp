#include "geo/Ops.h"
#include "geo/OpsCommon.h"

#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>
#include <limits>

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

std::vector<Index> insertEdgeLoop(Mesh& mesh, Index ringEdge, const EdgeLoopOptions& opts)
{
    LILA_ZONE();

    std::vector<RingEdge> ring;
    std::vector<Index> quads;
    bool closed = false;
    if (!walkRing(mesh, ringEdge, ring, quads, closed) || quads.empty())
        return {};

    std::vector<double> fracs;
    const int count = std::max(1, opts.count);
    if (count == 1)
        fracs.push_back(std::clamp(opts.fraction, 1e-4, 1.0 - 1e-4));
    else
        for (int k = 1; k <= count; ++k)
            fracs.push_back(double(k) / (count + 1));

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

std::vector<Index> bridgeEdgeLoops(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts)
{
    LILA_ZONE();

    std::vector<EdgePath> paths = chainEdges(mesh, edges);
    if (paths.size() != 2 || paths[0].edges.size() != paths[1].edges.size()
        || paths[0].closed != paths[1].closed || paths[0].edges.empty())
        return {};
    for (const EdgePath& p : paths)
        for (Index e : p.edges)
            if (!mesh.isBoundaryEdge(e))
                return {};

    // Orient each path the way a face filling it would run: against the existing face.
    for (EdgePath& p : paths)
    {
        const Index c = mesh.edgeCorner(p.edges[0]);
        if (mesh.cornerPoint(c) == p.points[0])
        {
            if (p.closed)
            {
                std::reverse(p.points.begin() + 1, p.points.end());
                std::reverse(p.edges.begin(), p.edges.end());
            }
            else
            {
                std::reverse(p.points.begin(), p.points.end());
                std::reverse(p.edges.begin(), p.edges.end());
            }
        }
    }
    const std::vector<Index>& P = paths[0].points;
    const std::vector<Index>& Q = paths[1].points;
    const size_t n = P.size();
    const bool closed = paths[0].closed;

    // Partner of P[i] is Q[(s - i) mod n]; pick the shift with the shortest total span.
    size_t shift = n - 1;
    if (closed)
    {
        double best = std::numeric_limits<double>::max();
        for (size_t s = 0; s < n; ++s)
        {
            double sum = 0.0;
            for (size_t i = 0; i < n; ++i)
                sum += glm::distance(mesh.position(P[i]), mesh.position(Q[(s + n - i % n) % n]));
            if (sum < best)
            {
                best = sum;
                shift = s;
            }
        }
    }
    auto partner = [&](size_t i) { return Q[(shift + n - i % n) % n]; };

    // Corner sources: the existing boundary face's corner at each point.
    auto cornerAt = [&](Index p, Index e) {
        const Index c = mesh.edgeCorner(e);
        return mesh.cornerPoint(c) == p ? c : mesh.cornerNext(c);
    };
    const int rows = std::max(0, opts.divisions) + 1;
    std::vector<std::vector<Index>> row(size_t(rows) + 1, std::vector<Index>(n));
    std::vector<std::vector<Index>> src(size_t(rows) + 1, std::vector<Index>(n, kInvalid));
    for (size_t i = 0; i < n; ++i)
    {
        row[0][i] = P[i];
        row[size_t(rows)][i] = partner(i);
        const size_t ei = i < paths[0].edges.size() ? i : i - 1;
        src[0][i] = cornerAt(P[i], paths[0].edges[ei]);
        const Index q = partner(i);
        const auto qi = size_t(std::find(Q.begin(), Q.end(), q) - Q.begin());
        const size_t qe = qi < paths[1].edges.size() ? qi : qi - 1;
        src[size_t(rows)][i] = cornerAt(q, paths[1].edges[qe]);
    }
    for (int r = 1; r < rows; ++r)
        for (size_t i = 0; i < n; ++i)
        {
            const double t = double(r) / rows;
            const Index np = mesh.addPoint(glm::mix(mesh.position(P[i]), mesh.position(partner(i)), t));
            const Index ps[2] = {P[i], partner(i)};
            const float w[2] = {float(1.0 - t), float(t)};
            mesh.pointAttrs().mix(np, ps, w);
            row[size_t(r)][i] = np;
        }

    const Index srcFace = mesh.cornerFace(mesh.edgeCorner(paths[0].edges[0]));
    std::vector<Index> out;
    const size_t quadsPerRow = closed ? n : n - 1;
    for (int r = 0; r < rows; ++r)
        for (size_t i = 0; i < quadsPerRow; ++i)
        {
            const size_t j = (i + 1) % n;
            const Index quad[4] = {row[size_t(r)][i], row[size_t(r)][j], row[size_t(r) + 1][j], row[size_t(r) + 1][i]};
            const Index nf = mesh.addFace(quad, srcFace, {});
            if (nf == kInvalid)
                continue;
            out.push_back(nf);
            const float t0 = float(r) / rows, t1 = float(r + 1) / rows;
            const float ts[4] = {t0, t0, t1, t1};
            const size_t is[4] = {i, j, j, i};
            Index c = mesh.faceCorner(nf);
            for (int k = 0; k < 4; ++k, c = mesh.cornerNext(c))
            {
                const Index s[2] = {src[0][is[k]], src[size_t(rows)][is[k]]};
                const float w[2] = {1.0f - ts[k], ts[k]};
                mesh.cornerAttrs().mix(c, s, w);
            }
        }
    return out;
}
} // namespace lila::geo
