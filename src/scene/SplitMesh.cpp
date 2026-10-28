#include "scene/SplitMesh.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "geo/CrossSection.h"
#include "geo/Ops.h"
#include "geo/Reduce.h"
#include "geo/Symmetry.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "retopo/Retopo.h"
#include "scene/DominantInfluence.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Rig.h"
#include "scene/SelectAs.h"
#include "scene/Skin.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <charconv>
#include <utility>
#include <climits>
#include <cmath>
#include <format>
#include <map>
#include <numeric>

namespace lila::scene::split
{
namespace
{
using geo::Index;
using geo::kInvalid;
using T = ArgType;

// Scratch layers: the source face each face came from, and the edges a plane cut along.
constexpr const char* kSourceAttr = "lilaSplitSource";
constexpr const char* kCutAttr = "lilaSplitCut";

glm::dvec3 unitOr(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double len = glm::length(v);
    return len > 1e-12 ? v / len : fallback;
}

void planeBasis(const glm::dvec3& n, glm::dvec3& u, glm::dvec3& v)
{
    u = glm::normalize(glm::cross(n, std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0)));
    v = glm::cross(n, u);
}

/** Whether origin lies inside the loop, both taken into the plane. */
bool loopContains(const std::vector<glm::dvec3>& loop, const glm::dvec3& origin, const glm::dvec3& n)
{
    glm::dvec3 u, v;
    planeBasis(n, u, v);
    bool inside = false;
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
    {
        const double xi = glm::dot(loop[i] - origin, u), yi = glm::dot(loop[i] - origin, v);
        const double xj = glm::dot(loop[j] - origin, u), yj = glm::dot(loop[j] - origin, v);
        if ((yi > 0.0) != (yj > 0.0) && 0.0 < (xj - xi) * (0.0 - yi) / (yj - yi) + xi)
            inside = !inside;
    }
    return inside;
}

/** Cuts the faces crossing the plane inside its disc along it, and marks the edges on the cut. */
void cutDisc(geo::Mesh& m, const Plane& plane, double radius, double tol)
{
    LILA_ZONE();

    const glm::dvec3 n = unitOr(plane.normal, {0, 1, 0});
    const glm::dvec3 o = plane.origin;
    auto dist = [&](const glm::dvec3& x) { return glm::dot(n, x - o); };
    auto inDisc = [&](const glm::dvec3& x) {
        glm::dvec3 d = x - o;
        d -= glm::dot(d, n) * n;
        return glm::dot(d, d) <= radius * radius;
    };
    auto onPlane = [&](Index p) {
        const glm::dvec3& x = m.position(p);
        return std::abs(dist(x)) <= tol && inDisc(x);
    };
    auto side = [&](Index p) { return onPlane(p) ? 0 : dist(m.position(p)) >= 0.0 ? 1 : -1; };

    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && onPlane(p))
            m.setPosition(p, m.position(p) - dist(m.position(p)) * n);

    const Index edgeCap = m.edgeCapacity();
    for (Index e = 0; e < edgeCap; ++e)
    {
        if (!m.edgeValid(e))
            continue;
        const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        const double da = dist(a), db = dist(b);
        if (!((da > tol && db < -tol) || (da < -tol && db > tol)))
            continue;
        const double t = da / (da - db);
        const glm::dvec3 x = a + (b - a) * t;
        if (!inDisc(x))
            continue;
        const Index mid = m.splitEdge(e, t);
        m.setPosition(mid, x - dist(x) * n);
    }

    // As cutAtPlane: each sign change round a face now passes through an on-plane point.
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        Index face = f;
        while (m.faceValid(face))
        {
            bool pos = false, neg = false;
            Index start = kInvalid;
            m.forEachFaceCorner(face, [&](Index c) {
                const int s = side(m.cornerPoint(c));
                pos |= s > 0;
                neg |= s < 0;
                if (start == kInvalid && s == 0 && side(m.cornerNextPoint(c)) > 0)
                    start = c;
            });
            if (!pos || !neg || start == kInvalid)
                break;
            Index end = m.cornerNext(start);
            while (side(m.cornerPoint(end)) != 0)
                end = m.cornerNext(end);
            if (end == start || m.cornerNext(start) == end || m.cornerPrev(start) == end)
                break;
            const Index rest = m.splitFace(face, start, end);
            if (rest == kInvalid)
                break;
            face = rest;
        }
    }

    std::span<int32_t> cut = m.edgeAttrs().get<int32_t>(kCutAttr);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && onPlane(m.edgePoint(e, 0)) && onPlane(m.edgePoint(e, 1)))
            cut[e] = 1;
}

