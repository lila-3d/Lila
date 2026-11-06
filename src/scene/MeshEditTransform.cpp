#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/MeshEditInternal.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/SoftSelect.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>
#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>
#include <glm/gtc/matrix_transform.hpp>

namespace lila::scene
{
namespace
{
using components::Ids;
using geo::Index;

/** How many pieces the points make, joined by the mesh's edges between them. */
int selectionPieces(const geo::Mesh& m, const Ids& points)
{
    std::set<Index> open(points.begin(), points.end());
    int pieces = 0;
    while (!open.empty())
    {
        ++pieces;
        std::vector<Index> stack{*open.begin()};
        open.erase(open.begin());
        while (!stack.empty())
        {
            const Index p = stack.back();
            stack.pop_back();
            m.forEachPointEdge(p, [&](Index e) {
                if (auto it = open.find(m.edgeOtherPoint(e, p)); it != open.end())
                {
                    stack.push_back(*it);
                    open.erase(it);
                }
            });
        }
    }
    return pieces;
}

/** The wedge holding most of points; the one last picked with symmetry when it holds as many. */
int driveWedge(NodeId node, const geo::SymmetryMap& sym, const Ids& points)
{
    const int picked = components::pickedWedge(node);
    if (picked < 0 || picked >= sym.order())
        return sym.majorityWedge(points);
    std::vector<int64_t> count(size_t(sym.order()), 0);
    for (Index p : points)
        if (const int w = sym.wedge(p); w >= 0)
            ++count[size_t(w)];
    return count[size_t(picked)] == *std::max_element(count.begin(), count.end()) ? picked : sym.majorityWedge(points);
}

/** World-space distance along the edges from the nearest selected point, up to limit (infinity beyond it). */
std::vector<double> surfaceDistances(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected, double limit)
{
    LILA_ZONE();

    std::vector<double> dist(m.pointCapacity(), std::numeric_limits<double>::infinity());
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    for (Index p : selected)
        if (p < dist.size() && m.pointValid(p))
        {
            dist[p] = 0.0;
            open.push({0.0, p});
        }
    auto wp = [&](Index p) { return glm::dvec3(world * glm::dvec4(m.position(p), 1.0)); };
    while (!open.empty())
    {
        auto [d, p] = open.top();
        open.pop();
        if (d > dist[p])
            continue;
        glm::dvec3 at = wp(p);
        m.forEachPointEdge(p, [&](Index e) {
            Index q = m.edgePoint(e, 0) == p ? m.edgePoint(e, 1) : m.edgePoint(e, 0);
            double nd = d + glm::length(wp(q) - at);
            if (nd < dist[q] && nd < limit)
            {
                dist[q] = nd;
                open.push({nd, q});
            }
        });
    }
    return dist;
}
} // namespace

glm::dvec3 selectionCenter()
{
    Graph& g = detail::graph();
    g.evaluate();
    ComponentMode kind = components::mode();
    glm::dvec3 sum(0.0);
    size_t count = 0;
    if (kind == ComponentMode::Object)
        return sum;
    for (auto& [node, ids] : components::selected(kind))
    {
        Node* n = g.node(node);
        if (const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr)
        {
            glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            std::set<uint32_t> want(ids.begin(), ids.end());
            for (auto& [id, p] : pp->points(g, node))
                if (want.contains(id))
                {
                    sum += glm::dvec3(w * glm::dvec4(p, 1.0));
                    ++count;
                }
            continue;
        }
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        // The object-space sum only changes with the mesh or the selection; the manipulator asks every frame.
        struct Cached
        {
            std::weak_ptr<const geo::Mesh> cage;
            uint64_t serial = 0;
            ComponentMode kind = ComponentMode::Object;
            glm::dvec3 sum{0.0};
            size_t count = 0;
        };
        static std::unordered_map<NodeId, Cached> cache;
        Cached& c = cache[node];
        if (c.cage.lock() != cage || c.serial != components::serial() || c.kind != kind)
        {
            const geo::Mesh& m = *cage;
            c = {cage, components::serial(), kind};
            for (Index p : components::convert(m, kind, ids, ComponentMode::Vertex))
                if (m.pointValid(p))
                {
                    c.sum += m.position(p);
                    ++c.count;
                }
        }
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        sum += glm::dvec3(w * glm::dvec4(c.sum, double(c.count)));
        count += c.count;
    }
    return count ? sum / double(count) : sum;
}

glm::dvec3 symmetricSelectionCenter()
{
    Graph& g = detail::graph();
    g.evaluate();
    const ComponentMode kind = components::mode();
    if (kind == ComponentMode::Object)
        return glm::dvec3(0.0);
    glm::dvec3 sum(0.0);
    size_t count = 0;
    for (auto& [node, ids] : components::selected(kind))
    {
        Node* n = g.node(node);
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            return selectionCenter();
        const geo::Mesh& m = *cage;
        const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
        for (Index p : components::convert(m, kind, drivingSide(node, kind, ids, cage), ComponentMode::Vertex))
            if (m.pointValid(p))
            {
                sum += glm::dvec3(w * glm::dvec4(m.position(p), 1.0));
                ++count;
            }
    }
    return count ? sum / double(count) : selectionCenter();
}

namespace
{
Ids drivingSideOf(const Node& n, const geo::Mesh& mesh, ComponentMode kind, const Ids& ids)
{
    geo::Mesh m = mesh;
    auto sym = detail::symmetryFor(n, m);
    if (!sym || ids.empty())
        return ids;
    const Ids pts = components::convert(m, kind, ids, ComponentMode::Vertex);
    // Radial: a ring round the axis is one connected selection and stays whole; separate pieces (a petal in every wedge)
    // each act in place.
    if (std::any_of(pts.begin(), pts.end(), [&](Index p) { return sym->onPlane(p); }) || (sym->isRadial() && selectionPieces(m, pts) <= 1))
        return ids;
    const int drive = driveWedge(n.id(), *sym, pts);
    Ids out;
    for (Index id : ids)
    {
        const Ids own = kind == ComponentMode::Vertex ? Ids{id} : components::convert(m, kind, {id}, ComponentMode::Vertex);
        if (!own.empty() && sym->wedge(own.front()) == drive)
            out.push_back(id);
    }
    return out.empty() ? ids : out;
}
} // namespace

Ids drivingSide(NodeId node, ComponentMode kind, const Ids& ids, MeshPtr mesh)
{
    Graph& g = detail::graph();
    const Node* n = g.node(node);
    if (!n || !n->type().isA("mesh") || kind == ComponentMode::Object)
        return ids;
    if (!mesh)
        mesh = modifiers::cage(g, node);
    const std::optional<geo::SymmetrySpec> spec = symmetrySpec(*n);
    if (!mesh || !spec)
        return ids;
    const glm::dvec3 key(spec->axis, spec->offset, spec->order());
    const int picked = components::pickedWedge(node);
    // Building the symmetry map is the cost, and gizmos ask every frame.
    struct Cached
    {
        std::weak_ptr<const geo::Mesh> mesh;
        ComponentMode kind = ComponentMode::Object;
        glm::dvec3 spec{0.0};
        int picked = -1;
        Ids ids, out;
    };
    static std::map<std::pair<NodeId, const geo::Mesh*>, Cached> cache;
    std::erase_if(cache, [](const auto& e) { return e.second.mesh.expired(); });
    Cached& c = cache[{node, mesh.get()}];
    if (c.mesh.lock() != mesh || c.kind != kind || c.spec != key || c.picked != picked || c.ids != ids)
        c = {mesh, kind, key, picked, ids, drivingSideOf(*n, *mesh, kind, ids)};
    return c.out;
}

namespace
{
std::vector<float> falloffWeights(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected)
{
    LILA_ZONE();

    std::vector<float> w(m.pointCapacity(), 0.0f);
    for (Index p : selected)
        if (p < w.size())
            w[p] = 1.0f;
    if (!PrefStore::get().value<bool>("general", "/softSelect/enabled", false) || selected.empty())
        return w;
    double radius = PrefStore::get().value<double>("general", "/softSelect/radius", 25.0);
    if (radius <= 0.0)
        return w;
    softSelect::Curve curve = softSelect::currentCurve();
    if (softSelect::surfaceFalloff())
    {
        std::vector<double> dist = surfaceDistances(m, world, selected, radius);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (w[p] < 1.0f && dist[p] < radius)
                w[p] = float(curve.evaluate(dist[p] / radius));
        return w;
    }
    std::vector<glm::dvec3> seeds;
    for (Index p : selected)
        if (m.pointValid(p))
            seeds.push_back(glm::dvec3(world * glm::dvec4(m.position(p), 1.0)));
    // Many seeds on a dense mesh: thin them out; the falloff is smooth, so a subset barely shows.
    size_t stride = std::max<size_t>(1, seeds.size() / 4096);
    double r2 = radius * radius;
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p) || w[p] >= 1.0f)
            return;
        glm::dvec3 q = glm::dvec3(world * glm::dvec4(m.position(p), 1.0));
        double best = r2;
        for (size_t k = 0; k < seeds.size(); k += stride)
            best = std::min(best, glm::dot(q - seeds[k], q - seeds[k]));
        if (best >= r2)
            return;
        w[p] = float(curve.evaluate(std::sqrt(best) / radius));
    });
    return w;
}
} // namespace

