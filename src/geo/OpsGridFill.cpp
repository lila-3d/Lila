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
/** Weighted sources for a fill point's attributes, deduplicated so blends of blends stay small. */
struct Blend
{
    std::vector<Index> src;
    std::vector<float> w;

    void add(const Blend& o, double k)
    {
        for (size_t i = 0; i < o.src.size(); ++i)
        {
            const auto it = std::find(src.begin(), src.end(), o.src[i]);
            if (it == src.end())
            {
                src.push_back(o.src[i]);
                w.push_back(float(o.w[i] * k));
            }
            else
                w[size_t(it - src.begin())] += float(o.w[i] * k);
        }
    }
};

struct FillPoint
{
    Index id = kInvalid;
    glm::dvec3 pos{0.0};
    Blend points, corners;
};

FillPoint borderPoint(const Mesh& m, Index p, Index corner)
{
    FillPoint fp;
    fp.id = p;
    fp.pos = m.position(p);
    fp.points = {{p}, {1.0f}};
    if (corner != kInvalid)
        fp.corners = {{corner}, {1.0f}};
    return fp;
}

/** A new point weighted from others (weights summing to 1), added to the mesh. */
FillPoint blendPoint(Mesh& m, std::initializer_list<std::pair<const FillPoint*, double>> parts)
{
    FillPoint fp;
    for (const auto& [src, k] : parts)
    {
        fp.pos += src->pos * k;
        fp.points.add(src->points, k);
        fp.corners.add(src->corners, k);
    }
    fp.id = m.addPoint(fp.pos);
    m.pointAttrs().mix(fp.id, fp.points.src, fp.points.w);
    return fp;
}

class Filler
{
public:
    Filler(Mesh& m, Index srcFace) : mMesh(m), mSrcFace(srcFace) {}

    void face(std::initializer_list<const FillPoint*> pts)
    {
        std::vector<Index> ids;
        for (const FillPoint* p : pts)
            ids.push_back(p->id);
        const Index f = mMesh.addFace(ids, mSrcFace, {});
        if (f == kInvalid)
            return;
        Index c = mMesh.faceCorner(f);
        for (const FillPoint* p : pts)
        {
            if (!p->corners.src.empty())
                detail::mixCorners(mMesh, c, p->corners.src, p->corners.w);
            c = mMesh.cornerNext(c);
        }
        out.push_back(f);
    }

    /** Coons grid over an even loop of 4 or more, sides a and b with 2(a + b) = loop size. */
    void grid(const std::vector<FillPoint>& loop)
    {
        const int n = int(loop.size());
        if (n == 4)
            return face({&loop[0], &loop[1], &loop[2], &loop[3]});
        const int a = n / 4, b = n / 2 - a;
        std::vector<std::vector<FillPoint>> G(size_t(a) + 1, std::vector<FillPoint>(size_t(b) + 1));
        for (int i = 0; i <= a; ++i)
            G[size_t(i)][0] = loop[size_t(i)];
        for (int j = 0; j <= b; ++j)
            G[size_t(a)][size_t(j)] = loop[size_t(a + j)];
        for (int i = 0; i <= a; ++i)
            G[size_t(i)][size_t(b)] = loop[size_t(a + b + (a - i))];
        for (int j = 0; j <= b; ++j)
            G[0][size_t(j)] = loop[size_t((2 * a + b + (b - j)) % n)];
        auto at = [&](int i, int j) { return &G[size_t(i)][size_t(j)]; };
        for (int i = 1; i < a; ++i)
            for (int j = 1; j < b; ++j)
            {
                const double u = double(i) / a, v = double(j) / b;
                G[size_t(i)][size_t(j)] = blendPoint(mMesh, {{at(i, 0), 1 - v}, {at(i, b), v}, {at(0, j), 1 - u}, {at(a, j), u},
                                                             {at(0, 0), -(1 - u) * (1 - v)}, {at(a, 0), -u * (1 - v)},
                                                             {at(0, b), -(1 - u) * v}, {at(a, b), -u * v}});
            }
        for (int i = 0; i < a; ++i)
            for (int j = 0; j < b; ++j)
                face({at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)});
    }

    std::vector<Index> out;

private:
    Mesh& mMesh;
    Index mSrcFace;
};

/** Fills a closed border, given in the winding the new faces take, round the centre point. */
std::vector<Index> fillLoop(Mesh& m, std::vector<FillPoint> outer, const FillPoint& centre, Index srcFace, const GridFillOptions& o)
{
    const int n = int(outer.size());
    if (n < 3)
        return {};
    std::rotate(outer.begin(), outer.begin() + ((o.rotate % n) + n) % n, outer.end());
    Filler fill(m, srcFace);
    const double s = std::clamp(o.gridSize, 0.05, 1.0);
    if (s >= 1.0)
    {
        if (n % 2 == 0)
            fill.grid(outer);
        else
        {
            std::vector<Index> ids, corners;
            for (const FillPoint& p : outer)
            {
                ids.push_back(p.id);
                corners.push_back(p.corners.src.empty() ? kInvalid : p.corners.src[0]);
            }
            if (const Index f = m.addFace(ids, srcFace, corners); f != kInvalid)
                fill.out.push_back(f);
        }
        return fill.out;
    }

    std::vector<FillPoint> inner;
    for (const FillPoint& p : outer)
        inner.push_back(blendPoint(m, {{&centre, 1 - s}, {&p, s}}));
    const int quads = n % 2 == 0 ? n : n - 1;
    for (int i = 0; i < quads; ++i)
        fill.face({&outer[size_t(i)], &outer[size_t((i + 1) % n)], &inner[size_t((i + 1) % n)], &inner[size_t(i)]});
    if (n % 2 == 1)
    {
        // An odd border gets one extra inner point, so the grid's loop is even, at the cost of one triangle.
        const FillPoint& last = outer.back();
        inner.push_back(blendPoint(m, {{&centre, 1 - s}, {&last, s / 2}, {&outer[0], s / 2}}));
        fill.face({&last, &outer[0], &inner[0], &inner.back()});
        fill.face({&last, &inner.back(), &inner[size_t(n - 1)]});
    }
    fill.grid(inner);
    return fill.out;
}

