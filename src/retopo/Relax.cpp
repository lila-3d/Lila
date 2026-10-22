#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "geo/Normals.h"

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <optional>

namespace lila::retopo
{
namespace
{
bool uvSplitsAt(const geo::Mesh& m, Index e, std::span<const glm::vec2> uv)
{
    const Index c0 = m.edgeCorner(e), c1 = m.cornerRadial(c0);
    if (c1 == c0 || c0 >= uv.size() || c1 >= uv.size())
        return false;
    // c0 runs a -> b in its face; the other face's corner at a is c1 or the one after it, depending on winding.
    const Index a0 = c0, b0 = m.cornerNext(c0);
    const bool flipped = m.cornerPoint(c1) == m.cornerPoint(c0);
    const Index a1 = flipped ? c1 : m.cornerNext(c1), b1 = flipped ? m.cornerNext(c1) : c1;
    auto differs = [](glm::vec2 x, glm::vec2 y) { return glm::length(x - y) > 1e-6f; };
    return differs(uv[a0], uv[a1]) || differs(uv[b0], uv[b1]);
}
} // namespace

std::vector<uint8_t> featurePoints(const geo::Mesh& mesh, const FeatureOptions& opts)
{
    LILA_ZONE();

    std::vector<uint8_t> out(mesh.pointCapacity(), 0);
    std::span<const float> crease = mesh.edgeAttrs().get<float>(geo::attr::kCrease);
    std::span<const bool> hard = mesh.edgeAttrs().get<bool>(geo::attr::kHard);
    std::span<const bool> seam = mesh.edgeAttrs().get<bool>(geo::attr::kSeam);
    std::vector<std::span<const glm::vec2>> uvs;
    if (opts.uvSeams)
        for (int i = 0; i < 8; ++i)
            if (auto uv = mesh.cornerAttrs().get<glm::vec2>(geo::attr::uvName(i)); !uv.empty())
                uvs.push_back(uv);
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        bool pin = opts.boundary && !mesh.isManifoldEdge(e);
        pin |= opts.creases && e < crease.size() && crease[e] > 0.0f;
        pin |= opts.hardEdges && e < hard.size() && hard[e];
        if (opts.uvSeams && !pin)
        {
            pin = e < seam.size() && seam[e];
            for (size_t i = 0; i < uvs.size() && !pin; ++i)
                pin = mesh.isManifoldEdge(e) && uvSplitsAt(mesh, e, uvs[i]);
        }
        if (pin)
            out[mesh.edgePoint(e, 0)] = out[mesh.edgePoint(e, 1)] = 1;
    }
    return out;
}