std::vector<float> softWeights(const std::vector<glm::dvec3>& points, const std::vector<char>& selected)
{
    std::vector<float> w(points.size(), 0.0f);
    std::vector<glm::dvec3> seeds;
    for (size_t i = 0; i < points.size() && i < selected.size(); ++i)
        if (selected[i])
        {
            w[i] = 1.0f;
            seeds.push_back(points[i]);
        }
    const double radius = PrefStore::get().value<double>("general", "/softSelect/radius", 25.0);
    if (!PrefStore::get().value<bool>("general", "/softSelect/enabled", false) || seeds.empty() || radius <= 0.0)
        return w;
    const softSelect::Curve curve = softSelect::currentCurve();
    for (size_t i = 0; i < points.size(); ++i)
    {
        if (w[i] >= 1.0f)
            continue;
        double best = radius * radius;
        for (const glm::dvec3& s : seeds)
            best = std::min(best, glm::dot(points[i] - s, points[i] - s));
        if (best < radius * radius)
            w[i] = float(curve.evaluate(std::sqrt(best) / radius));
    }
    return w;
}

std::vector<float> softWeights(const geo::Mesh& m, const glm::dmat4& world, const Ids& selected)
{
    std::vector<float> w = falloffWeights(m, world, selected);
    fields::modulateSoftSelection(m, world, w);
    return w;
}

