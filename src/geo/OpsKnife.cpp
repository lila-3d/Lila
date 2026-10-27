#include "geo/Knife.h"

#include "geo/Topology.h"

#include "core/Profile.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>

namespace lila::geo
{
namespace
{
using Kind = KnifePoint::Kind;

double meshScale(const Mesh& m)
{
    const Bounds b = m.bounds();
    const double s = b.valid() ? glm::length(b.size()) : 0.0;
    return s > 0.0 ? s : 1.0;
}

bool validPoint(const Mesh& m, const KnifePoint& k)
{
    switch (k.kind)
    {
    case Kind::Point: return m.pointValid(k.id);
    case Kind::Edge: return m.edgeValid(k.id) && k.t >= 0.0 && k.t <= 1.0;
    case Kind::Face: return m.faceValid(k.id);
    }
    return false;
}

std::vector<Index> facesAt(const Mesh& m, const KnifePoint& k)
{
    std::vector<Index> out;
    switch (k.kind)
    {
    case Kind::Point: out = m.pointFaces(k.id); break;
    case Kind::Edge: m.forEachEdgeCorner(k.id, [&](Index c) { out.push_back(m.cornerFace(c)); }); break;
    case Kind::Face: out.push_back(k.id); break;
    }
    return out;
}

bool contains(const std::vector<Index>& v, Index x) { return std::find(v.begin(), v.end(), x) != v.end(); }

/** k's image under symmetry element g. */
std::optional<KnifePoint> imageKnifePoint(const Mesh& m, const SymmetryMap& sym, const KnifePoint& k, int g)
{
    KnifePoint out = k;
    switch (k.kind)
    {
    case Kind::Point: out.id = sym.image(g, k.id); break;
    case Kind::Edge:
        out.id = sym.imageEdge(m, g, k.id);
        if (out.id != kInvalid && m.edgePoint(out.id, 0) != sym.image(g, m.edgePoint(k.id, 0)))
            out.t = 1.0 - k.t;
        break;
    case Kind::Face: out.id = sym.imageFace(m, g, k.id); break;
    }
    if (out.id == kInvalid)
        return std::nullopt;
    out.position = sym.transform(g, k.position);
    return out;
}

/** The cutting plane of one stretch of the path, and how faces cross it. */
struct Cutter
{
    const Mesh& m;
    glm::dvec3 n{0.0};
    double offset = 0.0;
    double eps = 0.0;

    double distance(const glm::dvec3& p) const { return glm::dot(n, p) - offset; }

