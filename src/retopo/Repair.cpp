#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Ops.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numeric>
#include <unordered_map>

namespace lila::retopo
{
namespace
{
using geo::kInvalid;

struct IssueInfo
{
    IssueElement element;
    const char* name;
    const char* label;
};

constexpr IssueInfo kIssues[kIssueCount] = {
    {IssueElement::Point, "coincident", "coincident points"},
    {IssueElement::Edge, "zeroLengthEdges", "zero-length edges"},
    {IssueElement::Face, "lamina", "lamina faces"},
    {IssueElement::Face, "zeroAreaFaces", "zero-area faces"},
    {IssueElement::Edge, "nonManifoldEdges", "non-manifold edges"},
    {IssueElement::Point, "nonManifoldVertices", "non-manifold vertices"},
    {IssueElement::Edge, "holes", "border edges"},
    {IssueElement::Face, "ngons", "n-gons"},
    {IssueElement::Face, "concave", "concave faces"},
    {IssueElement::Face, "zeroAreaUvs", "zero-area uv faces"},
    {IssueElement::Face, "flippedNormals", "flipped faces"},
};

/** Moves face f's corner off point from onto to, dropping the edges that leaves without faces. */
void replacePoint(geo::Mesh& mesh, Index f, Index from, Index to)
{
    std::vector<Index> pts = mesh.facePoints(f), edges;
    mesh.forEachFaceCorner(f, [&](Index c) { edges.push_back(mesh.cornerEdge(c)); });
    for (Index& p : pts)
        if (p == from)
            p = to;
    mesh.setFacePoints(f, pts);
    mesh.removeLoose(edges, {});
}

Index clonePoint(geo::Mesh& mesh, Index p)
{
    Index q = mesh.addPoint(mesh.position(p));
    mesh.pointAttrs().copy(q, p);
    return q;
}

std::vector<Index> nonManifoldEdges(const geo::Mesh& mesh)
{
    std::vector<Index> out;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e) && mesh.edgeFaceCount(e) > 2)
            out.push_back(e);
    return out;
}

Index splitNonManifoldEdges(geo::Mesh& mesh)
{
    LILA_ZONE();

    Index count = 0;
    for (Index e : nonManifoldEdges(mesh))
    {
        if (!mesh.edgeValid(e) || mesh.edgeFaceCount(e) <= 2)
            continue;
        ++count;
        std::vector<Index> corners;
        mesh.forEachEdgeCorner(e, [&](Index c) { corners.push_back(c); });
        // Keep the first face and the first one running the edge the other way; the rest get their own copy of the edge.
        Index keep = corners[0], partner = kInvalid;
        for (size_t i = 1; i < corners.size() && partner == kInvalid; ++i)
            if (mesh.cornerPoint(corners[i]) != mesh.cornerPoint(keep))
                partner = corners[i];
        if (partner == kInvalid)
            partner = corners[1];
        for (Index c : corners)
        {
            if (c == keep || c == partner)
                continue;
            Index f = mesh.cornerFace(c);
            Index a = mesh.edgePoint(e, 0), b = mesh.edgePoint(e, 1);
            Index na = clonePoint(mesh, a), nb = clonePoint(mesh, b);
            replacePoint(mesh, f, a, na);
            replacePoint(mesh, f, b, nb);
        }
    }
    return count;
}

/**
 * Faces around p grouped into fans joined across shared edges. One fan is a manifold point; a point on a
 * non-manifold edge alone is one fan too, so it is counted as that edge and not again as a vertex.
 */
std::vector<std::vector<Index>> pointFans(const geo::Mesh& mesh, Index p)
{
    std::vector<Index> corners;
    mesh.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
    if (corners.size() < 2)
        return {};
    std::sort(corners.begin(), corners.end());
    std::vector<Index> parent(corners.size());
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    auto slot = [&](Index c) { return Index(std::lower_bound(corners.begin(), corners.end(), c) - corners.begin()); };
    mesh.forEachPointEdge(p, [&](Index e) {
        if (mesh.edgeFaceCount(e) < 2)
            return;
        // The corner at p of each face on e.
        Index first = kInvalid;
        mesh.forEachEdgeCorner(e, [&](Index c) {
            Index at = mesh.cornerPoint(c) == p ? c : mesh.cornerNext(c);
            Index r = find(slot(at));
            if (first == kInvalid)
                first = r;
            else if (Index rf = find(first); rf != r)
                parent[std::max(rf, r)] = std::min(rf, r);
        });
    });
    std::map<Index, std::vector<Index>> fans;
    for (Index i = 0; i < Index(corners.size()); ++i)
        fans[find(i)].push_back(mesh.cornerFace(corners[i]));
    std::vector<std::vector<Index>> out;
    for (auto& [root, faces] : fans)
        out.push_back(std::move(faces));
    return out;
}