std::vector<glm::dvec3> componentNormals(const geo::Mesh& m, ComponentMode kind, const Ids& ids)
{
    LILA_ZONE();

    std::vector<glm::dvec3> n(m.pointCapacity(), glm::dvec3(0.0));
    // A face selection's points take only the selected faces, so a region offsets as a whole.
    std::vector<char> fromSelection(m.pointCapacity(), 0);
    if (kind == ComponentMode::Face)
        for (Index f : ids)
            if (m.faceValid(f))
            {
                glm::dvec3 a = m.faceAreaVector(f);
                m.forEachFaceCorner(f, [&](Index c) {
                    n[m.cornerPoint(c)] += a;
                    fromSelection[m.cornerPoint(c)] = 1;
                });
            }
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            glm::dvec3 a = m.faceAreaVector(f);
            m.forEachFaceCorner(f, [&](Index c) {
                if (!fromSelection[m.cornerPoint(c)])
                    n[m.cornerPoint(c)] += a;
            });
        }
    for (glm::dvec3& v : n)
        if (double len = glm::length(v); len > 1e-12)
            v /= len;
        else
            v = glm::dvec3(0.0);
    return n;
}

namespace
{
glm::dmat3 worldFrame(const glm::dmat3& f, const glm::dmat3& linear);

/** World frames of a point provider's given components, those without one left out. */
std::vector<glm::dmat3> providedWorldFrames(Graph& g, Node& n, const components::PointProvider& pp, const Ids& ids)
{
    std::vector<glm::dmat3> out;
    if (!pp.frames)
        return out;
    glm::dmat3 linear(n.as<glm::dmat4>(Transform::kWorldMatrix));
    for (const glm::dmat3& f : pp.frames(g, n.id(), ids))
        if (glm::dmat3 w = worldFrame(f, linear); glm::length(w[2]) > 0.5)
            out.push_back(w);
    return out;
}
} // namespace

glm::dvec3 selectionNormal()
{
    Graph& g = detail::graph();
    g.evaluate();
    ComponentMode kind = components::mode();
    glm::dvec3 sum(0.0);
    if (kind == ComponentMode::Object)
        return glm::dvec3(0, 1, 0);
    for (auto& [node, ids] : components::selected(kind))
    {
        Node* n = g.node(node);
        if (const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr)
        {
            for (const glm::dmat3& f : providedWorldFrames(g, *n, *pp, ids))
                sum += f[2];
            continue;
        }
        MeshPtr cage = n && n->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
        if (!cage)
            continue;
        const geo::Mesh& m = *cage;
        glm::dmat3 toWorld = glm::inverseTranspose(glm::dmat3(n->as<glm::dmat4>(Transform::kWorldMatrix)));
        const Ids side = drivingSide(node, kind, ids, cage);
        // Runs on every manipulator refresh, so only the selection's own faces are visited.
        const bool faces = kind == ComponentMode::Face;
        const Ids points = faces ? Ids() : components::convert(m, kind, side, ComponentMode::Vertex);
        const Ids& visit = faces ? side : points;
        const glm::dvec3 local = tbb::parallel_deterministic_reduce(
            tbb::blocked_range<size_t>(0, visit.size(), 4096), glm::dvec3(0.0),
            [&](const tbb::blocked_range<size_t>& r, glm::dvec3 acc) {
                for (size_t i = r.begin(); i < r.end(); ++i)
                {
                    const Index id = visit[i];
                    if (faces)
                        acc += m.faceValid(id) ? m.faceAreaVector(id) : glm::dvec3(0.0);
                    else if (m.pointValid(id))
                    {
                        // Each face once, as pointFaces gives them, without its allocation for the usual handful.
                        std::array<Index, 32> seen;
                        size_t count = 0;
                        bool many = false;
                        m.forEachPointCorner(id, [&](Index c) {
                            const Index f = m.cornerFace(c);
                            if (std::find(seen.begin(), seen.begin() + count, f) != seen.begin() + count)
                                return;
                            if (count == seen.size())
                                many = true;
                            else
                            {
                                seen[count++] = f;
                                acc += m.faceAreaVector(f);
                            }
                        });
                        if (many)
                            for (Index f : m.pointFaces(id))
                                if (std::find(seen.begin(), seen.end(), f) == seen.end())
                                    acc += m.faceAreaVector(f);
                    }
                }
                return acc;
            },
            [](const glm::dvec3& a, const glm::dvec3& b) { return a + b; });
        sum += toWorld * local;
    }
    double len = glm::length(sum);
    return len > 1e-12 ? sum / len : glm::dvec3(0, 1, 0);
}