    /** Where the face's border meets the plane, in corner order. A point on the plane counts once. */
    std::vector<KnifePoint> crossings(Index f) const
    {
        std::vector<KnifePoint> out;
        m.forEachFaceCorner(f, [&](Index c) {
            const Index p = m.cornerPoint(c), q = m.cornerNextPoint(c);
            const double sp = distance(m.position(p)), sq = distance(m.position(q));
            if (std::abs(sp) <= eps)
            {
                KnifePoint k = KnifePoint::point(p);
                k.position = m.position(p);
                out.push_back(k);
                return;
            }
            if (std::abs(sq) <= eps || (sp > 0.0) == (sq > 0.0))
                return;
            const double s = sp / (sp - sq);
            const Index e = m.cornerEdge(c);
            KnifePoint k = KnifePoint::edge(e, m.edgePoint(e, 0) == p ? s : 1.0 - s);
            k.position = glm::mix(m.position(p), m.position(q), s);
            out.push_back(k);
        });
        return out;
    }
};

/** Walks the surface from a to b through the faces the plane crosses, always moving towards b. */
bool walk(const Cutter& cut, const KnifePoint& a, const KnifePoint& b, double posEps, bool mirrored, std::vector<KnifeSegment>& out)
{
    const Mesh& m = cut.m;
    const std::vector<Index> starts = facesAt(m, a), targets = facesAt(m, b);
    for (Index f : starts)
        if (contains(targets, f))
        {
            out.push_back({f, a, b, mirrored});
            return true;
        }
    const glm::dvec3 dir = b.position - a.position;
    const double len2 = glm::dot(dir, dir);
    if (len2 <= 0.0)
        return false;
    auto param = [&](const glm::dvec3& x) { return glm::dot(x - a.position, dir) / len2; };

    // The next crossing along the line after the entry, never past b.
    auto forwardExit = [&](Index f, const KnifePoint& entry) -> std::optional<KnifePoint> {
        const double from = param(entry.position);
        std::optional<KnifePoint> best;
        double bestParam = 0.0;
        for (const KnifePoint& k : cut.crossings(f))
        {
            if (glm::distance(k.position, entry.position) <= posEps)
                continue;
            const double p = param(k.position);
            if (p <= from + 1e-9 || p > 1.0 + 1e-6)
                continue;
            if (!best || p < bestParam)
            {
                best = k;
                bestParam = p;
            }
        }
        return best;
    };

    std::vector<KnifeSegment> best;
    bool found = false;
    std::vector<uint8_t> visited;
    for (Index start : starts)
    {
        visited.assign(m.faceCapacity(), 0);
        std::vector<KnifeSegment> segs;
        Index f = start;
        KnifePoint entry = a;
        bool ok = false;
        for (Index guard = 0; guard <= m.numFaces(); ++guard)
        {
            visited[f] = 1;
            if (contains(targets, f))
            {
                segs.push_back({f, entry, b, mirrored});
                ok = true;
                break;
            }
            std::optional<KnifePoint> exit = forwardExit(f, entry);
            if (!exit)
                break;
            segs.push_back({f, entry, *exit, mirrored});
            Index next = kInvalid;
            if (exit->kind == Kind::Edge)
            {
                next = m.oppositeFace(exit->id, f);
                if (next != kInvalid && visited[next])
                    next = kInvalid;
            }
            else
                for (Index g : m.pointFaces(exit->id))
                {
                    if (g == f || visited[g])
                        continue;
                    if (contains(targets, g))
                    {
                        next = g;
                        break;
                    }
                    if (next == kInvalid && forwardExit(g, *exit))
                        next = g;
                }
            if (next == kInvalid)
                break;
            entry = *exit;
            f = next;
        }
        if (ok && (!found || segs.size() < best.size()))
        {
            best = std::move(segs);
            found = true;
        }
    }
    if (found)
        out.insert(out.end(), best.begin(), best.end());
    return found;
}

/** Every face piece under the screen line from a to b: the plane, clipped to the wedge between the two view rays. */
bool slice(const Cutter& cut, const KnifePoint& a, const KnifePoint& b, const glm::dvec3& o0, const glm::dvec3& n0,
           const glm::dvec3& o1, const glm::dvec3& n1, double posEps, bool mirrored, std::vector<KnifeSegment>& out)
{
    const Mesh& m = cut.m;
    const size_t before = out.size();
    auto snapToClick = [&](KnifePoint k, Index f) {
        for (const KnifePoint* c : {&a, &b})
            if (c->kind == Kind::Face && c->id == f && glm::distance(c->position, k.position) <= posEps * 10.0)
                return *c;
        return k;
    };
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        std::vector<KnifePoint> cr = cut.crossings(f);
        if (cr.size() < 2 || cr.size() % 2)
            continue;
        const glm::dvec3 along = glm::cross(cut.n, m.faceNormal(f));
        if (glm::dot(along, along) < 1e-20)
            continue;
        std::sort(cr.begin(), cr.end(), [&](const KnifePoint& x, const KnifePoint& y) { return glm::dot(x.position, along) < glm::dot(y.position, along); });
        for (size_t i = 0; i + 1 < cr.size(); i += 2)
        {
            const KnifePoint& x0 = cr[i];
            const KnifePoint& x1 = cr[i + 1];
            double s0 = 0.0, s1 = 1.0;
            bool empty = false;
            for (auto [o, n] : {std::pair{o0, n0}, std::pair{o1, n1}})
            {
                const double g0 = glm::dot(n, x0.position - o), g1 = glm::dot(n, x1.position - o);
                const double tol = cut.eps;
                if (g0 < -tol && g1 < -tol)
                {
                    empty = true;
                    break;
                }
                if (g0 < -tol)
                    s0 = std::max(s0, g0 / (g0 - g1));
                if (g1 < -tol)
                    s1 = std::min(s1, g0 / (g0 - g1));
            }
            if (empty || s1 - s0 <= 1e-9)
                continue;
            auto end = [&](double s, const KnifePoint& k) {
                if (s <= 1e-9 || s >= 1.0 - 1e-9)
                    return k;
                return snapToClick(KnifePoint::face(f, glm::mix(x0.position, x1.position, s)), f);
            };
            out.push_back({f, end(s0, x0), end(s1, x1), mirrored});
        }
    }
    return out.size() > before;
}

void planChain(const Mesh& m, const std::vector<KnifePoint>& pts, const KnifeOptions& opts, const std::optional<glm::dvec3>& eye,
               const glm::dvec3& viewDir, bool mirrored, KnifePlan& plan)
{
    const double scale = meshScale(m);
    const double posEps = scale * 1e-7;
    const size_t n = pts.size();
    const size_t pairs = opts.closed && n > 2 ? n : (n ? n - 1 : 0);
    for (size_t i = 0; i < pairs; ++i)
    {
        const KnifePoint& a = pts[i];
        const KnifePoint& b = pts[(i + 1) % n];
        glm::dvec3 avgNormal(0.0);
        for (const KnifePoint* k : {&a, &b})
            for (Index f : facesAt(m, *k))
                avgNormal += m.faceNormal(f);
        const glm::dvec3 look = eye ? glm::dvec3(0.0) : glm::dot(viewDir, viewDir) > 0.0 ? viewDir : avgNormal;
        glm::dvec3 normal = eye ? glm::cross(a.position - *eye, b.position - *eye) : glm::cross(b.position - a.position, look);
        const double len = glm::length(normal);
        bool ok = false;
        std::vector<KnifeSegment> segs;
        if (len > 1e-12 * scale * scale)
        {
            normal /= len;
            const Cutter cut{m, normal, glm::dot(normal, a.position), scale * 1e-9};
            if (opts.through)
            {
                const glm::dvec3 rayA = eye ? a.position - *eye : look, rayB = eye ? b.position - *eye : look;
                const glm::dvec3 o0 = eye ? *eye : a.position, o1 = eye ? *eye : b.position;
                glm::dvec3 n0 = glm::cross(normal, rayA), n1 = glm::cross(normal, rayB);
                if (glm::dot(n0, b.position - o0) < 0.0)
                    n0 = -n0;
                if (glm::dot(n1, a.position - o1) < 0.0)
                    n1 = -n1;
                ok = slice(cut, a, b, o0, n0, o1, n1, posEps, mirrored, segs);
            }
            else
                ok = walk(cut, a, b, posEps, mirrored, segs);
        }
        else
            for (Index f : facesAt(m, a))
                if (contains(facesAt(m, b), f))
                {
                    segs.push_back({f, a, b, mirrored});
                    ok = true;
                    break;
                }
        plan.segments.insert(plan.segments.end(), segs.begin(), segs.end());
        if (!ok && !mirrored)
            plan.failed.push_back(int(i));
    }
}

void ringPlan(const Mesh& m, Index edge, double t, bool mirrored, KnifePlan& plan)
{
    auto at = [&](Index e, Index startPoint) {
        KnifePoint k = KnifePoint::edge(e, m.edgePoint(e, 0) == startPoint ? t : 1.0 - t);
        k.position = knifePosition(m, k);
        return k;
    };
    std::vector<Index> faces;
    m.forEachEdgeCorner(edge, [&](Index c) { faces.push_back(m.cornerFace(c)); });
    std::vector<uint8_t> seen(m.faceCapacity(), 0);
    for (size_t dir = 0; dir < faces.size() && dir < 2; ++dir)
    {
        Index e = edge, start = m.edgePoint(edge, 0), f = faces[dir];
        while (f != kInvalid && !seen[f])
        {
            seen[f] = 1;
            const Index opp = quadOppositeEdge(m, e, f);
            if (opp == kInvalid)
                break;
            // The start side continues along the quad's side edge from start.
            Index next = kInvalid;
            m.forEachFaceCorner(f, [&](Index c) {
                const Index p = m.cornerPoint(c), q = m.cornerNextPoint(c);
                if (m.cornerEdge(c) == e || m.cornerEdge(c) == opp)
                    return;
                if (p == start)
                    next = q;
                else if (q == start)
                    next = p;
            });
            if (next == kInvalid)
                break;
            plan.segments.push_back({f, at(e, start), at(opp, next), mirrored});
            f = m.oppositeFace(opp, f);
            e = opp;
            start = next;
        }
    }
}

/** Barycentric source corners and weights of a spot inside a face, from a fan of its corners. */
struct FaceWeights
{
    Index corners[3] = {kInvalid, kInvalid, kInvalid};
    Index points[3] = {kInvalid, kInvalid, kInvalid};
    float w[3] = {1.0f, 0.0f, 0.0f};
};

FaceWeights faceWeights(const Mesh& m, const std::vector<Index>& corners, const glm::dvec3& normal, const glm::dvec3& p)
{
    glm::dvec3 u = std::abs(normal.x) < 0.9 ? glm::cross(normal, glm::dvec3(1, 0, 0)) : glm::cross(normal, glm::dvec3(0, 1, 0));
    u = glm::normalize(u);
    const glm::dvec3 v = glm::cross(normal, u);
    auto flat = [&](const glm::dvec3& x) { return glm::dvec2(glm::dot(x, u), glm::dot(x, v)); };
    const glm::dvec2 q = flat(p);
    FaceWeights best;
    double bestViolation = std::numeric_limits<double>::max();
    for (size_t k = 1; k + 1 < corners.size(); ++k)
    {
        const Index cs[3] = {corners[0], corners[k], corners[k + 1]};
        const glm::dvec2 a = flat(m.position(m.cornerPoint(cs[0]))), b = flat(m.position(m.cornerPoint(cs[1]))),
                         c = flat(m.position(m.cornerPoint(cs[2])));
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-20)
            continue;
        const double wa = ((b.x - q.x) * (c.y - q.y) - (b.y - q.y) * (c.x - q.x)) / area;
        const double wb = ((c.x - q.x) * (a.y - q.y) - (c.y - q.y) * (a.x - q.x)) / area;
        const double wc = 1.0 - wa - wb;
        const double violation = std::max({0.0, -wa, -wb, -wc});
        if (violation < bestViolation)
        {
            bestViolation = violation;
            const double ws[3] = {std::max(0.0, wa), std::max(0.0, wb), std::max(0.0, wc)};
            const double sum = ws[0] + ws[1] + ws[2];
            for (int i = 0; i < 3; ++i)
            {
                best.corners[i] = cs[i];
                best.points[i] = m.cornerPoint(cs[i]);
                best.w[i] = float(sum > 0.0 ? ws[i] / sum : 1.0 / 3.0);
            }
        }
    }
    return best;
}