std::vector<Index> nonManifoldPoints(const geo::Mesh& mesh)
{
    std::vector<Index> out;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && pointFans(mesh, p).size() > 1)
            out.push_back(p);
    return out;
}

/** Points whose faces form more than one fan get one copy per extra fan. */
Index splitNonManifoldPoints(geo::Mesh& mesh)
{
    LILA_ZONE();

    Index count = 0;
    for (Index p : nonManifoldPoints(mesh))
    {
        std::vector<std::vector<Index>> fans = pointFans(mesh, p);
        if (fans.size() < 2)
            continue;
        ++count;
        for (size_t i = 1; i < fans.size(); ++i)
        {
            Index q = clonePoint(mesh, p);
            for (Index f : fans[i])
                replacePoint(mesh, f, p, q);
        }
    }
    return count;
}

std::vector<Index> coincidentPoints(const geo::Mesh& mesh, double tol)
{
    LILA_ZONE();

    auto cellOf = [&](const glm::dvec3& p) { return glm::i64vec3(glm::floor(p / tol)); };
    auto key = [](const glm::i64vec3& c) {
        return uint64_t(c.x) * 73856093ull ^ uint64_t(c.y) * 19349663ull ^ uint64_t(c.z) * 83492791ull;
    };
    std::unordered_map<uint64_t, std::vector<Index>> grid;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            grid[key(cellOf(mesh.position(p)))].push_back(p);
    std::vector<Index> out;
    const double tol2 = tol * tol;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        const glm::dvec3 pos = mesh.position(p);
        const glm::i64vec3 c = cellOf(pos);
        bool hit = false;
        for (int dx = -1; dx <= 1 && !hit; ++dx)
            for (int dy = -1; dy <= 1 && !hit; ++dy)
                for (int dz = -1; dz <= 1 && !hit; ++dz)
                {
                    auto it = grid.find(key(c + glm::i64vec3(dx, dy, dz)));
                    if (it == grid.end())
                        continue;
                    for (Index q : it->second)
                        if (q != p && glm::dot(mesh.position(q) - pos, mesh.position(q) - pos) <= tol2)
                        {
                            hit = true;
                            break;
                        }
                }
        if (hit)
            out.push_back(p);
    }
    return out;
}

std::vector<Index> zeroLengthEdges(const geo::Mesh& mesh, double tol)
{
    std::vector<Index> out;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e) && mesh.edgeLength(e) <= tol)
            out.push_back(e);
    return out;
}

std::vector<Index> laminaFaces(const geo::Mesh& mesh)
{
    std::vector<Index> out;
    std::map<std::vector<Index>, Index> seen;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        std::vector<Index> key = mesh.facePoints(f);
        std::sort(key.begin(), key.end());
        if (!seen.emplace(std::move(key), f).second)
            out.push_back(f);
    }
    return out;
}

Index longestEdge(const geo::Mesh& mesh, Index f)
{
    Index best = kInvalid;
    double len = -1.0;
    mesh.forEachFaceCorner(f, [&](Index c) {
        double l = mesh.edgeLength(mesh.cornerEdge(c));
        if (l > len)
        {
            len = l;
            best = mesh.cornerEdge(c);
        }
    });
    return best;
}

/** Faces whose height over their longest edge is under the tolerance: slivers and collapsed faces. */
std::vector<Index> zeroAreaFaces(const geo::Mesh& mesh, double tol)
{
    std::vector<Index> out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && mesh.faceArea(f) <= tol * std::max(tol, mesh.edgeLength(longestEdge(mesh, f))))
            out.push_back(f);
    return out;
}

std::vector<Index> borderEdges(const geo::Mesh& mesh)
{
    std::vector<Index> out;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (mesh.edgeValid(e) && mesh.edgeFaceCount(e) == 1)
            out.push_back(e);
    return out;
}

std::vector<Index> ngonFaces(const geo::Mesh& mesh)
{
    std::vector<Index> out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && mesh.faceSize(f) > 4)
            out.push_back(f);
    return out;
}

