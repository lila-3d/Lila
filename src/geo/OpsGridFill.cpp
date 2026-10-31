#include "geo/Ops.h"
#include "geo/OpsCommon.h"

#include "geo/Symmetry.h"
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
    /** flip winds every face the other way, for a border walked against the faces it fills. */
    Filler(Mesh& m, Index srcFace, bool flip) : mMesh(m), mSrcFace(srcFace), mFlip(flip) {}

    void face(std::initializer_list<const FillPoint*> list)
    {
        std::vector<const FillPoint*> pts(list);
        if (mFlip)
            std::reverse(pts.begin(), pts.end());
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
    bool mFlip;
};

/**
 * The border point to start the grid from: the one whose four grid corners turn the most, so a rectangular hole keeps its
 * corners as the grid's, and a mirrored hole picks the same corners whatever point its border was walked from.
 */
int cornerStart(const std::vector<FillPoint>& loop, int gridLoop)
{
    const int n = int(loop.size());
    std::vector<double> turn(size_t(n), 0.0);
    for (int i = 0; i < n; ++i)
    {
        const glm::dvec3 in = loop[size_t(i)].pos - loop[size_t((i + n - 1) % n)].pos;
        const glm::dvec3 out = loop[size_t((i + 1) % n)].pos - loop[size_t(i)].pos;
        const double len = glm::length(in) * glm::length(out);
        if (len > 0.0)
            turn[size_t(i)] = std::acos(std::clamp(glm::dot(in, out) / len, -1.0, 1.0));
    }
    const int a = gridLoop / 4, b = gridLoop / 2 - a;
    int best = 0;
    double bestScore = -1.0;
    for (int s = 0; s < n; ++s)
    {
        const double score = turn[size_t(s)] + turn[size_t((s + a) % n)] + turn[size_t((s + a + b) % n)] + turn[size_t((s + 2 * a + b) % n)];
        if (score > bestScore + 1e-9)
        {
            bestScore = score;
            best = s;
        }
    }
    return best;
}

/** The border point the grid starts from, the given rotation applied. */
int fillStart(const std::vector<FillPoint>& outer, const GridFillOptions& o)
{
    const int n = int(outer.size());
    const int start = n >= 4 ? cornerStart(outer, n % 2 == 0 ? n : n + 1) : 0;
    return ((start + o.rotate) % n + n) % n;
}

/**
 * Fills a closed border round the centre point, the grid starting at outer[0]. The border runs in the winding the new
 * faces take, or against it with flip.
 */