namespace
{
/** The world axis least aligned with the normal: the U fallback, shared by every point so a region's tangents agree. */
glm::dvec3 tangentReference(const glm::dvec3& normal)
{
    int axis = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(normal[i]) < std::abs(normal[axis]) - 1e-9)
            axis = i;
    glm::dvec3 r(0.0);
    r[axis] = 1.0;
    return r;
}

/** Direction of increasing u across face f (dP/du over its triangle fan), weighted by area; zero where its uvs are degenerate. */
glm::dvec3 faceUTangent(const geo::Mesh& m, Index f, std::span<const glm::vec2> uv)
{
    glm::dvec3 sum(0.0);
    const Index c0 = m.faceCorner(f);
    const glm::dvec3 p0 = m.position(m.cornerPoint(c0));
    const glm::dvec2 t0 = uv[c0];
    for (Index c1 = m.cornerNext(c0), c2 = m.cornerNext(c1); c2 != c0; c1 = c2, c2 = m.cornerNext(c2))
    {
        glm::dvec3 e1 = m.position(m.cornerPoint(c1)) - p0, e2 = m.position(m.cornerPoint(c2)) - p0;
        glm::dvec2 d1 = glm::dvec2(uv[c1]) - t0, d2 = glm::dvec2(uv[c2]) - t0;
        double det = d1.x * d2.y - d2.x * d1.y;
        if (std::abs(det) < 1e-14)
            continue;
        glm::dvec3 t = (e1 * d2.y - e2 * d1.y) / det;
        if (double len = glm::length(t); len > 1e-12)
            sum += t / len * glm::length(glm::cross(e1, e2));
    }
    return sum;
}

struct FrameContext
{
    const geo::Mesh& m;
    std::span<const glm::vec2> uv;
    std::vector<char> selFace, selEdge;
    glm::dvec3 reference{1, 0, 0};
};

FrameContext frameContext(const geo::Mesh& m, ComponentMode kind, const Ids& ids, const glm::dvec3& reference)
{
    FrameContext x{m, m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0))};
    if (x.uv.size() < m.cornerCapacity())
        x.uv = {};
    std::vector<char>& sel = kind == ComponentMode::Face ? x.selFace : x.selEdge;
    if (kind == ComponentMode::Face || kind == ComponentMode::Edge)
    {
        sel.assign(kind == ComponentMode::Face ? m.faceCapacity() : m.edgeCapacity(), 0);
        for (Index i : ids)
            if (i < sel.size())
                sel[i] = 1;
    }
    if (double len = glm::length(reference); len > 1e-12)
        x.reference = reference / len;
    return x;
}

glm::dmat3 pointFrame(const FrameContext& x, Index p)
{
    const geo::Mesh& m = x.m;
    bool onSelected = false;
    if (!x.selFace.empty())
        m.forEachPointCorner(p, [&](Index c) { onSelected = onSelected || x.selFace[m.cornerFace(c)]; });
    glm::dvec3 n(0.0), t(0.0);
    double tWeight = 0.0;
    m.forEachPointCorner(p, [&](Index c) {
        const Index f = m.cornerFace(c);
        if (onSelected && !x.selFace[f])
            return;
        n += m.faceAreaVector(f);
        if (!x.uv.empty())
        {
            glm::dvec3 ft = faceUTangent(m, f, x.uv);
            t += ft;
            tWeight += glm::length(ft);
        }
    });
    if (double len = glm::length(n); len > 1e-12)
        n /= len;
    else
        return glm::dmat3(0.0);
    auto inPlane = [&](const glm::dvec3& v) { return v - n * glm::dot(v, n); };
    t = inPlane(t);
    // Uv islands mirrored across the point cancel out, which counts as having no uvs.
    glm::dvec3 u = tWeight > 0.0 && glm::length(t) > 0.25 * tWeight ? t : inPlane(x.reference);
    if (glm::length(u) < 1e-3)
        u = inPlane(std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0));
    if (!x.selEdge.empty())
    {
        glm::dvec3 along(0.0);
        m.forEachPointEdge(p, [&](Index e) {
            if (!x.selEdge[e])
                return;
            glm::dvec3 d = inPlane(m.position(m.edgeOtherPoint(e, p)) - m.position(p));
            along += glm::dot(d, u) < 0.0 ? -d : d;
        });
        if (glm::length(along) > 1e-9)
            u = along;
    }
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(n, u), n);
}