/** Component ids after merging every component no cut touches into the nearest one a cut does. */
void mergeUntouched(const geo::Mesh& m, std::vector<Index>& comp, Index count, std::span<const uint8_t> blocked)
{
    std::vector<uint8_t> touched(count, 0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (e < blocked.size() && blocked[e] && m.edgeValid(e))
        {
            const Index c0 = m.edgeCorner(e);
            if (c0 == kInvalid)
                continue;
            Index c = c0;
            do
            {
                touched[comp[m.cornerFace(c)]] = 1;
                c = m.cornerRadial(c);
            } while (c != c0);
        }
    if (std::ranges::none_of(touched, [](uint8_t t) { return t != 0; }))
        return;
    std::vector<glm::dvec3> centroid(count, glm::dvec3(0.0));
    std::vector<double> weight(count, 0.0);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && !touched[comp[f]])
        {
            const double a = std::max(m.faceArea(f), 1e-12);
            centroid[comp[f]] += m.faceCentroid(f) * a;
            weight[comp[f]] += a;
        }
    std::vector<Index> into(count);
    std::iota(into.begin(), into.end(), Index(0));
    for (Index c = 0; c < count; ++c)
    {
        if (touched[c] || weight[c] <= 0.0)
            continue;
        const glm::dvec3 at = centroid[c] / weight[c];
        double best = 1e300;
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && touched[comp[f]])
                if (const double d = glm::length(m.faceCentroid(f) - at); d < best)
                {
                    best = d;
                    into[c] = comp[f];
                }
    }
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (comp[f] != kInvalid)
            comp[f] = into[comp[f]];
}

/** Smallest-eigenvalue direction of a symmetric 3x3 (Jacobi sweeps). */
glm::dvec3 leastAxis(glm::dmat3 a)
{
    glm::dmat3 v(1.0);
    for (int sweep = 0; sweep < 32; ++sweep)
    {
        double off = std::abs(a[0][1]) + std::abs(a[0][2]) + std::abs(a[1][2]);
        if (off < 1e-14)
            break;
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q)
            {
                if (std::abs(a[p][q]) < 1e-18)
                    continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                glm::dmat3 r(1.0);
                r[p][p] = c;
                r[q][q] = c;
                r[q][p] = s;
                r[p][q] = -s;
                a = glm::transpose(r) * a * r;
                v = v * r;
            }
    }
    int k = 0;
    for (int i = 1; i < 3; ++i)
        if (a[i][i] < a[k][k])
            k = i;
    return glm::normalize(v[k]);
}

NodeId firstChildJoint(const Graph& g, NodeId joint)
{
    for (NodeId c : g.children(joint))
        if (const Node* n = g.node(c); n && rig::isJoint(*n))
            return c;
    return kNoNode;
}

glm::dvec3 worldPos(const Graph& g, NodeId id) { return glm::dvec3(g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix)[3]); }

bool isAncestor(const Graph& g, NodeId above, NodeId id)
{
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        if (p == above)
            return true;
    return false;
}

glm::dvec3 vec3Arg(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string("splitMesh: ") + what + " must be [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

geo::Mesh transformed(const geo::Mesh& m, const glm::dmat4& xf)
{
    geo::Mesh out = m;
    for (Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, glm::dvec3(xf * glm::dvec4(out.position(p), 1.0)));
    return out;
}
} // namespace

double fitRadius(const geo::Mesh& mesh, const Plane& plane)
{
    LILA_ZONE();

    const glm::dvec3 n = unitOr(plane.normal, {0, 1, 0});
    const geo::SectionMesh sm{&mesh, glm::dmat4(1.0)};
    const std::vector<geo::SectionContour> loops = geo::crossSections(std::span(&sm, 1), plane.origin, n);
    const geo::SectionContour* pick = nullptr;
    for (const geo::SectionContour& c : loops)
        if (c.points.size() >= 3 && loopContains(c.points, plane.origin, n) && (!pick || c.area < pick->area))
            pick = &c;
    if (!pick)
    {
        double best = 1e300;
        for (const geo::SectionContour& c : loops)
            for (const glm::dvec3& p : c.points)
                if (const double d = glm::length(p - plane.origin); d < best)
                {
                    best = d;
                    pick = &c;
                }
    }
    if (!pick)
        return 0.0;
    double r = 0.0;
    for (const glm::dvec3& p : pick->points)
        r = std::max(r, glm::length(p - plane.origin));
    return r * 1.05 + 1e-4;
}