std::vector<Index> fillLoop(Mesh& m, const std::vector<FillPoint>& outer, const FillPoint& centre, Index srcFace, const GridFillOptions& o,
                            bool flip)
{
    const int n = int(outer.size());
    if (n < 3)
        return {};
    const double s = std::clamp(o.gridSize, 0.05, 1.0);
    Filler fill(m, srcFace, flip);
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
            if (flip)
            {
                std::reverse(ids.begin(), ids.end());
                std::reverse(corners.begin(), corners.end());
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
/** A border to fill: its points in the winding of the faces that fill it, and the faces it replaces, if any. */
struct Patch
{
    std::vector<FillPoint> border;
    FillPoint centre;
    Index srcFace = kInvalid;
    std::vector<Index> faces;
};

/**
 * Fills each patch. A patch that is a symmetry image of one already filled is filled from that one's border mapped
 * across, so both get the same grid point for point and the symmetry still pairs them.
 */
std::vector<Index> fillPatches(Mesh& mesh, std::vector<Patch>& patches, const GridFillOptions& o, const SymmetryMap* sym)
{
    std::unordered_map<Index, size_t> owner;
    for (size_t i = 0; i < patches.size(); ++i)
        for (const FillPoint& p : patches[i].border)
            owner[p.id] = i;
    std::vector<uint8_t> done(patches.size(), 0);
    std::vector<Index> out;
    auto fill = [&](Patch& patch, const std::vector<FillPoint>& border, bool flip) {
        detail::removeFacesClean(mesh, patch.faces);
        const std::vector<Index> made = fillLoop(mesh, border, patch.centre, patch.srcFace, o, flip);
        out.insert(out.end(), made.begin(), made.end());
    };
    for (size_t i = 0; i < patches.size(); ++i)
    {
        if (done[i])
            continue;
        done[i] = 1;
        std::vector<FillPoint> border = patches[i].border;
        const size_t n = border.size();
        if (n >= 3)
            std::rotate(border.begin(), border.begin() + fillStart(border, o), border.end());
        fill(patches[i], border, false);
        for (int g = 1; sym && n >= 3 && g < sym->order(); ++g)
        {
            std::vector<Index> image;
            for (const FillPoint& p : border)
                image.push_back(sym->image(g, p.id));
            const auto it = owner.find(image[0]);
            if (it == owner.end() || done[it->second] || patches[it->second].border.size() != n)
                continue;
            Patch& other = patches[it->second];
            std::unordered_map<Index, size_t> at;
            for (size_t k = 0; k < n; ++k)
                at[other.border[k].id] = k;
            std::vector<FillPoint> mapped;
            for (Index q : image)
                if (const auto found = at.find(q); found != at.end())
                    mapped.push_back(other.border[found->second]);
            if (mapped.size() != n)
                continue;
            done[it->second] = 1;
            const bool flip = other.border[(at[image[0]] + 1) % n].id != image[1];
            fill(other, mapped, flip);
        }
    }
    return out;
}
} // namespace

std::vector<Index> gridFillFaces(Mesh& mesh, std::span<const Index> faces, const GridFillOptions& opts, const SymmetryMap* sym)
{
    LILA_ZONE();

    std::vector<Index> sel;
    for (Index f : uniqueSorted(faces))
        if (mesh.faceValid(f))
            sel.push_back(f);
    std::vector<Patch> found;
    for (std::vector<Index>& patch : patches(mesh, sel))
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
        Patch p;
        const Index first = leaving.begin()->second;
        Index c = first;
        do
        {
            p.border.push_back(borderPoint(mesh, mesh.cornerPoint(c), c));
            const auto next = leaving.find(mesh.cornerPoint(mesh.cornerNext(c)));
            if (next == leaving.end() || p.border.size() > rim.size())
                break;
            c = next->second;
        } while (c != first);
        // Fewer points than rim edges means a second loop: a ring or a patch with holes.
        if (c != first || p.border.size() != rim.size())
            continue;

        std::vector<FillPoint> inside;
        for (Index f : patch)
            mesh.forEachFaceCorner(f, [&](Index fc) {
                const Index q = mesh.cornerPoint(fc);
                if (!leaving.contains(q) && std::none_of(inside.begin(), inside.end(), [&](const FillPoint& r) { return r.id == q; }))
                    inside.push_back(borderPoint(mesh, q, fc));
            });
        p.centre = averagePoint(inside.empty() ? std::span<const FillPoint>(p.border) : std::span<const FillPoint>(inside));
        p.srcFace = mesh.cornerFace(first);
        p.faces = std::move(patch);
        found.push_back(std::move(p));
    }
    return fillPatches(mesh, found, opts, sym);
}

std::vector<Index> gridFillHoles(Mesh& mesh, std::span<const Index> edges, const GridFillOptions& opts, const SymmetryMap* sym)
{
    LILA_ZONE();

    const std::vector<uint8_t> sel = makeMask(mesh.edgeCapacity(), edges);
    std::vector<Patch> found;
    for (const EdgePath& loop : boundaryLoops(mesh))
    {
        if (!loop.closed || loop.points.size() < 3)
            continue;
        bool wanted = edges.empty();
        for (Index e : loop.edges)
            wanted |= sel[e] != 0;
        if (!wanted)
            continue;
        Patch p;
        for (size_t i = 0; i < loop.points.size(); ++i)
            p.border.push_back(borderPoint(mesh, loop.points[i], mesh.cornerNext(mesh.edgeCorner(loop.edges[i]))));
        p.centre = averagePoint(p.border);
        p.srcFace = mesh.cornerFace(mesh.edgeCorner(loop.edges[0]));
        found.push_back(std::move(p));
    }
    return fillPatches(mesh, found, opts, sym);
}
} // namespace lila::geo
