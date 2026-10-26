#include "geo/ArrayMesh.h"

#include "geo/Ops.h"
#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace lila::geo
{
namespace
{
struct CellHash
{
    size_t operator()(const glm::ivec3& c) const
    {
        return size_t(uint32_t(c.x) * 73856093u ^ uint32_t(c.y) * 19349663u ^ uint32_t(c.z) * 83492791u);
    }
};

Index findRoot(std::vector<Index>& parent, Index p)
{
    while (parent[p] != p)
    {
        parent[p] = parent[parent[p]];
        p = parent[p];
    }
    return p;
}

/** True when copies a and b sit next to each other in the layout, so their seams may weld. */
bool neighbours(const ArrayOptions& o, int copies, int a, int b)
{
    const ArrayLayout& l = o.layout;
    if (l.mode == ArrayMode::Grid)
    {
        const glm::ivec3 d = glm::abs(arrayGridCell(l, a) - arrayGridCell(l, b));
        return d.x + d.y + d.z == 1;
    }
    if (std::abs(a - b) == 1)
        return true;
    const bool ends = o.mergeEnds || arrayClosesRing(l);
    return ends && copies > 2 && std::min(a, b) == 0 && std::max(a, b) == copies - 1;
}

/**
 * Removes faces that two different copies both put on the same welded points: the seam walls of a closed wedge, which
 * would otherwise be left inside the ring as non-manifold faces. Edges left without faces go too; returns the walls'
 * points, which may be left loose once welded.
 */
std::vector<Index> removeSharedWalls(Mesh& m, const std::vector<Index>& root, Index perCopy)
{
    std::map<std::vector<Index>, std::vector<Index>> byPoints;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<Index> key = m.facePoints(f);
        for (Index& p : key)
            p = root[p];
        std::ranges::sort(key);
        byPoints[std::move(key)].push_back(f);
    }
    std::vector<Index> walls;
    for (const auto& [key, faces] : byPoints)
    {
        if (faces.size() < 2)
            continue;
        const Index copy = m.facePoints(faces.front()).front() / perCopy;
        const bool shared = std::ranges::any_of(faces, [&](Index f) { return m.facePoints(f).front() / perCopy != copy; });
        if (shared)
            walls.insert(walls.end(), faces.begin(), faces.end());
    }
    std::vector<Index> points;
    for (Index f : walls)
        for (Index p : m.facePoints(f))
            points.push_back(p);
    const std::vector<Index> edges = edgesOfFaces(m, walls);
    for (Index f : walls)
        m.removeFace(f);
    m.removeLoose(edges, {});
    return points;
}

void weldCopies(Mesh& m, const ArrayOptions& o, int copies, Index perCopy)
{
    LILA_ZONE();

    const double tol = std::max(o.tolerance, 1e-9);
    const Index n = m.pointCapacity();
    std::unordered_map<glm::ivec3, std::vector<Index>, CellHash> grid;
    auto cellOf = [&](const glm::dvec3& p) { return glm::ivec3(glm::floor(p / tol)); };
    for (Index p = 0; p < n; ++p)
        if (m.pointValid(p))
            grid[cellOf(m.position(p))].push_back(p);

    std::vector<Index> parent(n);
    std::iota(parent.begin(), parent.end(), Index(0));
    bool any = false;
    for (Index p = 0; p < n; ++p)
    {
        if (!m.pointValid(p))
            continue;
        const glm::dvec3 pos = m.position(p);
        const glm::ivec3 c = cellOf(pos);
        const int copyP = int(p / perCopy);
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    auto it = grid.find(c + glm::ivec3(dx, dy, dz));
                    if (it == grid.end())
                        continue;
                    for (Index q : it->second)
                    {
                        // Each pair once, and never two points of the same copy: those coincide by the modeller's choice.
                        if (q <= p || int(q / perCopy) == copyP || !neighbours(o, copies, copyP, int(q / perCopy)))
                            continue;
                        if (glm::distance(pos, m.position(q)) > tol)
                            continue;
                        const Index a = findRoot(parent, p), b = findRoot(parent, q);
                        if (a != b)
                        {
                            parent[std::max(a, b)] = std::min(a, b);
                            any = true;
                        }
                    }
                }
    }
    if (!any)
        return;
    std::vector<Index> root(n);
    for (Index p = 0; p < n; ++p)
        root[p] = findRoot(parent, p);
    std::vector<Index> wallPoints = removeSharedWalls(m, root, perCopy);
    weldPoints(m, root);
    for (Index& p : wallPoints)
        p = root[p];
    m.removeLoose({}, wallPoints);
}
} // namespace

void transformCopy(Mesh& mesh, const glm::dmat4& xf)
{
    for (glm::dvec3& p : mesh.positions())
        p = glm::dvec3(xf * glm::dvec4(p, 1.0));
    const glm::mat3 nm = glm::mat3(glm::transpose(glm::inverse(glm::dmat3(xf))));
    for (glm::vec3& nrm : mesh.cornerAttrs().get<glm::vec3>(attr::kNormal))
        nrm = glm::normalize(nm * nrm);
    if (glm::determinant(glm::dmat3(xf)) < 0.0)
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
            if (mesh.faceValid(f))
                mesh.flipFace(f);
}

Mesh arrayMesh(const Mesh& in, const ArrayOptions& opts)
{
    LILA_ZONE();

    Mesh base = in;
    base.compact();
    Mesh out = base;
    const ArrayLayout& l = opts.layout;
    const int copies = arrayCopyCount(l);
    const Index perCopy = base.pointCapacity();
    if (copies < 2 || perCopy == 0)
        return out;
    const glm::dmat4 step = arrayStep(l), frameInv = glm::inverse(l.frame);
    glm::dmat4 local(1.0);
    for (int k = 1; k < copies; ++k)
    {
        glm::dmat4 xf;
        if (l.mode == ArrayMode::Grid)
            xf = arrayCopyTransform(l, k);
        else
        {
            local = local * step;
            xf = l.frame * local * frameInv;
        }
        Mesh copy = base;
        transformCopy(copy, xf);
        out.append(copy);
    }
    if (opts.weld)
    {
        weldCopies(out, opts, copies, perCopy);
        out.compact();
    }
    return out;
}
} // namespace lila::geo