Result splitByPlanes(const geo::Mesh& mesh, std::span<const Plane> planes, const Options& opts)
{
    LILA_ZONE();

    Result out;
    geo::Mesh m = mesh;
    {
        std::span<int32_t> src = m.faceAttrs().getOrAdd<int32_t>(kSourceAttr);
        for (Index f = 0; f < m.faceCapacity(); ++f)
            src[f] = int32_t(f);
        m.edgeAttrs().getOrAdd<int32_t>(kCutAttr);
    }
    for (const Plane& p : planes)
    {
        const double r = p.radius > 0.0 ? p.radius : fitRadius(mesh, p);
        out.radii.push_back(r);
        if (r <= 0.0)
        {
            out.warnings.push_back(std::format("a plane at ({:.3g}, {:.3g}, {:.3g}) misses the mesh", p.origin.x, p.origin.y, p.origin.z));
            continue;
        }
        cutDisc(m, p, r, opts.tolerance);
    }

    std::vector<uint8_t> blocked(m.edgeCapacity(), 0);
    if (std::span<const int32_t> cut = m.edgeAttrs().get<int32_t>(kCutAttr); !cut.empty())
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            blocked[e] = m.edgeValid(e) && cut[e] != 0;
    std::vector<Index> comp;
    const Index count = geo::faceComponents(m, comp, blocked);
    if (opts.mergeLoose)
        mergeUntouched(m, comp, count, blocked);

    std::span<const int32_t> src = m.faceAttrs().get<int32_t>(kSourceAttr);
    std::map<Index, std::vector<Index>> groups;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (comp[f] != kInvalid)
            groups[comp[f]].push_back(f);
    std::vector<std::vector<Index>> ordered;
    for (auto& [c, faces] : groups)
        ordered.push_back(std::move(faces));
    auto lowest = [&](const std::vector<Index>& faces) {
        int32_t lo = INT32_MAX;
        for (Index f : faces)
            lo = std::min(lo, src[f]);
        return lo;
    };
    std::ranges::sort(ordered, [&](const auto& a, const auto& b) { return lowest(a) < lowest(b); });

    for (const std::vector<Index>& faces : ordered)
    {
        geo::Mesh piece = geo::extractFaces(m, faces);
        if (opts.cap)
        {
            std::vector<Index> holes;
            std::span<const int32_t> cut = piece.edgeAttrs().get<int32_t>(kCutAttr);
            for (Index e = 0; e < piece.edgeCapacity(); ++e)
                if (piece.edgeValid(e) && cut[e] && piece.isBoundaryEdge(e))
                    holes.push_back(e);
            if (!holes.empty())
                geo::fillHoles(piece, holes);
        }
        std::vector<uint32_t> ids;
        std::span<const int32_t> pieceSrc = piece.faceAttrs().get<int32_t>(kSourceAttr);
        for (Index f = 0; f < piece.faceCapacity(); ++f)
            if (piece.faceValid(f))
                ids.push_back(uint32_t(pieceSrc[f]));
        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        piece.faceAttrs().remove(kSourceAttr);
        piece.edgeAttrs().remove(kCutAttr);
        piece.compact();
        out.pieces.push_back(std::move(piece));
        out.sourceFaces.push_back(std::move(ids));
    }
    return out;
}

double enclosedVolume(const geo::Mesh& mesh)
{
    double v = 0.0;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<Index> pts = mesh.facePoints(f);
        const glm::dvec3 a = mesh.position(pts[0]);
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            v += glm::dot(a, glm::cross(mesh.position(pts[i]), mesh.position(pts[i + 1])));
    }
    return v / 6.0;
}

const char* methodName(Remesh::Method m)
{
    switch (m)
    {
    case Remesh::Method::Quad: return "quadRemesh";
    case Remesh::Method::Voxel: return "voxelRemesh";
    case Remesh::Method::Decimate: return "decimate";
    default: return "none";
    }
}

Remesh::Method parseMethod(const std::string& name)
{
    for (Remesh::Method m : {Remesh::Method::None, Remesh::Method::Quad, Remesh::Method::Voxel, Remesh::Method::Decimate})
        if (name == methodName(m))
            return m;
    throw std::invalid_argument("remesh must be none, quadRemesh, voxelRemesh or decimate, not '" + name + "'");
}

