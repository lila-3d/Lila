#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>

namespace lila::geo
{
std::vector<Index> uniqueSorted(std::span<const Index> ids)
{
    std::vector<Index> out(ids.begin(), ids.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<uint8_t> makeMask(size_t n, std::span<const Index> ids)
{
    std::vector<uint8_t> mask(n, 0);
    for (Index i : ids)
        if (i < n)
            mask[i] = 1;
    return mask;
}

std::vector<Index> uvVertexCorners(const Mesh& mesh, std::string_view uvSet, std::span<const Index> corners)
{
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(uvSet);
    std::span<const bool> seam = mesh.edgeAttrs().get<bool>(attr::kSeam);
    std::vector<Index> out, stack;
    std::vector<uint8_t> taken(uv.empty() ? 0 : mesh.cornerCapacity(), 0);
    for (Index c : corners)
    {
        if (!mesh.cornerValid(c))
            continue;
        out.push_back(c);
        if (uv.empty() || taken[c])
            continue;
        taken[c] = 1;
        // Around the point through edges that are not seams, so uvs that only touch across a cut stay behind.
        const Index p = mesh.cornerPoint(c);
        stack.assign(1, c);
        while (!stack.empty())
        {
            const Index k = stack.back();
            stack.pop_back();
            for (Index e : {mesh.cornerEdge(k), mesh.cornerEdge(mesh.cornerPrev(k))})
            {
                if (!seam.empty() && seam[e])
                    continue;
                mesh.forEachEdgeCorner(e, [&](Index r) {
                    const Index at = mesh.cornerPoint(r) == p ? r : mesh.cornerNext(r);
                    if (!taken[at] && glm::all(glm::lessThanEqual(glm::abs(uv[at] - uv[c]), glm::vec2(1e-6f))))
                    {
                        taken[at] = 1;
                        out.push_back(at);
                        stack.push_back(at);
                    }
                });
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Index> faceVertexRows(const Mesh& mesh)
{
    std::vector<Index> rows(mesh.cornerCapacity(), kInvalid);
    Index row = 0;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](Index c) { rows[c] = row++; });
    return rows;
}

std::vector<Index> faceVertexCorners(const Mesh& mesh)
{
    std::vector<Index> corners;
    corners.reserve(mesh.numCorners());
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](Index c) { corners.push_back(c); });
    return corners;
}

namespace
{
/** The edge at p other than e on face f, kInvalid if f does not hold e at p. */
Index otherFaceEdgeAt(const Mesh& mesh, Index f, Index e, Index p)
{
    Index found = kInvalid;
    mesh.forEachFaceCorner(f, [&](Index c) {
        const Index fe = mesh.cornerEdge(c);
        if (fe != e && (mesh.edgePoint(fe, 0) == p || mesh.edgePoint(fe, 1) == p))
            found = fe;
    });
    return found;
}

/**
 * At a valence-3 point, the loop along an n-gon's border (a cap's rim): e and the next edge share the n-gon and every
 * other face at p is a quad.
 */
Index ngonBorderNext(const Mesh& mesh, Index e, Index p)
{
    if (!mesh.isManifoldEdge(e))
        return kInvalid;
    const Index c = mesh.edgeCorner(e);
    Index fa = mesh.cornerFace(c), fb = mesh.oppositeFace(e, fa);
    if (mesh.faceSize(fa) == 4)
        std::swap(fa, fb);
    if (mesh.faceSize(fa) < 5 || mesh.faceSize(fb) != 4)
        return kInvalid;
    const Index next = otherFaceEdgeAt(mesh, fa, e, p);
    if (next == kInvalid || !mesh.isManifoldEdge(next))
        return kInvalid;
    const Index beyond = mesh.oppositeFace(next, fa);
    return beyond != kInvalid && mesh.faceSize(beyond) == 4 ? next : kInvalid;
}
} // namespace

Index edgeLoopNext(const Mesh& mesh, Index e, Index p)
{
    if (mesh.isBoundaryEdge(e))
    {
        // Follow the border through every point where it does not branch.
        Index next = kInvalid;
        int count = 0;
        mesh.forEachPointEdge(p, [&](Index x) {
            if (x != e && mesh.isBoundaryEdge(x))
            {
                next = x;
                ++count;
            }
        });
        return count == 1 ? next : kInvalid;
    }
    if (mesh.isBoundaryPoint(p))
        return kInvalid;
    const Index valence = mesh.pointValence(p);
    if (valence == 3)
        return ngonBorderNext(mesh, e, p);
    if (valence != 4)
        return kInvalid;
    // The edge sharing no face with e.
    std::vector<Index> touching;
    mesh.forEachEdgeCorner(e, [&](Index c) { touching.push_back(otherFaceEdgeAt(mesh, mesh.cornerFace(c), e, p)); });
    Index found = kInvalid;
    mesh.forEachPointEdge(p, [&](Index x) {
        if (x != e && std::find(touching.begin(), touching.end(), x) == touching.end())
            found = x;
    });
    return found;
}

std::vector<Index> edgeLoop(const Mesh& mesh, Index e)
{
    LILA_ZONE();

    if (!mesh.edgeValid(e))
        return {};
    std::vector<Index> forward{e};
    std::vector<uint8_t> seen;
    auto visited = [&](Index x) {
        if (seen.size() < mesh.edgeCapacity())
            seen.assign(mesh.edgeCapacity(), 0);
        return seen[x] != 0;
    };
    visited(e);
    seen[e] = 1;

    // Walk from point 1 forward; if that does not close, walk from point 0 backward.
    Index cur = e;
    Index p = mesh.edgePoint(e, 1);
    bool closed = false;
    for (;;)
    {
        const Index next = edgeLoopNext(mesh, cur, p);
        if (next == kInvalid)
            break;
        if (next == e)
        {
            closed = true;
            break;
        }
        if (visited(next))
            break;
        seen[next] = 1;
        forward.push_back(next);
        p = mesh.edgeOtherPoint(next, p);
        cur = next;
    }
    if (closed)
        return forward;

    std::vector<Index> backward;
    cur = e;
    p = mesh.edgePoint(e, 0);
    for (;;)
    {
        const Index next = edgeLoopNext(mesh, cur, p);
        if (next == kInvalid || visited(next))
            break;
        seen[next] = 1;
        backward.push_back(next);
        p = mesh.edgeOtherPoint(next, p);
        cur = next;
    }
    std::reverse(backward.begin(), backward.end());
    backward.insert(backward.end(), forward.begin(), forward.end());
    return backward;
}

Index quadOppositeEdge(const Mesh& mesh, Index e, Index f)
{
    if (!mesh.faceValid(f) || mesh.faceSize(f) != 4)
        return kInvalid;
    Index found = kInvalid;
    mesh.forEachFaceCorner(f, [&](Index c) {
        if (mesh.cornerEdge(c) == e)
            found = mesh.cornerEdge(mesh.cornerNext(mesh.cornerNext(c)));
    });
    return found;
}

std::vector<Index> edgeRing(const Mesh& mesh, Index e)
{
    LILA_ZONE();

    if (!mesh.edgeValid(e) || mesh.edgeCorner(e) == kInvalid)
        return {e};
    std::vector<uint8_t> seen(mesh.edgeCapacity(), 0);
    seen[e] = 1;

    auto walk = [&](Index startFace, std::vector<Index>& out) -> bool {
        Index cur = e;
        Index f = startFace;
        while (f != kInvalid)
        {
            const Index opp = quadOppositeEdge(mesh, cur, f);
            if (opp == kInvalid)
                return false;
            if (opp == e)
                return true;
            if (seen[opp])
                return false;
            seen[opp] = 1;
            out.push_back(opp);
            f = mesh.oppositeFace(opp, f);
            cur = opp;
        }
        return false;
    };

    const Index c0 = mesh.edgeCorner(e);
    const Index f0 = mesh.cornerFace(c0);
    std::vector<Index> forward{e};
    if (walk(f0, forward))
        return forward;
    std::vector<Index> backward;
    const Index f1 = mesh.oppositeFace(e, f0);
    if (f1 != kInvalid)
        walk(f1, backward);
    std::reverse(backward.begin(), backward.end());
    backward.insert(backward.end(), forward.begin(), forward.end());
    return backward;
}

std::vector<EdgePath> boundaryLoops(const Mesh& mesh)
{
    LILA_ZONE();

    std::vector<EdgePath> out;
    std::vector<uint8_t> seen(mesh.edgeCapacity(), 0);
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e) || seen[e] || !mesh.isBoundaryEdge(e))
            continue;
        EdgePath path;
        // Walk against the face winding so the loop runs the way a filling face would.
        Index c = mesh.edgeCorner(e);
        Index cur = e;
        Index p = mesh.cornerNextPoint(c);
        for (;;)
        {
            seen[cur] = 1;
            path.points.push_back(p);
            path.edges.push_back(cur);
            const Index q = mesh.edgeOtherPoint(cur, p);
            Index next = kInvalid;
            mesh.forEachPointEdge(q, [&](Index x) {
                if (x != cur && !seen[x] && mesh.isBoundaryEdge(x) && next == kInvalid)
                {
                    // The face on x must arrive at q, so the fill direction carries on away from it.
                    const Index xc = mesh.edgeCorner(x);
                    if (mesh.cornerNextPoint(xc) == q)
                        next = x;
                }
            });
            if (next == kInvalid)
            {
                path.closed = (q == path.points.front());
                if (!path.closed)
                    path.points.push_back(q);
                break;
            }
            p = q;
            cur = next;
        }
        out.push_back(std::move(path));
    }
    return out;
}