namespace
{
enum class Role : uint8_t
{
    Surface,
    Line,
    Fixed,
};

/** Interior angle of the face at corner c, reflex corners of concave faces included. */
double interiorAngle(const geo::Mesh& m, Index c, const glm::dvec3& faceNormal)
{
    const glm::dvec3 p = m.position(m.cornerPoint(c));
    const glm::dvec3 a = m.position(m.cornerPoint(m.cornerPrev(c))) - p, b = m.position(m.cornerNextPoint(c)) - p;
    const double ang = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
    return glm::dot(glm::cross(b, a), faceNormal) < 0.0 ? glm::two_pi<double>() - ang : ang;
}

struct Crease
{
    std::vector<Role> role;
    /** Per line point: its two neighbours along the crease. */
    std::vector<std::array<Index, 2>> along;
    /** Per line point: the starting crease segments it may slide over. */
    std::vector<std::vector<std::array<glm::dvec3, 2>>> segments;
};

/**
 * Sorts points into free surface points, points on a crease or border (which slide along it) and points that stay:
 * crease corners and ends, tips with a large angle deficit, and points on non-manifold edges.
 */
Crease classify(const geo::Mesh& m, const std::vector<glm::dvec3>& fn, const RelaxOptions& opts)
{
    const Index n = m.pointCapacity();
    std::span<const bool> hard = m.edgeAttrs().get<bool>(geo::attr::kHard);
    const double cosSharp = opts.sharpAngle >= 0.0 ? std::cos(glm::radians(opts.sharpAngle)) : -2.0;
    std::vector<uint8_t> creaseEdge(m.edgeCapacity(), 0);
    std::vector<uint8_t> fixed(n, 0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const Index count = m.edgeFaceCount(e);
        if (count > 2)
            fixed[m.edgePoint(e, 0)] = fixed[m.edgePoint(e, 1)] = 1;
        else if (count == 1)
            creaseEdge[e] = 1;
        else if (count == 2)
        {
            const Index c = m.edgeCorner(e);
            creaseEdge[e] = glm::dot(fn[m.cornerFace(c)], fn[m.cornerFace(m.cornerRadial(c))]) < cosSharp
                         || (opts.hardEdgesAreCreases && e < hard.size() && hard[e]);
        }
    }

    Crease out;
    out.role.assign(n, Role::Surface);
    out.along.assign(n, {geo::kInvalid, geo::kInvalid});
    out.segments.resize(n);
    const double cosTurn = std::cos(glm::radians(opts.sharpAngle >= 0.0 ? opts.sharpAngle : 30.0));
    for (Index p = 0; p < n; ++p)
    {
        if (!m.pointValid(p))
            continue;
        Index count = 0;
        m.forEachPointEdge(p, [&](Index e) {
            if (creaseEdge[e])
            {
                if (count < 2)
                    out.along[p][count] = m.edgeOtherPoint(e, p);
                ++count;
            }
        });
        if (fixed[p] || (count != 0 && count != 2))
        {
            out.role[p] = Role::Fixed;
            continue;
        }
        if (count == 2)
        {
            const glm::dvec3 a = m.position(p) - m.position(out.along[p][0]), b = m.position(out.along[p][1]) - m.position(p);
            const double la = glm::length(a), lb = glm::length(b);
            out.role[p] = la > 0.0 && lb > 0.0 && glm::dot(a, b) >= cosTurn * la * lb ? Role::Line : Role::Fixed;
            continue;
        }
        if (opts.cornerAngle >= 0.0 && !m.isBoundaryPoint(p))
        {
            double sum = 0.0;
            m.forEachPointCorner(p, [&](Index c) { sum += interiorAngle(m, c, fn[m.cornerFace(c)]); });
            if (std::abs(glm::two_pi<double>() - sum) > glm::radians(opts.cornerAngle))
                out.role[p] = Role::Fixed;
        }
    }
    // A line point can drift about as far as its neighbours do, so it may land on any crease edge a few steps along.
    for (Index p = 0; p < n; ++p)
    {
        if (out.role[p] != Role::Line)
            continue;
        std::vector<Index> near = {p};
        for (size_t k = 0; k < near.size() && k < 7; ++k)
            for (Index q : out.along[near[k]])
                if (q != geo::kInvalid && std::find(near.begin(), near.end(), q) == near.end())
                    near.push_back(q);
        for (Index a : near)
            m.forEachPointEdge(a, [&](Index e) {
                if (!creaseEdge[e])
                    return;
                const std::array<glm::dvec3, 2> seg = {m.position(m.edgePoint(e, 0)), m.position(m.edgePoint(e, 1))};
                if (std::find(out.segments[p].begin(), out.segments[p].end(), seg) == out.segments[p].end())
                    out.segments[p].push_back(seg);
            });
    }
    return out;
}

glm::dvec3 closestOnSegments(const glm::dvec3& x, const std::vector<std::array<glm::dvec3, 2>>& segs)
{
    glm::dvec3 best = x;
    double bestD = std::numeric_limits<double>::max();
    for (const auto& [a, b] : segs)
    {
        const glm::dvec3 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(x - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec3 q = a + ab * t;
        const double d = glm::dot(x - q, x - q);
        if (d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

/** Puts back the points of faces that a step folded over or collapsed, until none are left. */
void undoFolds(geo::Mesh& mesh, const std::vector<glm::dvec3>& before, const std::vector<uint8_t>& moved,
               const std::vector<glm::dvec3>& startNormal)
{
    for (int pass = 0; pass < 16; ++pass)
    {
        std::vector<Index> revert;
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
        {
            if (!mesh.faceValid(f))
                continue;
            bool touched = false;
            mesh.forEachFaceCorner(f, [&](Index c) { touched = touched || mesh.position(mesh.cornerPoint(c)) != before[mesh.cornerPoint(c)]; });
            if (!touched)
                continue;
            glm::dvec3 old(0.0);
            const Index c0 = mesh.faceCorner(f);
            const glm::dvec3 o0 = before[mesh.cornerPoint(c0)];
            for (Index c = mesh.cornerNext(c0); mesh.cornerNext(c) != c0; c = mesh.cornerNext(c))
                old += glm::cross(before[mesh.cornerPoint(c)] - o0, before[mesh.cornerNextPoint(c)] - o0);
            const double oldLen2 = glm::dot(old, old);
            const glm::dvec3 now = mesh.faceAreaVector(f);
            // Folded, shrunk to a sliver of its former area, or turned away from where it started over several steps.
            if (oldLen2 > 0.0 && (glm::dot(now, old) < 0.05 * oldLen2 || glm::dot(now, startNormal[f]) <= 0.0))
                mesh.forEachFaceCorner(f, [&](Index c) { revert.push_back(mesh.cornerPoint(c)); });
        }
        if (revert.empty())
            return;
        for (Index p : revert)
            if (moved[p])
                mesh.setPosition(p, before[p]);
    }
}
} // namespace

void relaxMesh(geo::Mesh& mesh, const geo::MeshBvh* target, const RelaxOptions& opts)
{
    LILA_ZONE();

    const Index n = mesh.pointCapacity();
    std::vector<glm::dvec3> startNormal(mesh.faceCapacity());
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            startNormal[f] = mesh.faceNormal(f);
    const bool external = target != nullptr;
    std::optional<geo::MeshBvh> own;
    if (!target && opts.project)
    {
        own.emplace(mesh);
        target = &*own;
    }
    // Wrapping onto another surface: the mesh's own creases and tips are just where it happens to be now.
    RelaxOptions featureOpts = opts;
    if (external)
    {
        featureOpts.sharpAngle = -1.0;
        featureOpts.hardEdgesAreCreases = false;
        featureOpts.cornerAngle = -1.0;
    }
    const Crease crease = classify(mesh, startNormal, featureOpts);
    std::vector<uint8_t> moves(n, 0);
    for (Index p = 0; p < n; ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        bool on = opts.active.empty() || (p < opts.active.size() && opts.active[p]);
        if (p < opts.pinned.size() && opts.pinned[p])
            on = false;
        if (opts.pinBoundary && mesh.isBoundaryPoint(p))
            on = false;
        moves[p] = on && crease.role[p] != Role::Fixed;
    }
    const double strength = std::clamp(opts.strength, 0.0, 1.0);
    std::vector<glm::dvec3> next(n), before(n);
    for (int it = 0; it < opts.iterations; ++it)
    {
        std::vector<glm::vec3> normals = geo::pointNormals(mesh);
        for (Index p = 0; p < n; ++p)
            if (mesh.pointValid(p))
                before[p] = mesh.position(p);
        tbb::parallel_for(Index(0), n, [&](Index p) {
            if (!moves[p])
                return;
            const glm::dvec3 at = mesh.position(p);
            next[p] = at;
            if (crease.role[p] == Role::Line)
            {
                // Along the crease toward the farther neighbour, by half the difference: evens the spacing without
                // cutting across a curved crease.
                const glm::dvec3 va = mesh.position(crease.along[p][0]) - at, vb = mesh.position(crease.along[p][1]) - at;
                const double la = glm::length(va), lb = glm::length(vb);
                const glm::dvec3 t = vb / std::max(lb, 1e-300) - va / std::max(la, 1e-300);
                const double lt = glm::length(t);
                if (lt > 0.0)
                    next[p] = closestOnSegments(at + t / lt * (0.5 * (lb - la) * strength), crease.segments[p]);
                return;
            }
            const glm::dvec3 nn(normals[p]);
            glm::dvec3 sum(0.0);
            int count = 0;
            mesh.forEachPointEdge(p, [&](Index e) {
                glm::dvec3 v = mesh.position(mesh.edgeOtherPoint(e, p)) - at;
                if (opts.tangential)
                {
                    // Each neighbour laid flat into the tangent plane at its full distance: on a curved surface the
                    // plain average leans toward the inside of the bend, which would drag points along it.
                    const glm::dvec3 t = v - nn * glm::dot(nn, v);
                    const double lt = glm::length(t);
                    v = lt > 0.0 ? t * (glm::length(v) / lt) : glm::dvec3(0.0);
                }
                sum += v;
                ++count;
            });
            if (count == 0)
                return;
            const glm::dvec3 d = sum / double(count) * strength;
            next[p] = at + d;
            geo::BvhClosest c;
            if (target && target->closestPoint(at + d, c))
            {
                // On the own surface a hit facing away is the far side of a thin wall: stay rather than jump through.
                const bool farSide = own && c.face < startNormal.size() && glm::dot(startNormal[c.face], nn) < 0.0;
                next[p] = farSide ? at : c.point;
            }
        });
        for (Index p = 0; p < n; ++p)
            if (moves[p])
                mesh.setPosition(p, next[p]);
        undoFolds(mesh, before, moves, startNormal);
    }
}

void shrinkwrap(geo::Mesh& mesh, const geo::MeshBvh& target, int relaxIterations)
{
    LILA_ZONE();

    tbb::parallel_for(Index(0), mesh.pointCapacity(), [&](Index p) {
        if (!mesh.pointValid(p))
            return;
        geo::BvhClosest c;
        if (target.closestPoint(mesh.position(p), c))
            mesh.setPosition(p, c.point);
    });
    if (relaxIterations > 0)
    {
        RelaxOptions o;
        o.iterations = relaxIterations;
        o.pinBoundary = false;
        o.sharpAngle = -1.0;
        o.hardEdgesAreCreases = false;
        o.cornerAngle = -1.0;
        relaxMesh(mesh, &target, o);
    }
}

Index projectMesh(geo::Mesh& mesh, const geo::Mesh& target, const geo::MeshBvh& bvh, const ProjectOptions& opts)
{
    LILA_ZONE();

    if (bvh.empty())
        return 0;
    std::vector<glm::vec3> normals;
    if (opts.method == ProjectMethod::Normal)
        normals = geo::pointNormals(mesh);
    const double maxD = opts.maxDistance < 0.0 ? std::numeric_limits<double>::max() : opts.maxDistance;
    std::atomic<Index> moved{0};
    tbb::parallel_for(Index(0), mesh.pointCapacity(), [&](Index p) {
        if (!mesh.pointValid(p) || (!opts.active.empty() && (p >= opts.active.size() || !opts.active[p])))
            return;
        const glm::dvec3 at = mesh.position(p);
        glm::dvec3 hitPoint(0.0);
        Index face = geo::kInvalid;
        bool found = false;
        if (opts.method != ProjectMethod::Closest)
        {
            glm::dvec3 dir = opts.method == ProjectMethod::Normal ? glm::dvec3(normals[p]) : opts.direction;
            if (glm::length(dir) > 1e-12)
            {
                dir = glm::normalize(dir);
                geo::BvhHit fwd, back;
                bool f = bvh.raycast(at, dir, fwd, maxD);
                bool b = bvh.raycast(at, -dir, back, maxD);
                if (f || b)
                {
                    const geo::BvhHit& h = f && (!b || fwd.t <= back.t) ? fwd : back;
                    hitPoint = h.point;
                    face = h.face;
                    found = true;
                }
            }
        }
        if (!found)
        {
            geo::BvhClosest c;
            if (!bvh.closestPoint(at, c, maxD))
                return;
            hitPoint = c.point;
            face = c.face;
        }
        if (opts.offset != 0.0 && target.faceValid(face))
            hitPoint += target.faceNormal(face) * opts.offset;
        mesh.setPosition(p, hitPoint);
        moved.fetch_add(1, std::memory_order_relaxed);
    });
    return moved.load();
}
} // namespace lila::retopo