bool insideFace(const Mesh& m, Index f, const glm::dvec3& p)
{
    const glm::dvec3 n = m.faceNormal(f);
    glm::dvec3 u = std::abs(n.x) < 0.9 ? glm::cross(n, glm::dvec3(1, 0, 0)) : glm::cross(n, glm::dvec3(0, 1, 0));
    u = glm::normalize(u);
    const glm::dvec3 v = glm::cross(n, u);
    std::vector<glm::dvec2> poly;
    m.forEachFaceCorner(f, [&](Index c) { poly.push_back({glm::dot(m.position(m.cornerPoint(c)), u), glm::dot(m.position(m.cornerPoint(c)), v)}); });
    const glm::dvec2 q(glm::dot(p, u), glm::dot(p, v));
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > q.y) != (poly[j].y > q.y) && q.x < (poly[j].x - poly[i].x) * (q.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            in = !in;
    return in;
}
} // namespace

glm::dvec3 knifePosition(const Mesh& mesh, const KnifePoint& p)
{
    switch (p.kind)
    {
    case Kind::Point: return mesh.position(p.id);
    case Kind::Edge: return glm::mix(mesh.position(mesh.edgePoint(p.id, 0)), mesh.position(mesh.edgePoint(p.id, 1)), p.t);
    case Kind::Face: return p.position;
    }
    return p.position;
}