/** An object-space point frame in world space, kept orthonormal under non-uniform scale; zero stays zero. */
glm::dmat3 worldFrame(const glm::dmat3& f, const glm::dmat3& linear)
{
    glm::dvec3 n = glm::inverseTranspose(linear) * f[2];
    if (glm::length(n) < 1e-12)
        return glm::dmat3(0.0);
    n = glm::normalize(n);
    glm::dvec3 u = linear * f[0];
    u -= n * glm::dot(u, n);
    if (glm::length(u) < 1e-12)
        return glm::dmat3(0.0);
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(n, u), n);
}
} // namespace

std::vector<glm::dmat3> componentFrames(const geo::Mesh& m, ComponentMode kind, const Ids& ids, const glm::dvec3& reference)
{
    LILA_ZONE();

    const FrameContext x = frameContext(m, kind, ids, reference);
    std::vector<glm::dmat3> frames(m.pointCapacity(), glm::dmat3(0.0));
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (m.pointValid(p))
            frames[p] = pointFrame(x, p);
    });
    return frames;
}

namespace
{
/** worldFrame(f, linear)[0], with linear's inverse transpose worked out once by the caller. */
glm::dvec3 worldTangent(const glm::dmat3& f, const glm::dmat3& linear, const glm::dmat3& normals)
{
    glm::dvec3 n = normals * f[2];
    if (glm::length(n) < 1e-12)
        return glm::dvec3(0.0);
    n = glm::normalize(n);
    glm::dvec3 u = linear * f[0];
    u -= n * glm::dot(u, n);
    const double len = glm::length(u);
    return len < 1e-12 ? glm::dvec3(0.0) : u / len;
}
} // namespace

glm::dmat3 selectionNormalFrame()
{
    LILA_ZONE();

    const glm::dvec3 n = selectionNormal();
    const glm::dvec3 ref = tangentReference(n);
    Graph& g = detail::graph();
    ComponentMode kind = components::mode();
    glm::dvec3 sum(0.0);
    if (kind != ComponentMode::Object)
        for (auto& [node, ids] : components::selected(kind))
        {
            Node* nd = g.node(node);
            if (const components::PointProvider* pp = nd ? components::pointProvider(*nd) : nullptr)
            {
                for (const glm::dmat3& f : providedWorldFrames(g, *nd, *pp, ids))
                    sum += f[0];
                continue;
            }
            MeshPtr cage = nd && nd->type().isA("mesh") ? modifiers::cage(g, node) : nullptr;
            if (!cage)
                continue;
            const geo::Mesh& m = *cage;
            glm::dmat3 linear(nd->as<glm::dmat4>(Transform::kWorldMatrix));
            // Only the selection's own points, since this runs on every manipulator refresh.
            const Ids side = drivingSide(node, kind, ids, cage);
            const FrameContext x = frameContext(m, kind, side, glm::inverse(linear) * ref);
            const Ids points = components::convert(m, kind, side, ComponentMode::Vertex);
            const glm::dmat3 normals = glm::inverseTranspose(linear);
            // Deterministic, so the same selection always gets the same frame.
            sum += tbb::parallel_deterministic_reduce(
                tbb::blocked_range<size_t>(0, points.size(), 4096), glm::dvec3(0.0),
                [&](const tbb::blocked_range<size_t>& r, glm::dvec3 acc) {
                    for (size_t i = r.begin(); i < r.end(); ++i)
                        if (const Index p = points[i]; m.pointValid(p))
                            acc += worldTangent(pointFrame(x, p), linear, normals);
                    return acc;
                },
                [](const glm::dvec3& a, const glm::dvec3& b) { return a + b; });
        }
    glm::dvec3 u = sum - n * glm::dot(sum, n);
    if (glm::length(u) < 1e-6)
        u = ref - n * glm::dot(ref, n);
    u = glm::normalize(u);
    return glm::dmat3(u, glm::cross(n, u), n);
}