Remesh remeshFromJson(const json& j, Remesh base)
{
    if (!j.is_object())
        return base;
    if (j.contains("remesh") && j["remesh"].is_string())
        base.method = parseMethod(j["remesh"].get<std::string>());
    if (j.contains("targetFaces") && j["targetFaces"].is_number())
        base.targetFaces = std::max(8, j["targetFaces"].get<int>());
    if (j.contains("resolution") && j["resolution"].is_number())
        base.resolution = std::clamp(j["resolution"].get<int>(), 4, 512);
    if (j.contains("ratio") && j["ratio"].is_number())
        base.ratio = std::clamp(j["ratio"].get<double>(), 0.001, 1.0);
    return base;
}

geo::Mesh remeshPiece(const geo::Mesh& piece, const Remesh& r)
{
    LILA_ZONE();

    switch (r.method)
    {
    case Remesh::Method::Quad:
    {
        retopo::QuadRemeshOptions o;
        o.targetFaces = r.targetFaces;
        retopo::QuadRemeshResult res = retopo::quadRemesh(piece, o);
        if (res.ok() && res.mesh.numFaces() > 0)
            return std::move(res.mesh);
        log::warn("splitMesh: quad remesh failed on a piece ({}); kept it as it was", res.error);
        return piece;
    }
    case Remesh::Method::Voxel:
    {
        retopo::VoxelOptions o;
        o.resolution = r.resolution;
        geo::Mesh v = retopo::voxelRemesh(piece, o);
        return v.numFaces() > 0 ? std::move(v) : piece;
    }
    case Remesh::Method::Decimate:
    {
        geo::ReduceOptions o;
        o.ratio = r.ratio;
        o.targetTriangles = geo::Index(std::max(0, r.targetTriangles));
        geo::ReduceResult res = geo::reduceMesh(piece, o);
        return res.mesh.numFaces() > 0 ? std::move(res.mesh) : piece;
    }
    default: return piece;
    }
}

void applyPieceOverrides(std::vector<Remesh>& pieces, const std::string& pieceFaces, const std::string& exactPieces)
{
    auto tokens = [](const std::string& text) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : text + " ")
            if (c == ' ' || c == ',' || c == ';' || c == '	')
            {
                if (!cur.empty())
                    out.push_back(std::exchange(cur, {}));
            }
            else
                cur.push_back(c);
        return out;
    };
    auto index = [&](const std::string& t) -> int {
        int v = -1;
        std::from_chars(t.data(), t.data() + t.size(), v);
        return v >= 0 && size_t(v) < pieces.size() ? v : -1;
    };
    for (const std::string& t : tokens(pieceFaces))
    {
        const size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        const int i = index(t.substr(0, colon));
        int faces = 0;
        const std::string n = t.substr(colon + 1);
        std::from_chars(n.data(), n.data() + n.size(), faces);
        if (i < 0 || faces < 4)
            continue;
        pieces[size_t(i)].targetFaces = faces;
        pieces[size_t(i)].targetTriangles = faces;
    }
    for (const std::string& t : tokens(exactPieces))
        if (const int i = index(t); i >= 0)
            pieces[size_t(i)].method = Remesh::Method::None;
}

Plane symmetryPlane(Graph& g, NodeId meshId)
{
    g.evaluate();
    const Node* n = g.node(meshId);
    if (!n)
        return {};
    std::optional<geo::SymmetrySpec> spec = symmetrySpec(*n);
    if (!spec || spec->isRadial())
        spec = geo::SymmetrySpec{.axis = 0, .offset = symmetryOffset(*n, 0)};
    glm::dvec3 axis(0.0);
    axis[spec->axis] = 1.0;
    const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
    const glm::dvec3 origin(w * glm::dvec4(axis * spec->offset, 1.0));
    const glm::dvec3 normal = unitOr(glm::transpose(glm::inverse(glm::dmat3(w))) * axis, {1, 0, 0});
    return {origin, normal, 0.0};
}

Plane reflect(const Plane& p, const Plane& sym)
{
    const glm::dvec3 n = unitOr(sym.normal, {1, 0, 0});
    Plane out = p;
    out.origin = p.origin - 2.0 * glm::dot(p.origin - sym.origin, n) * n;
    out.normal = p.normal - 2.0 * glm::dot(p.normal, n) * n;
    return out;
}