KnifePlan knifePlan(const Mesh& mesh, std::span<const KnifePoint> points, const KnifeOptions& opts)
{
    LILA_ZONE();

    KnifePlan plan;
    std::vector<KnifePoint> pts;
    for (KnifePoint k : points)
        if (validPoint(mesh, k))
        {
            k.position = knifePosition(mesh, k);
            pts.push_back(k);
        }
    planChain(mesh, pts, opts, opts.eye, opts.viewDir, false, plan);
    if (opts.symmetry && !opts.symmetry->empty())
    {
        const SymmetryMap& sym = *opts.symmetry;
        for (int g = 1; g < sym.order(); ++g)
        {
            std::vector<KnifePoint> mirrored;
            for (const KnifePoint& k : pts)
                if (std::optional<KnifePoint> mk = imageKnifePoint(mesh, sym, k, g))
                    mirrored.push_back(*mk);
            if (mirrored.size() != pts.size())
                continue;
            std::optional<glm::dvec3> eye;
            if (opts.eye)
                eye = sym.transform(g, *opts.eye);
            planChain(mesh, mirrored, opts, eye, sym.transformVector(g, opts.viewDir), true, plan);
        }
    }
    return plan;
}

KnifePlan knifeLoopPlan(const Mesh& mesh, Index edge, double t, const SymmetryMap* symmetry)
{
    LILA_ZONE();

    KnifePlan plan;
    if (!mesh.edgeValid(edge))
        return plan;
    t = std::clamp(t, 0.0, 1.0);
    ringPlan(mesh, edge, t, false, plan);
    if (!symmetry || symmetry->empty())
        return plan;
    // A ring an element maps onto itself at the same spot (round the axis) is planned once.
    auto planned = [&](const KnifePoint& k) {
        for (const KnifeSegment& s : plan.segments)
            for (const KnifePoint* q : {&s.a, &s.b})
                if (q->kind == Kind::Edge && q->id == k.id && std::abs(q->t - k.t) < 1e-9)
                    return true;
        return false;
    };
    for (int g = 1; g < symmetry->order(); ++g)
        if (std::optional<KnifePoint> mk = imageKnifePoint(mesh, *symmetry, KnifePoint::edge(edge, t), g); mk && !planned(*mk))
            ringPlan(mesh, mk->id, mk->t, true, plan);
    return plan;
}