std::vector<EdgePath> chainEdges(const Mesh& mesh, std::span<const Index> edges)
{
    LILA_ZONE();

    std::vector<uint8_t> in = makeMask(mesh.edgeCapacity(), edges);
    std::vector<uint8_t> used(mesh.edgeCapacity(), 0);
    auto degree = [&](Index p) {
        int d = 0;
        mesh.forEachPointEdge(p, [&](Index x) { d += in[x]; });
        return d;
    };
    auto nextAt = [&](Index p, Index from) {
        Index next = kInvalid;
        if (degree(p) != 2)
            return next;
        mesh.forEachPointEdge(p, [&](Index x) {
            if (x != from && in[x] && !used[x])
                next = x;
        });
        return next;
    };

    std::vector<EdgePath> out;
    auto trace = [&](Index e, Index start) {
        EdgePath path;
        Index p = start;
        Index cur = e;
        while (cur != kInvalid)
        {
            used[cur] = 1;
            path.points.push_back(p);
            path.edges.push_back(cur);
            p = mesh.edgeOtherPoint(cur, p);
            if (p == path.points.front())
            {
                path.closed = true;
                break;
            }
            cur = nextAt(p, cur);
        }
        if (!path.closed)
            path.points.push_back(p);
        out.push_back(std::move(path));
    };

    // Open chains first, starting from ends, then what remains is closed.
    for (Index e : edges)
    {
        if (!mesh.edgeValid(e) || used[e])
            continue;
        for (int s = 0; s < 2; ++s)
            if (!used[e] && degree(mesh.edgePoint(e, s)) != 2)
                trace(e, mesh.edgePoint(e, s));
    }
    for (Index e : edges)
        if (mesh.edgeValid(e) && !used[e])
            trace(e, mesh.edgePoint(e, 0));
    return out;
}