std::vector<Plane> suggestPlanes(Graph& g, NodeId meshId, int minFaces)
{
    LILA_ZONE();

    std::vector<Plane> out;
    const NodeId skinId = skin::find(g, meshId);
    if (skinId == kNoNode)
        return out;
    g.evaluate();
    const Node* meshNode = g.node(meshId);
    const MeshPtr& mp = meshNode->as<MeshPtr>(MeshNode::kOutMesh);
    std::shared_ptr<const rig::DominantInfluenceMap> map = rig::dominantInfluences(g, skinId);
    if (!mp || !map)
        return out;
    const geo::Mesh& m = *mp;
    const glm::dmat4 w = meshNode->as<glm::dmat4>(Transform::kWorldMatrix);
    std::unordered_map<int64_t, NodeId> jointOf;
    for (const skin::Influence& inf : skin::influences(g, skinId))
        if (inf.joint != kNoNode)
            jointOf[int64_t(inf.index)] = rig::selectAsTarget(g, inf.joint);

    std::vector<NodeId> faceJoint(m.faceCapacity(), kNoNode);
    std::map<NodeId, int> faceCount;
    std::map<NodeId, glm::dvec3> regionSum;
    for (Index f = 0; f < m.faceCapacity() && f < map->faceInfluence.size(); ++f)
        if (m.faceValid(f))
            if (auto it = jointOf.find(map->faceInfluence[f]); it != jointOf.end())
            {
                faceJoint[f] = it->second;
                ++faceCount[it->second];
                regionSum[it->second] += glm::dvec3(w * glm::dvec4(m.faceCentroid(f), 1.0));
            }

    std::map<std::pair<NodeId, NodeId>, std::vector<glm::dvec3>> borders;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        const Index c = m.edgeCorner(e);
        const NodeId a = faceJoint[m.cornerFace(c)], b = faceJoint[m.cornerFace(m.cornerRadial(c))];
        if (a == b || a == kNoNode || b == kNoNode || faceCount[a] < minFaces || faceCount[b] < minFaces)
            continue;
        const glm::dvec3 mid = (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5;
        borders[{std::min(a, b), std::max(a, b)}].push_back(glm::dvec3(w * glm::dvec4(mid, 1.0)));
    }

    for (auto& [pair, pts] : borders)
    {
        if (pts.size() < 3)
            continue;
        auto [a, b] = pair;
        // The plane faces the child's side: b when it hangs below a, else whichever is lower in the hierarchy.
        if (isAncestor(g, b, a))
            std::swap(a, b);
        glm::dvec3 origin(0.0);
        for (const glm::dvec3& p : pts)
            origin += p;
        origin /= double(pts.size());
        glm::dmat3 cov(0.0);
        for (const glm::dvec3& p : pts)
        {
            const glm::dvec3 d = p - origin;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    cov[i][j] += d[i] * d[j];
        }
        const glm::dvec3 toChild = regionSum[b] / double(faceCount[b]) - regionSum[a] / double(faceCount[a]);
        glm::dvec3 n = leastAxis(cov);
        if (!std::isfinite(n.x) || glm::length(n) < 0.5)
            n = unitOr(toChild, {0, 1, 0});
        if (glm::dot(n, toChild) < 0.0)
            n = -n;
        double r = 0.0;
        for (const glm::dvec3& p : pts)
        {
            glm::dvec3 d = p - origin;
            d -= glm::dot(d, n) * n;
            r = std::max(r, glm::length(d));
        }
        out.push_back({origin, n, r * 1.15 + 1e-3});
    }
    return out;
}