void addSymmetricImages(const Mesh& mesh, KnifePlan& plan, const SymmetryMap& symmetry)
{
    LILA_ZONE();

    if (symmetry.empty())
        return;
    const size_t own = plan.segments.size();
    for (int g = 1; g < symmetry.order(); ++g)
        for (size_t i = 0; i < own; ++i)
        {
            const KnifeSegment s = plan.segments[i];
            const Index f = symmetry.imageFace(mesh, g, s.face);
            const std::optional<KnifePoint> a = imageKnifePoint(mesh, symmetry, s.a, g), b = imageKnifePoint(mesh, symmetry, s.b, g);
            if (f != kInvalid && a && b)
                plan.segments.push_back({f, *a, *b, true});
        }
}

std::vector<Index> knifeApply(Mesh& mesh, const KnifePlan& plan)
{
    LILA_ZONE();

    constexpr double kTTol = 1e-7;
    const double posEps = meshScale(mesh) * 1e-7;

    auto normalize = [&](KnifePoint k) {
        if (k.kind == Kind::Edge)
        {
            if (k.t <= kTTol)
                k = KnifePoint::point(mesh.edgePoint(k.id, 0));
            else if (k.t >= 1.0 - kTTol)
                k = KnifePoint::point(mesh.edgePoint(k.id, 1));
        }
        k.position = knifePosition(mesh, k);
        return k;
    };

    std::vector<KnifeSegment> segs;
    for (const KnifeSegment& s : plan.segments)
        if (mesh.faceValid(s.face) && validPoint(mesh, s.a) && validPoint(mesh, s.b))
            segs.push_back({s.face, normalize(s.a), normalize(s.b), s.mirrored});

    // Split every crossed edge once per distinct spot, from its first point on.
    std::map<Index, std::vector<double>> cuts;
    for (const KnifeSegment& s : segs)
        for (const KnifePoint* k : {&s.a, &s.b})
            if (k->kind == Kind::Edge)
                cuts[k->id].push_back(k->t);
    std::map<Index, std::vector<std::pair<double, Index>>> made;
    for (auto& [e, ts] : cuts)
    {
        std::sort(ts.begin(), ts.end());
        Index cur = e;
        double prev = 0.0;
        for (double x : ts)
        {
            if (!made[e].empty() && x - made[e].back().first <= kTTol)
                continue;
            const Index p = mesh.splitEdge(cur, (x - prev) / (1.0 - prev));
            made[e].push_back({x, p});
            cur = mesh.lastSplitEdge();
            prev = x;
        }
    }
    auto vertexOf = [&](const KnifePoint& k) {
        if (k.kind == Kind::Point)
            return k.id;
        if (k.kind == Kind::Face)
            return kInvalid;
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (auto& [x, p] : made[k.id])
            if (std::abs(x - k.t) < bestD)
            {
                bestD = std::abs(x - k.t);
                best = p;
            }
        return best;
    };

    std::map<Index, std::vector<const KnifeSegment*>> byFace;
    for (const KnifeSegment& s : segs)
        byFace[s.face].push_back(&s);

    std::vector<Index> origin(mesh.faceCapacity());
    std::iota(origin.begin(), origin.end(), Index(0));
    std::vector<Index> out;

    for (auto& [face, list] : byFace)
    {
        // Path graph inside the face: border points, and face-inside spots joined by the pieces.
        struct Node
        {
            Index vertex = kInvalid;
            glm::dvec3 pos{0.0};
            std::vector<int> links;
        };
        std::vector<Node> nodes;
        auto nodeOf = [&](const KnifePoint& k) {
            const Index v = vertexOf(k);
            for (size_t i = 0; i < nodes.size(); ++i)
                if (v != kInvalid ? nodes[i].vertex == v : nodes[i].vertex == kInvalid && glm::distance(nodes[i].pos, k.position) <= posEps)
                    return int(i);
            nodes.push_back({v, v != kInvalid ? mesh.position(v) : k.position, {}});
            return int(nodes.size() - 1);
        };
        std::set<std::pair<int, int>> linkSet;
        for (const KnifeSegment* s : list)
        {
            const int a = nodeOf(s->a), b = nodeOf(s->b);
            if (a != b)
                linkSet.insert({std::min(a, b), std::max(a, b)});
        }
        std::vector<std::pair<int, int>> links(linkSet.begin(), linkSet.end());
        for (size_t l = 0; l < links.size(); ++l)
        {
            nodes[links[l].first].links.push_back(int(l));
            nodes[links[l].second].links.push_back(int(l));
        }

        std::vector<std::vector<int>> paths;
        std::vector<uint8_t> used(links.size(), 0);
        for (int start = 0; start < int(nodes.size()); ++start)
        {
            if (nodes[start].vertex == kInvalid)
                continue;
            for (int l0 : nodes[start].links)
            {
                if (used[l0])
                    continue;
                std::vector<int> path{start};
                int link = l0, cur = start;
                bool dead = false;
                while (true)
                {
                    used[link] = 1;
                    cur = links[link].first == cur ? links[link].second : links[link].first;
                    path.push_back(cur);
                    if (nodes[cur].vertex != kInvalid)
                        break;
                    if (nodes[cur].links.size() != 2)
                    {
                        dead = true;
                        break;
                    }
                    link = nodes[cur].links[0] == link ? nodes[cur].links[1] : nodes[cur].links[0];
                    if (used[link])
                    {
                        dead = true;
                        break;
                    }
                }
                if (!dead)
                    paths.push_back(std::move(path));
            }
        }
        if (paths.empty())
            continue;

        const std::vector<Index> srcCorners = mesh.faceCorners(face);
        const glm::dvec3 srcNormal = mesh.faceNormal(face);

        for (const std::vector<int>& path : paths)
        {
            const Index va = nodes[path.front()].vertex, vb = nodes[path.back()].vertex;
            std::vector<glm::dvec3> inner;
            for (size_t i = 1; i + 1 < path.size(); ++i)
                inner.push_back(nodes[path[i]].pos);
            if (va == vb || mesh.findEdge(va, vb) != kInvalid)
                continue;

            // The piece of the original face that still holds both ends with room between them.
            const glm::dvec3 probe = inner.empty() ? (mesh.position(va) + mesh.position(vb)) * 0.5 : inner.front();
            Index target = kInvalid, ca = kInvalid, cb = kInvalid;
            for (Index f : mesh.pointFaces(va))
            {
                if (f >= origin.size() || origin[f] != face)
                    continue;
                const Index a = mesh.findCorner(f, va), b = mesh.findCorner(f, vb);
                if (b == kInvalid || mesh.cornerNext(a) == b || mesh.cornerNext(b) == a)
                    continue;
                const bool better = target == kInvalid || insideFace(mesh, f, probe);
                if (better)
                {
                    target = f;
                    ca = a;
                    cb = b;
                }
            }
            if (target == kInvalid)
                continue;
            const Index f2 = mesh.splitFace(target, ca, cb);
            if (f2 == kInvalid)
                continue;
            origin.resize(mesh.faceCapacity(), kInvalid);
            origin[f2] = face;
            Index e = mesh.lastSplitEdge();
            if (mesh.edgePoint(e, 0) != va)
                std::reverse(inner.begin(), inner.end());
            for (size_t i = 0; i < inner.size(); ++i)
            {
                const Index p = mesh.splitEdge(e, 1.0 / double(inner.size() - i + 1));
                mesh.setPosition(p, inner[i]);
                const FaceWeights fw = faceWeights(mesh, srcCorners, srcNormal, inner[i]);
                if (fw.corners[0] != kInvalid)
                {
                    mesh.pointAttrs().mix(p, fw.points, fw.w);
                    mesh.forEachPointCorner(p, [&](Index c) { mesh.cornerAttrs().mix(c, fw.corners, fw.w); });
                }
                out.push_back(e);
                e = mesh.lastSplitEdge();
            }
            out.push_back(e);
        }
    }
    return out;
}

std::vector<Index> knifeCut(Mesh& mesh, std::span<const KnifePoint> points, const KnifeOptions& opts)
{
    return knifeApply(mesh, knifePlan(mesh, points, opts));
}
} // namespace lila::geo
