#include "retopo/Construct.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lila::retopo
{
namespace
{
constexpr std::array<const char*, 7> kKindNames = {"patch", "strip", "stroke", "bridge", "sweep", "cylinder", "region"};

/** The existing point within weld of p, else a new one. */
Index pointNear(geo::Mesh& m, const glm::dvec3& p, double weld)
{
    Index best = geo::kInvalid;
    double bestD = weld * weld;
    for (Index q = 0; q < m.pointCapacity(); ++q)
        if (m.pointValid(q))
        {
            glm::dvec3 d = m.position(q) - p;
            if (double d2 = glm::dot(d, d); d2 <= bestD)
            {
                bestD = d2;
                best = q;
            }
        }
    return best != geo::kInvalid ? best : m.addPoint(p);
}

/** Adds a face wound against the faces it shares edges with; kInvalid when it exists or would make a non-manifold edge. */
Index addOriented(geo::Mesh& m, std::vector<Index> pts)
{
    std::vector<Index> sorted = pts;
    std::sort(sorted.begin(), sorted.end());
    if (pts.size() < 3 || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return geo::kInvalid;
    bool sameWay = false;
    for (size_t k = 0; k < pts.size(); ++k)
    {
        Index a = pts[k], b = pts[(k + 1) % pts.size()];
        Index e = m.findEdge(a, b);
        if (e == geo::kInvalid)
            continue;
        if (m.edgeFaceCount(e) >= 2)
            return geo::kInvalid;
        m.forEachEdgeCorner(e, [&](Index c) { sameWay |= m.cornerPoint(c) == a; });
    }
    if (sameWay)
        std::reverse(pts.begin(), pts.end());
    if (Index e0 = m.findEdge(pts[0], pts[1]); e0 != geo::kInvalid)
    {
        bool exists = false;
        m.forEachEdgeCorner(e0, [&](Index c) {
            std::vector<Index> fp = m.facePoints(m.cornerFace(c));
            std::sort(fp.begin(), fp.end());
            exists |= fp == sorted;
        });
        if (exists)
            return geo::kInvalid;
    }
    return m.addFace(pts);
}
} // namespace

const char* constructKindName(ConstructKind k) { return kKindNames[size_t(k)]; }

bool constructKindFromName(std::string_view name, ConstructKind& out)
{
    for (size_t i = 0; i < kKindNames.size(); ++i)
        if (name == kKindNames[i])
        {
            out = ConstructKind(i);
            return true;
        }
    return false;
}

ConstructKind constructKind(const ConstructSpec& spec) { return ConstructKind(spec.index()); }

StitchResult stitchConstruct(geo::Mesh& mesh, const GenResult& gen, int id, double weld, const std::optional<geo::SymmetryPlane>& symmetry)
{
    LILA_ZONE();

    StitchResult out;
    std::vector<glm::dvec3> pos = gen.positions;
    if (symmetry)
        for (glm::dvec3& p : pos)
            if (std::abs(symmetry->distance(p)) < std::max(weld, 1e-9))
                p = symmetry->project(p);

    const Index before = mesh.pointCapacity();
    auto place = [&](const std::vector<glm::dvec3>& ps) {
        std::vector<Index> ids(ps.size(), geo::kInvalid);
        for (size_t i = 0; i < ps.size(); ++i)
        {
            bool fixed = i < gen.fixed.size() && gen.fixed[i];
            ids[i] = pointNear(mesh, ps[i], fixed ? std::max(weld, 1e-6) : weld);
        }
        return ids;
    };
    std::vector<Index> ids = place(pos);
    std::vector<Index> mirrorIds;
    if (symmetry)
    {
        std::vector<glm::dvec3> mirrored(pos.size());
        for (size_t i = 0; i < pos.size(); ++i)
            mirrored[i] = symmetry->reflect(pos[i]);
        mirrorIds = place(mirrored);
    }

    mesh.faceAttrs().getOrAdd<int32_t>(kConstructAttr);
    auto add = [&](const std::vector<Index>& local, const std::vector<Index>& map) {
        std::vector<Index> pts;
        for (Index i : local)
            pts.push_back(map[i]);
        Index f = addOriented(mesh, pts);
        if (f == geo::kInvalid)
            return;
        mesh.faceAttrs().get<int32_t>(kConstructAttr)[f] = id;
        out.faces.push_back(f);
    };
    for (const std::vector<Index>& face : gen.faces)
        add(face, ids);
    if (symmetry)
        for (const std::vector<Index>& face : gen.faces)
            add(face, mirrorIds);

    for (Index p = before; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            out.movable.push_back(p);
    std::sort(out.faces.begin(), out.faces.end());
    return out;
}

std::vector<Index> constructFaces(const geo::Mesh& mesh, int id)
{
    std::vector<Index> out;
    std::span<const int32_t> tags = mesh.faceAttrs().get<int32_t>(kConstructAttr);
    if (tags.empty() || id == 0)
        return out;
    for (Index f = 0; f < mesh.faceCapacity() && f < tags.size(); ++f)
        if (mesh.faceValid(f) && tags[f] == id)
            out.push_back(f);
    return out;
}

void removeConstructFaces(geo::Mesh& mesh, int id)
{
    std::vector<Index> faces = constructFaces(mesh, id);
    if (!faces.empty())
        geo::deleteFaces(mesh, faces);
}

void untagConstruct(geo::Mesh& mesh, int id)
{
    for (int32_t& t : mesh.faceAttrs().get<int32_t>(kConstructAttr))
        if (t == id)
            t = 0;
}

uint64_t constructHash(const geo::Mesh& mesh, int id)
{
    std::vector<Index> faces = constructFaces(mesh, id);
    geo::Bounds b = mesh.bounds();
    double size = glm::length(b.max - b.min);
    double quantum = std::max(1e-9, (std::isfinite(size) ? size : 1.0) * 1e-6);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int64_t v) {
        h ^= uint64_t(v);
        h *= 1099511628211ull;
    };
    mix(int64_t(faces.size()));
    for (Index f : faces)
    {
        mix(int64_t(mesh.faceSize(f)));
        for (Index p : mesh.facePoints(f))
            for (int k = 0; k < 3; ++k)
                mix(std::llround(mesh.position(p)[k] / quantum));
    }
    return h;
}

GenResult gridBetween(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b, int rows, const geo::MeshBvh* surface)
{
    GenResult r;
    if (a.size() != b.size() || a.size() < 2 || rows < 1)
    {
        r.error = "the two sides need the same number of points, at least two";
        return r;
    }
    const size_t n = a.size();
    for (int j = 0; j <= rows; ++j)
        for (size_t i = 0; i < n; ++i)
        {
            glm::dvec3 p = j == 0 ? a[i] : j == rows ? b[i] : glm::mix(a[i], b[i], double(j) / rows);
            if (surface && j > 0 && j < rows)
                if (geo::BvhClosest c; surface->closestPoint(p, c))
                    p = c.point;
            r.positions.push_back(p);
        }
    r.fixed.assign(r.positions.size(), 0);
    for (int j = 0; j < rows; ++j)
        for (size_t i = 0; i + 1 < n; ++i)
        {
            Index p0 = Index(j * n + i), p1 = Index(j * n + i + 1), p2 = Index((j + 1) * n + i + 1), p3 = Index((j + 1) * n + i);
            r.faces.push_back({p0, p1, p2, p3});
        }
    return r;
}
} // namespace lila::retopo