std::vector<Plane> planesFromJson(Graph& g, const json& j)
{
    std::vector<Plane> out;
    if (j.is_null())
        return out;
    if (!j.is_array())
        throw CommandError("splitMesh: planes must be a list of {origin, normal, radius} or {joint}");
    g.evaluate();
    for (const json& e : j)
    {
        if (!e.is_object())
            throw CommandError("splitMesh: each plane is an object such as {\"origin\": [0, 100, 0], \"normal\": [0, 1, 0]}");
        Plane p;
        if (e.contains("joint"))
        {
            const Node* jn = g.find(e["joint"].get<std::string>());
            if (!jn || !rig::isJoint(*jn))
                throw CommandError("splitMesh: " + e["joint"].get<std::string>() + " is not a joint");
            p.origin = worldPos(g, jn->id());
            if (const NodeId child = firstChildJoint(g, jn->id()); child != kNoNode)
                p.normal = unitOr(worldPos(g, child) - p.origin, {0, 1, 0});
            else if (const NodeId up = g.parent(jn->id()); up != kNoNode && rig::isJoint(*g.node(up)))
                p.normal = unitOr(p.origin - worldPos(g, up), {0, 1, 0});
        }
        if (e.contains("origin"))
            p.origin = vec3Arg(e["origin"], "origin");
        if (e.contains("normal"))
            p.normal = unitOr(vec3Arg(e["normal"], "normal"), {0, 1, 0});
        if (e.contains("radius") && e["radius"].is_number())
            p.radius = e["radius"].get<double>();
        out.push_back(p);
    }
    return out;
}

json planesToJson(std::span<const Plane> planes)
{
    json out = json::array();
    for (const Plane& p : planes)
        out.push_back({{"origin", {p.origin.x, p.origin.y, p.origin.z}}, {"normal", {p.normal.x, p.normal.y, p.normal.z}}, {"radius", p.radius}});
    return out;
}