FillPoint averagePoint(std::span<const FillPoint> pts)
{
    FillPoint c;
    const double k = 1.0 / double(pts.size());
    for (const FillPoint& p : pts)
    {
        c.pos += p.pos * k;
        c.points.add(p.points, k);
        c.corners.add(p.corners, k);
    }
    return c;
}

/** Faces of sel in groups joined through shared edges. */
std::vector<std::vector<Index>> patches(const Mesh& m, std::span<const Index> sel)
{
    const std::vector<uint8_t> in = makeMask(m.faceCapacity(), sel);
    std::vector<uint8_t> seen(m.faceCapacity(), 0);
    std::vector<std::vector<Index>> out;
    for (Index start : sel)
    {
        if (seen[start])
            continue;
        std::vector<Index>& group = out.emplace_back();
        std::vector<Index> stack{start};
        seen[start] = 1;
        while (!stack.empty())
        {
            const Index f = stack.back();
            stack.pop_back();
            group.push_back(f);
            m.forEachFaceCorner(f, [&](Index c) {
                m.forEachEdgeCorner(m.cornerEdge(c), [&](Index r) {
                    const Index g = m.cornerFace(r);
                    if (in[g] && !seen[g])
                    {
                        seen[g] = 1;
                        stack.push_back(g);
                    }
                });
            });
        }
    }
    return out;
}
} // namespace

std::vector<Index> gridFillFaces(Mesh& mesh, std::span<const Index> faces, const GridFillOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> sel;
    for (Index f : uniqueSorted(faces))
        if (mesh.faceValid(f))
            sel.push_back(f);
    std::vector<Index> out;
    for (const std::vector<Index>& patch : patches(mesh, sel))
    {
        const std::vector<uint8_t> in = makeMask(mesh.faceCapacity(), patch);
        const std::vector<Index> rim = regionBoundaryEdges(mesh, patch);
        // Each rim edge's corner inside the patch, keyed by the point it leaves; a point leaving twice is not a disc.
        std::unordered_map<Index, Index> leaving;
        bool disc = !rim.empty();
        for (Index e : rim)
            mesh.forEachEdgeCorner(e, [&](Index c) {
                if (in[mesh.cornerFace(c)])
                    disc &= leaving.emplace(mesh.cornerPoint(c), c).second;
            });
        if (!disc || leaving.size() != rim.size())
            continue;
        std::vector<FillPoint> border;
        const Index first = leaving.begin()->second;
        Index c = first;
        do
        {
            border.push_back(borderPoint(mesh, mesh.cornerPoint(c), c));
            const auto next = leaving.find(mesh.cornerPoint(mesh.cornerNext(c)));
            if (next == leaving.end() || border.size() > rim.size())
                break;
            c = next->second;
        } while (c != first);
        // Fewer points than rim edges means a second loop: a ring or a patch with holes.
        if (c != first || border.size() != rim.size())
            continue;

        std::vector<FillPoint> inside;
        for (Index f : patch)
            mesh.forEachFaceCorner(f, [&](Index fc) {
                const Index p = mesh.cornerPoint(fc);
                if (!leaving.contains(p) && std::none_of(inside.begin(), inside.end(), [&](const FillPoint& q) { return q.id == p; }))
                    inside.push_back(borderPoint(mesh, p, fc));
            });
        const FillPoint centre = averagePoint(inside.empty() ? std::span<const FillPoint>(border) : std::span<const FillPoint>(inside));
        const Index srcFace = mesh.cornerFace(leaving.begin()->second);
        detail::removeFacesClean(mesh, patch);
        const std::vector<Index> made = fillLoop(mesh, std::move(border), centre, srcFace, opts);
        out.insert(out.end(), made.begin(), made.end());
    }
    return out;
}

std::vector<Index> gridFillHoles(Mesh& mesh, std::span<const Index> edges, const GridFillOptions& opts)
{
    LILA_ZONE();

    const std::vector<uint8_t> sel = makeMask(mesh.edgeCapacity(), edges);
    std::vector<Index> out;
    for (const EdgePath& loop : boundaryLoops(mesh))
    {
        if (!loop.closed || loop.points.size() < 3)
            continue;
        bool wanted = edges.empty();
        for (Index e : loop.edges)
            wanted |= sel[e] != 0;
        if (!wanted)
            continue;
        std::vector<FillPoint> border;
        for (size_t i = 0; i < loop.points.size(); ++i)
            border.push_back(borderPoint(mesh, loop.points[i], mesh.cornerNext(mesh.edgeCorner(loop.edges[i]))));
        const FillPoint centre = averagePoint(border);
        const Index srcFace = mesh.cornerFace(mesh.edgeCorner(loop.edges[0]));
        const std::vector<Index> made = fillLoop(mesh, std::move(border), centre, srcFace, opts);
        out.insert(out.end(), made.begin(), made.end());
    }
    return out;
}
} // namespace lila::geo