Index faceComponents(const Mesh& mesh, std::vector<Index>& component, std::span<const uint8_t> blockEdge)
{
    LILA_ZONE();

    component.assign(mesh.faceCapacity(), kInvalid);
    Index count = 0;
    std::vector<Index> stack;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f) || component[f] != kInvalid)
            continue;
        component[f] = count;
        stack.push_back(f);
        while (!stack.empty())
        {
            const Index g = stack.back();
            stack.pop_back();
            mesh.forEachFaceCorner(g, [&](Index c) {
                const Index e = mesh.cornerEdge(c);
                if (!blockEdge.empty() && blockEdge[e])
                    return;
                mesh.forEachEdgeCorner(e, [&](Index r) {
                    const Index h = mesh.cornerFace(r);
                    if (component[h] == kInvalid)
                    {
                        component[h] = count;
                        stack.push_back(h);
                    }
                });
            });
        }
        ++count;
    }
    return count;
}

std::vector<Index> regionBoundaryEdges(const Mesh& mesh, std::span<const Index> faces)
{
    LILA_ZONE();

    std::vector<uint8_t> in = makeMask(mesh.faceCapacity(), faces);
    std::vector<Index> out;
    for (Index e : edgesOfFaces(mesh, faces))
    {
        int inside = 0, outside = 0;
        mesh.forEachEdgeCorner(e, [&](Index c) { (in[mesh.cornerFace(c)] ? inside : outside)++; });
        if (inside >= 1 && (outside >= 1 || inside == 1))
            out.push_back(e);
    }
    return out;
}

std::vector<Index> facesOfPoints(const Mesh& mesh, std::span<const Index> points)
{
    std::vector<Index> out;
    for (Index p : points)
        if (mesh.pointValid(p))
            mesh.forEachPointCorner(p, [&](Index c) { out.push_back(mesh.cornerFace(c)); });
    return uniqueSorted(out);
}

std::vector<Index> facesOfEdges(const Mesh& mesh, std::span<const Index> edges)
{
    std::vector<Index> out;
    for (Index e : edges)
        if (mesh.edgeValid(e))
            mesh.forEachEdgeCorner(e, [&](Index c) { out.push_back(mesh.cornerFace(c)); });
    return uniqueSorted(out);
}

std::vector<Index> pointsOfFaces(const Mesh& mesh, std::span<const Index> faces)
{
    std::vector<Index> out;
    for (Index f : faces)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](Index c) { out.push_back(mesh.cornerPoint(c)); });
    return uniqueSorted(out);
}

std::vector<Index> edgesOfFaces(const Mesh& mesh, std::span<const Index> faces)
{
    std::vector<Index> out;
    for (Index f : faces)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](Index c) { out.push_back(mesh.cornerEdge(c)); });
    return uniqueSorted(out);
}

std::vector<Index> pointsOfEdges(const Mesh& mesh, std::span<const Index> edges)
{
    std::vector<Index> out;
    for (Index e : edges)
        if (mesh.edgeValid(e))
        {
            out.push_back(mesh.edgePoint(e, 0));
            out.push_back(mesh.edgePoint(e, 1));
        }
    return uniqueSorted(out);
}
} // namespace lila::geo