/** A corner turns against the face normal by more than rounding, so the polygon is not convex. */
bool isConcave(const geo::Mesh& mesh, Index f)
{
    if (mesh.faceSize(f) < 4)
        return false;
    const glm::dvec3 n = mesh.faceNormal(f);
    bool concave = false;
    mesh.forEachFaceCorner(f, [&](Index c) {
        const glm::dvec3 p = mesh.position(mesh.cornerPoint(c));
        const glm::dvec3 a = p - mesh.position(mesh.cornerPoint(mesh.cornerPrev(c)));
        const glm::dvec3 b = mesh.position(mesh.cornerNextPoint(c)) - p;
        if (glm::dot(glm::cross(a, b), n) < -1e-9 * glm::length(a) * glm::length(b))
            concave = true;
    });
    return concave;
}

std::vector<Index> concaveFaces(const geo::Mesh& mesh)
{
    std::vector<Index> out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && isConcave(mesh, f))
            out.push_back(f);
    return out;
}

std::vector<Index> zeroAreaUvFaces(const geo::Mesh& mesh)
{
    std::span<const glm::vec2> uv = mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    if (uv.size() < mesh.cornerCapacity())
        return {};
    std::vector<Index> out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        double twiceArea = 0.0;
        mesh.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec2 a = uv[c], b = uv[mesh.cornerNext(c)];
            twiceArea += a.x * b.y - b.x * a.y;
        });
        // Float uvs: anything under about a thousandth of a 4k texel is no area.
        if (std::abs(twiceArea) * 0.5 < 1e-11)
            out.push_back(f);
    }
    return out;
}

/** The faces conformNormals would turn over. */
std::vector<Index> flippedFaces(const geo::Mesh& mesh)
{
    LILA_ZONE();

    geo::Mesh conformed = mesh;
    geo::conformNormals(conformed);
    std::vector<Index> out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f) || !conformed.faceValid(f))
            continue;
        const Index c = mesh.faceCorner(f);
        const Index cc = conformed.findCorner(f, mesh.cornerPoint(c));
        if (cc != kInvalid && conformed.cornerNextPoint(cc) != mesh.cornerNextPoint(c))
            out.push_back(f);
    }
    return out;
}

std::vector<Index> detect(const geo::Mesh& mesh, MeshIssue issue, double tol)
{
    switch (issue)
    {
    case MeshIssue::Coincident: return coincidentPoints(mesh, tol);
    case MeshIssue::ZeroLengthEdge: return zeroLengthEdges(mesh, tol);
    case MeshIssue::Lamina: return laminaFaces(mesh);
    case MeshIssue::ZeroAreaFace: return zeroAreaFaces(mesh, tol);
    case MeshIssue::NonManifoldEdge: return nonManifoldEdges(mesh);
    case MeshIssue::NonManifoldPoint: return nonManifoldPoints(mesh);
    case MeshIssue::Hole: return borderEdges(mesh);
    case MeshIssue::Ngon: return ngonFaces(mesh);
    case MeshIssue::Concave: return concaveFaces(mesh);
    case MeshIssue::ZeroAreaUv: return zeroAreaUvFaces(mesh);
    case MeshIssue::Flipped: return flippedFaces(mesh);
    case MeshIssue::Count: break;
    }
    return {};
}

/** Triangulates the faces, then pairs the triangles back into convex quads where they make good ones. */
void splitIntoQuadsAndTriangles(geo::Mesh& mesh, std::span<const Index> faces)
{
    std::vector<Index> tris = geo::triangulateFaces(mesh, faces);
    geo::QuadrangulateOptions qo;
    qo.maxFaceAngle = 20.0;
    geo::quadrangulateFaces(mesh, tris, qo);
}

/** Merges each sliver into the face across its longest edge, which keeps the surface closed; slivers on a border just go. */
void dissolveSlivers(geo::Mesh& mesh, std::span<const Index> faces)
{
    for (Index f : faces)
    {
        if (!mesh.faceValid(f))
            continue;
        const Index e = longestEdge(mesh, f);
        if (e == kInvalid || mesh.joinFaces(e) == kInvalid)
            mesh.removeFace(f);
    }
}