void registerCommands()
{
    if (ops::find("splitMesh"))
        return;

    ops::registerCommand({
        .name = "splitMesh",
        .doc = "Cut a mesh into pieces along planes, each cutting only within its disc, and optionally remesh each piece. "
               "Returns the pieces.\n\n"
               "planes are world space: [{origin, normal, radius}], or {joint} for a plane at a joint facing along its bone. A radius "
               "of 0 fits the plane to the mesh's cross-section around its origin. With suggest, planes go where the joints a click "
               "selects meet on a skinned mesh. pieceSettings overrides the remesh per piece: {\"2\": {\"remesh\": \"none\"}}.",
        .positionalDoc = "mesh",
        .flags = {
            {"planes", "pl", T::Any, nullptr, "World planes: [{\"origin\": [x, y, z], \"normal\": [x, y, z], \"radius\": r}] or [{\"joint\": name}]."},
            {"suggest", "sg", T::Bool, false, "Add planes at the joint boundaries of a skinned mesh."},
            {"duplicate", "dup", T::Bool, true, "Leave the mesh and make the pieces from a copy; off replaces it."},
            {"cap", "c", T::Bool, true, "Close the holes the cuts leave."},
            {"mergeLoose", "ml", T::Bool, true, "Shells no plane cuts join the nearest piece."},
            {"remesh", "rm", T::String, "none", "How each piece is rebuilt.", {"none", "quadRemesh", "voxelRemesh", "decimate"}},
            {"targetFaces", "tf", T::Int, 300, "Quad remesh: faces per piece.", {}, {}, 8.0},
            {"resolution", "res", T::Int, 32, "Voxel remesh: voxels along a piece's longest side.", {}, {}, 4.0, 512.0},
            {"ratio", "r", T::Float, 0.25, "Decimate: share of triangles kept.", {}, {}, 0.001, 1.0},
            {"pieceSettings", "ps", T::Any, nullptr, "Per piece index: {remesh, targetFaces, resolution, ratio}."},
            {"pieceFaces", "pf", T::String, "", "Face count per piece, as index:count pairs (0:150 3:80)."},
            {"exactPieces", "ep", T::String, "", "Pieces kept exact, by index (1 4)."},
            {"name", "n", T::String, nullptr, "Base name of the pieces; the mesh's name by default."},
        },
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("splitMesh: give a mesh");
            Node* n = g.find(objs.front());
            if (!n || !n->type().isA("mesh"))
                throw CommandError("splitMesh: " + objs.front() + " is not a mesh");
            const NodeId src = n->id();
            std::vector<Plane> planes = planesFromJson(g, a.flags.value("planes", json()));
            if (a.get<bool>("suggest", false))
                for (const Plane& p : suggestPlanes(g, src))
                    planes.push_back(p);
            if (planes.empty())
                throw CommandError("splitMesh: give planes, or suggest=True on a skinned mesh");
            Remesh base;
            try
            {
                base = remeshFromJson({{"remesh", a.get<std::string>("remesh", "none")},
                                       {"targetFaces", a.get<int>("targetFaces", 300)},
                                       {"resolution", a.get<int>("resolution", 32)},
                                       {"ratio", a.get<double>("ratio", 0.25)}});
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("splitMesh: ") + e.what());
            }

            g.evaluate();
            const MeshPtr& mp = n->as<MeshPtr>(MeshNode::kOutMesh);
            if (!mp || !mp->numFaces())
                throw CommandError("splitMesh: " + n->path() + " has no faces");
            const glm::dmat4 world = n->as<glm::dmat4>(Transform::kWorldMatrix);
            Options opts{.cap = a.get<bool>("cap", true), .mergeLoose = a.get<bool>("mergeLoose", true)};
            Result res = splitByPlanes(transformed(*mp, world), planes, opts);
            for (const std::string& w : res.warnings)
                log::warn("splitMesh: {}", w);
            if (res.pieces.size() < 2)
                log::warn("splitMesh: the planes did not separate {}; widen their radius or move them onto the mesh", n->path());

            const json pieceSettings = a.flags.value("pieceSettings", json::object());
            std::vector<Remesh> settings(res.pieces.size(), base);
            for (size_t i = 0; i < settings.size(); ++i)
                if (pieceSettings.is_object() && pieceSettings.contains(std::to_string(i)))
                    try
                    {
                        settings[i] = remeshFromJson(pieceSettings[std::to_string(i)], base);
                    }
                    catch (const std::invalid_argument& e)
                    {
                        throw CommandError(std::string("splitMesh: ") + e.what());
                    }
            applyPieceOverrides(settings, a.get<std::string>("pieceFaces", ""), a.get<std::string>("exactPieces", ""));
            tbb::parallel_for(size_t(0), res.pieces.size(), [&](size_t i) {
                if (settings[i].method != Remesh::Method::None)
                    res.pieces[i] = remeshPiece(res.pieces[i], settings[i]);
            });

            UndoChunk chunk("Split Mesh");
            const std::string baseName = a.has("name") ? a.get<std::string>("name") : n->name();
            const NodeId parent = g.parent(src);
            const glm::dmat4 toLocal = glm::inverse(world);
            json out = json::array();
            std::vector<NodeId> made;
            for (size_t i = 0; i < res.pieces.size(); ++i)
            {
                Node* part = g.createNode(*types::find("mesh"), std::format("{}_piece{}", baseName, i + 1), parent, true);
                for (int attr : {Transform::kTranslate, Transform::kRotate, Transform::kScale, Transform::kRotatePivot, Transform::kScalePivot,
                                 Transform::kRotatePivotTranslate, Transform::kScalePivotTranslate, Transform::kRotateOrder})
                    g.setValue({part->id(), attr, -1}, g.storedValue({src, attr, -1}));
                g.setValue({part->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(transformed(res.pieces[i], toLocal))));
                out.push_back(part->path());
                made.push_back(part->id());
            }
            if (!a.get<bool>("duplicate", true))
                g.deleteNode(src);
            g.select(made);
            return out;
        },
        .adjustable = true,
        .examples = {"splitMesh(\"Body\", planes=[{\"origin\": [0, 140, 0], \"normal\": [0, 1, 0]}])",
                     "splitMesh(\"Body\", suggest=True, remesh=\"voxelRemesh\", pieceSettings={\"0\": {\"remesh\": \"none\"}})"},
    });

    ops::options::Override o;
    o.order = {"remesh", "targetFaces", "resolution", "ratio", "pieceFaces", "exactPieces", "cap", "mergeLoose"};
    o.hidden = {"planes", "pieceSettings", "suggest", "duplicate", "name"};
    o.adjust = {"pieceFaces", "exactPieces"};
    o.popupHidden = {"suggest", "duplicate"};
    ops::options::setOverride("splitMesh", std::move(o));

    ops::registerCommand({
        .name = "suggestSplitPlanes",
        .doc = "Planes where the joints a click selects meet on a skinned mesh, fitted to each boundary and facing the child joint. "
               "Returns [{origin, normal, radius}] for splitMesh.",
        .positionalDoc = "mesh",
        .flags = {{"minFaces", "mf", T::Int, 8, "Regions with fewer faces get no plane.", {}, {}, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> objs = a.objects();
            Node* n = objs.empty() ? nullptr : g.find(objs.front());
            if (!n || !n->type().isA("mesh"))
                throw CommandError("suggestSplitPlanes: give a skinned mesh");
            return planesToJson(suggestPlanes(g, n->id(), std::max(1, a.get<int>("minFaces", 8))));
        },
    });
}
} // namespace lila::scene::split