namespace
{
/** Sets up a point provider node's targets by provider id, for edits that need more than a loose point mesh; empty skips the node. */
using ProvidedSetup = std::function<PointTarget(Graph&, Node&, const components::PointProvider&, const Ids&)>;

/**
 * deformSelectedComponents for a point provider's node: its selected points moved to their targets, from provided when set,
 * else from setup given them as a loose point mesh.
 */
bool deformProvidedPoints(Graph& g, Node& n, const components::PointProvider& pp, const Ids& ids, const PointTargetSetup& setup,
                          const ProvidedSetup& provided)
{
    std::map<uint32_t, glm::dvec3> all;
    for (auto& [id, p] : pp.points(g, n.id()))
        all[id] = p;
    geo::Mesh m;
    Ids local;
    std::vector<uint32_t> source;
    for (uint32_t id : ids)
        if (auto it = all.find(id); it != all.end())
        {
            local.push_back(m.addPoint(it->second));
            source.push_back(id);
        }
    if (source.empty())
        return false;
    glm::dmat4 world = n.as<glm::dmat4>(Transform::kWorldMatrix);
    // Soft selection pulls unselected points part of the way. They join the point mesh before setup sees it, but not the
    // selection; provided edits key their targets by id and only know the selection, so they stay hard.
    struct SoftPoint
    {
        uint32_t id;
        Index local;
        float weight;
    };
    std::vector<SoftPoint> soft;
    if (!provided && PrefStore::get().value<bool>("general", "/softSelect/enabled", false))
    {
        std::vector<uint32_t> order;
        std::vector<glm::dvec3> worldPts;
        std::vector<char> picked;
        const std::set<uint32_t> chosen(source.begin(), source.end());
        for (auto& [id, p] : all)
            if (chosen.contains(id) || !pp.soft || pp.soft(id))
            {
                order.push_back(id);
                worldPts.push_back(glm::dvec3(world * glm::dvec4(p, 1.0)));
                picked.push_back(chosen.contains(id) ? 1 : 0);
            }
        const std::vector<float> w = softWeights(worldPts, picked);
        for (size_t k = 0; k < order.size(); ++k)
            if (!picked[k] && w[k] > 0.0f)
                soft.push_back({order[k], m.addPoint(all[order[k]]), w[k]});
    }
    PointTarget targetOf = provided ? provided(g, n, pp, source) : setup(m, world, ComponentMode::Vertex, local);
    if (!targetOf)
        return false;
    std::vector<std::pair<uint32_t, glm::dvec3>> moves;
    for (size_t i = 0; i < source.size(); ++i)
        moves.push_back({source[i], targetOf(provided ? source[i] : local[i], m.position(local[i]))});
    for (const SoftPoint& sp : soft)
    {
        const glm::dvec3 at = m.position(sp.local);
        moves.push_back({sp.id, glm::mix(at, targetOf(sp.local, at), double(sp.weight))});
    }
    pp.setPoints(g, n.id(), moves);
    return true;
}

int deformSelected(const char* label, const PointTargetSetup& setup, const ProvidedSetup& provided, bool allowWhole = false)
{
    LILA_ZONE();

    ComponentMode kind = components::mode();
    Graph& g = detail::graph();
    std::map<NodeId, Ids> selection;
    const bool whole = kind == ComponentMode::Object && allowWhole;
    if (whole)
    {
        kind = ComponentMode::Vertex;
        for (NodeId id : g.selection())
            if (Node* n = g.node(id); n && n->type().isA("mesh"))
                selection[id] = {};
    }
    else if (kind == ComponentMode::Object)
        throw CommandError(std::string(label) + ": no components selected");
    else
        selection = components::selected(kind);
    UndoChunk chunk(label);
    int edited = 0;
    for (auto& [node, ids] : selection)
    {
        Node* n = g.node(node);
        if (const components::PointProvider* pp = n ? components::pointProvider(*n) : nullptr; pp && kind == ComponentMode::Vertex)
        {
            edited += deformProvidedPoints(g, *n, *pp, ids, setup, provided) ? 1 : 0;
            continue;
        }
        if (!n || !n->type().isA("mesh"))
            continue;
        PlugRef base = detail::basePlug(*n);
        geo::Mesh m = detail::baseMesh(base);
        if (whole)
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p))
                    ids.push_back(p);
        Ids pts = components::convert(m, kind, ids, ComponentMode::Vertex);
        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        PointTarget targetOf = setup(m, world, kind, ids);
        if (!targetOf)
            continue;
        auto sym = detail::symmetryFor(*n, m);

        // With symmetry, the wedge (side) the selection is mostly in drives and the others follow as its exact images.
        int drive = 0;
        if (sym)
        {
            drive = driveWedge(node, *sym, pts);
            pts = sym->expandPoints(pts);
        }
        std::vector<float> weight = softWeights(m, world, pts);
        std::vector<glm::dvec3> before(m.positions().begin(), m.positions().end());
        Ids moved;
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p) || weight[p] <= 0.0f)
                continue;
            moved.push_back(p);
            if (sym && !sym->drives(p, drive))
                continue;
            m.setPosition(p, glm::mix(before[p], targetOf(p, before[p]), double(weight[p])));
        }
        if (sym)
            sym->enforceWedge(m, moved, drive);
        g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        ++edited;
    }
    return edited;
}