Index fix(geo::Mesh& mesh, MeshIssue issue, double tol)
{
    std::vector<Index> found = detect(mesh, issue, tol);
    if (found.empty())
        return 0;
    switch (issue)
    {
    case MeshIssue::Coincident:
    {
        geo::MergeOptions mo;
        mo.distance = tol;
        geo::mergeByDistance(mesh, found, mo);
        break;
    }
    case MeshIssue::ZeroLengthEdge: geo::collapseEdges(mesh, found); break;
    case MeshIssue::Lamina:
        for (Index f : found)
            mesh.removeFace(f);
        break;
    case MeshIssue::ZeroAreaFace: dissolveSlivers(mesh, found); break;
    case MeshIssue::NonManifoldEdge: return splitNonManifoldEdges(mesh);
    case MeshIssue::NonManifoldPoint: return splitNonManifoldPoints(mesh);
    case MeshIssue::Hole: geo::fillHoles(mesh, found); break;
    case MeshIssue::Ngon: splitIntoQuadsAndTriangles(mesh, found); break;
    case MeshIssue::Concave: splitIntoQuadsAndTriangles(mesh, found); break;
    case MeshIssue::ZeroAreaUv: break;
    case MeshIssue::Flipped: geo::flipNormals(mesh, found); break;
    case MeshIssue::Count: break;
    }
    return Index(found.size());
}
} // namespace

IssueElement issueElement(MeshIssue issue) { return kIssues[size_t(issue)].element; }
const char* issueName(MeshIssue issue) { return kIssues[size_t(issue)].name; }
const char* issueLabel(MeshIssue issue) { return kIssues[size_t(issue)].label; }

double RepairOptions::toleranceFor(const geo::Mesh& mesh) const
{
    return tolerance >= 0.0 ? std::max(tolerance, 1e-12) : std::max(1e-9, glm::length(mesh.bounds().size()) * 1e-6);
}

Index MeshIssues::total() const
{
    Index n = 0;
    for (const std::vector<Index>& v : ids)
        n += Index(v.size());
    return n;
}

std::vector<Index> MeshIssues::elements(IssueElement kind) const
{
    std::vector<Index> out;
    for (size_t i = 0; i < kIssueCount; ++i)
        if (issueElement(MeshIssue(i)) == kind)
            out.insert(out.end(), ids[i].begin(), ids[i].end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string MeshIssues::summary() const
{
    std::string s;
    for (size_t i = 0; i < kIssueCount; ++i)
        if (!ids[i].empty())
            s += std::format("{}{} {}", s.empty() ? "" : ", ", ids[i].size(), issueLabel(MeshIssue(i)));
    return s.empty() ? "no issues" : s;
}

MeshIssues findIssues(const geo::Mesh& mesh, const RepairOptions& opts)
{
    LILA_ZONE();

    MeshIssues out;
    const double tol = opts.toleranceFor(mesh);
    for (size_t i = 0; i < kIssueCount; ++i)
        if (opts.checks[i])
            out.ids[i] = detect(mesh, MeshIssue(i), tol);
    return out;
}

RepairReport repairMesh(geo::Mesh& mesh, const RepairOptions& opts)
{
    LILA_ZONE();

    RepairReport r;
    const double tol = opts.toleranceFor(mesh);
    // Enum order is the fix order: welding first leaves real topology for the later checks to judge.
    for (size_t i = 0; i < kIssueCount; ++i)
        if (opts.checks[i])
            r.fixed[i] = fix(mesh, MeshIssue(i), tol);
    if (opts.removeLoose)
    {
        Index before = mesh.numPoints() + mesh.numEdges();
        mesh.removeAllLoose();
        r.looseElements = before - (mesh.numPoints() + mesh.numEdges());
    }
    mesh.compact();
    return r;
}

std::string RepairReport::summary() const
{
    std::string s;
    for (size_t i = 0; i < kIssueCount; ++i)
        if (fixed[i] && MeshIssue(i) != MeshIssue::ZeroAreaUv)
            s += std::format("{}{} {}", s.empty() ? "" : ", ", fixed[i], issueLabel(MeshIssue(i)));
    if (looseElements)
        s += std::format("{}{} loose elements", s.empty() ? "" : ", ", looseElements);
    s = s.empty() ? "nothing to repair" : "fixed " + s;
    if (Index uv = fixed[size_t(MeshIssue::ZeroAreaUv)])
        s += std::format("; {} zero-area uv faces left for a new unwrap", uv);
    return s;
}
} // namespace lila::retopo