/** Provided points moved by uvn along their own world frames; points without a frame stay put. */
ProvidedSetup alongProvidedFrames(const glm::dvec3& uvn)
{
    return [uvn](Graph& g, Node& n, const components::PointProvider& pp, const Ids& ids) -> PointTarget {
        if (!pp.frames)
            return {};
        glm::dmat3 linear(n.as<glm::dmat4>(Transform::kWorldMatrix)), toLocal = glm::inverse(linear);
        std::vector<glm::dmat3> frames = pp.frames(g, n.id(), ids);
        auto offsets = std::make_shared<std::map<uint32_t, glm::dvec3>>();
        for (size_t i = 0; i < ids.size() && i < frames.size(); ++i)
            (*offsets)[ids[i]] = toLocal * (worldFrame(frames[i], linear) * uvn);
        return [offsets](uint32_t id, const glm::dvec3& at) {
            auto it = offsets->find(id);
            return it == offsets->end() ? at : at + it->second;
        };
    };
}
} // namespace

int deformSelectedComponents(const char* label, const PointTargetSetup& setup, bool allowWhole)
{
    return deformSelected(label, setup, nullptr, allowWhole);
}

void transformSelectedComponents(const char* label, const glm::dmat4& worldXform)
{
    LILA_ZONE();

    deformSelectedComponents(label, [&](const geo::Mesh&, const glm::dmat4& world, ComponentMode, const Ids&) -> PointTarget {
        glm::dmat4 local = glm::inverse(world) * worldXform * world;
        return [local](Index, const glm::dvec3& p) { return glm::dvec3(local * glm::dvec4(p, 1.0)); };
    });
}

void transformSelectedComponentsOnto(const char* label, const glm::dmat4& worldXform, const SurfaceProjector& onto)
{
    LILA_ZONE();

    deformSelectedComponents(label, [&](const geo::Mesh&, const glm::dmat4& world, ComponentMode, const Ids&) -> PointTarget {
        glm::dmat4 local = glm::inverse(world) * worldXform * world, toLocal = glm::inverse(world);
        return [local, toLocal, world, &onto](Index, const glm::dvec3& p) {
            glm::dvec3 moved(world * local * glm::dvec4(p, 1.0));
            return glm::dvec3(toLocal * glm::dvec4(onto(moved), 1.0));
        };
    });
}

SurfaceProjector closestOnMeshes(const std::vector<NodeId>& meshes)
{
    LILA_ZONE();

    struct Surface
    {
        std::shared_ptr<geo::MeshBvh> bvh;
        glm::dmat4 world, toLocal;
    };
    Graph& g = detail::graph();
    g.evaluate();
    auto surfaces = std::make_shared<std::vector<Surface>>();
    for (NodeId id : meshes)
    {
        Node* n = g.node(id);
        MeshPtr mesh = n && n->type().isA("mesh") ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
        if (!mesh || mesh->numFaces() == 0)
            continue;
        glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        surfaces->push_back({std::make_shared<geo::MeshBvh>(*mesh), world, glm::inverse(world)});
    }
    return [surfaces](const glm::dvec3& p) {
        glm::dvec3 best = p;
        double bestDist = std::numeric_limits<double>::max();
        for (const Surface& s : *surfaces)
        {
            geo::BvhClosest c;
            if (!s.bvh->closestPoint(glm::dvec3(s.toLocal * glm::dvec4(p, 1.0)), c))
                continue;
            glm::dvec3 w(s.world * glm::dvec4(c.point, 1.0));
            if (double d = glm::length(w - p); d < bestDist)
            {
                bestDist = d;
                best = w;
            }
        }
        return best;
    };
}

void offsetSelectedComponents(const char* label, double distance)
{
    LILA_ZONE();

    deformSelected(label, [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, const Ids& ids) -> PointTarget {
        auto normals = std::make_shared<std::vector<glm::dvec3>>(componentNormals(m, kind, ids));
        glm::dmat3 toWorld = glm::inverseTranspose(glm::dmat3(world)), toLocal = glm::inverse(glm::dmat3(world));
        // The distance is world space, so a scaled object still moves by it.
        return [normals, toWorld, toLocal, distance](Index p, const glm::dvec3& at) {
            glm::dvec3 nw = toWorld * (*normals)[p];
            double len = glm::length(nw);
            return len > 1e-12 ? at + toLocal * (nw / len * distance) : at;
        };
    }, alongProvidedFrames(glm::dvec3(0.0, 0.0, distance)));
}

void offsetSelectedComponents(const char* label, const glm::dvec3& uvn)
{
    LILA_ZONE();

    if (uvn.x == 0.0 && uvn.y == 0.0)
    {
        offsetSelectedComponents(label, uvn.z);
        return;
    }
    const glm::dvec3 ref = tangentReference(selectionNormal());
    deformSelected(label, [&](const geo::Mesh& m, const glm::dmat4& world, ComponentMode kind, const Ids& ids) -> PointTarget {
        glm::dmat3 linear(world), toLocal = glm::inverse(linear);
        auto frames = std::make_shared<std::vector<glm::dmat3>>(componentFrames(m, kind, ids, toLocal * ref));
        return [frames, linear, toLocal, uvn](Index p, const glm::dvec3& at) { return at + toLocal * (worldFrame((*frames)[p], linear) * uvn); };
    }, alongProvidedFrames(uvn));
}
} // namespace lila::scene

namespace lila::scene::detail
{
void registerMeshEditTransformCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "moveComponents",
        .doc = "Move the selected components by an offset in world space, or with normal= push each point along its own vertex normal "
               "(a face selection uses only the selected faces' normals, so the region inflates as a whole). uvn= also slides each point "
               "along its own surface tangents: U follows the uv0 u direction where the mesh has uvs (in edge mode, the selected edges), "
               "else the world axis least aligned with the selection's normal, and V completes the frame. Curve points use the curve's frame: "
               "U its tangent (a handle's own direction), N across it within its plane, V out of the plane.",
        .flags = {{"by", "", T::Vec3, nullptr, "Offset, cm."},
                  {"normal", "", T::Float, nullptr, "Distance along the vertex normals, cm."},
                  {"uvn", "", T::Vec3, nullptr, "Distances along each point's own U tangent, V tangent and normal, cm."},
                  nodeFlag({"onto", "", T::List, nullptr, "Meshes whose surface each moved point then lands on, at its closest point."}, "mesh")},
        .run = [](const Args& a) -> json {
            if (int(a.has("by")) + int(a.has("normal")) + int(a.has("uvn")) != 1)
                throw CommandError("moveComponents: give one of by=, normal= or uvn=");
            if (a.has("normal"))
            {
                offsetSelectedComponents("Move Components", a.get<double>("normal"));
                return nullptr;
            }
            if (a.has("uvn"))
            {
                offsetSelectedComponents("Move Components", vec(a.flags["uvn"]));
                return nullptr;
            }
            glm::dmat4 by = glm::translate(glm::dmat4(1.0), vec(a.flags["by"]));
            if (a.has("onto"))
            {
                std::vector<NodeId> onto;
                for (const json& name : a.flags["onto"])
                    onto.push_back(meshArg(name.get<std::string>(), "moveComponents").id());
                transformSelectedComponentsOnto("Move Components", by, closestOnMeshes(onto));
                return nullptr;
            }
            transformSelectedComponents("Move Components", by);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "rotateComponents",
        .doc = "Rotate the selected components about a world pivot (default: their centre; with symmetry, a pair clear of the mirror plane turns about each side's own centre). Degrees, XYZ order.",
        .flags = {{"by", "", T::Vec3, nullptr, "Rotation, degrees."}, {"pivot", "", T::Vec3, nullptr, "World pivot."}},
        .run = [](const Args& a) -> json {
            if (!a.has("by"))
                throw CommandError("rotateComponents: give by=");
            glm::dvec3 pivot = a.has("pivot") ? vec(a.flags["pivot"]) : symmetricSelectionCenter();
            glm::dmat4 r = eulerToMatrix(vec(a.flags["by"]), RotateOrder::XYZ);
            transformSelectedComponents("Rotate Components",
                                        glm::translate(glm::dmat4(1.0), pivot) * r * glm::translate(glm::dmat4(1.0), -pivot));
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "scaleComponents",
        .doc = "Scale the selected components about a world pivot (default: their centre; with symmetry, a pair clear of the mirror plane scales about each side's own centre).",
        .flags = {{"by", "", T::Vec3, nullptr, "Factor per axis: the world axes, or the axes orient= turns them to."},
                  {"pivot", "", T::Vec3, nullptr, "World pivot."},
                  {"orient", "", T::Vec3, nullptr, "Rotation of the scale axes from the world axes, degrees, XYZ order."}},
        .run = [](const Args& a) -> json {
            if (!a.has("by"))
                throw CommandError("scaleComponents: give by=");
            glm::dvec3 pivot = a.has("pivot") ? vec(a.flags["pivot"]) : symmetricSelectionCenter();
            const glm::dmat4 r = a.has("orient") ? eulerToMatrix(vec(a.flags["orient"]), RotateOrder::XYZ) : glm::dmat4(1.0);
            transformSelectedComponents("Scale Components", glm::translate(glm::dmat4(1.0), pivot) * r * glm::scale(glm::dmat4(1.0), vec(a.flags["by"])) *
                                                                glm::transpose(r) * glm::translate(glm::dmat4(1.0), -pivot));
            return nullptr;
        },
    });
}
} // namespace lila::scene::detail
