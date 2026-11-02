#include "PrintGeo.h"

#include <lila/Boolean.h>
#include <lila/MeshOps.h>
#include <lila/Retopo.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>

namespace print
{
using lila::geo::kInvalid;

namespace
{
constexpr double kPi = 3.14159265358979323846;

double degrees(double rad) { return rad * 180.0 / kPi; }

std::vector<Index> liveFaces(const Mesh& m)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

double diagonal(const Mesh& m)
{
    const lila::geo::Bounds b = m.bounds();
    return b.valid() ? glm::length(b.size()) : 0.0;
}

/** Any unit vector perpendicular to n. */
glm::dvec3 perpendicular(const glm::dvec3& n)
{
    const glm::dvec3 a = std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    return glm::normalize(glm::cross(n, a));
}

/** Rotation taking unit a onto unit b. */
glm::dmat3 rotationBetween(const glm::dvec3& a, const glm::dvec3& b)
{
    const double c = glm::dot(a, b);
    if (c > 1.0 - 1e-12)
        return glm::dmat3(1.0);
    glm::dvec3 axis = glm::cross(a, b);
    double s = glm::length(axis);
    if (s < 1e-9)
    {
        axis = perpendicular(a);
        s = 0.0;
    }
    else
        axis /= s;
    const double angle = std::atan2(s, c);
    const double cs = std::cos(angle), sn = std::sin(angle), t = 1.0 - cs;
    const glm::dvec3& k = axis;
    // Rodrigues, column-major.
    return glm::dmat3(t * k.x * k.x + cs, t * k.x * k.y + sn * k.z, t * k.x * k.z - sn * k.y,
                      t * k.x * k.y - sn * k.z, t * k.y * k.y + cs, t * k.y * k.z + sn * k.x,
                      t * k.x * k.z + sn * k.y, t * k.y * k.z - sn * k.x, t * k.z * k.z + cs);
}

struct UnionFind
{
    std::vector<Index> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), Index(0)); }
    Index find(Index a)
    {
        while (parent[a] != a)
            a = parent[a] = parent[parent[a]];
        return a;
    }
    void join(Index a, Index b) { parent[find(a)] = find(b); }
};

/** Segment p-q against triangle abc, excluding touching at the very ends. */
bool segmentHitsTriangle(const glm::dvec3& p, const glm::dvec3& q, const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c)
{
    const glm::dvec3 d = q - p, e1 = b - a, e2 = c - a;
    const glm::dvec3 h = glm::cross(d, e2);
    const double det = glm::dot(e1, h);
    const double scale = glm::length(e1) * glm::length(e2) * glm::length(d);
    if (std::abs(det) <= 1e-12 * scale)
        return false;
    const double inv = 1.0 / det;
    const glm::dvec3 s = p - a;
    const double u = glm::dot(s, h) * inv;
    if (u < 0.0 || u > 1.0)
        return false;
    const glm::dvec3 qv = glm::cross(s, e1);
    const double v = glm::dot(d, qv) * inv;
    if (v < 0.0 || u + v > 1.0)
        return false;
    const double t = glm::dot(e2, qv) * inv;
    return t > 1e-9 && t < 1.0 - 1e-9;
}

bool trianglesIntersect(const std::array<glm::dvec3, 3>& t0, const std::array<glm::dvec3, 3>& t1)
{
    for (int i = 0; i < 3; ++i)
    {
        if (segmentHitsTriangle(t0[i], t0[(i + 1) % 3], t1[0], t1[1], t1[2]))
            return true;
        if (segmentHitsTriangle(t1[i], t1[(i + 1) % 3], t0[0], t0[1], t0[2]))
            return true;
    }
    return false;
}

/** Positive-volume pieces only: a cavity (an inward wound inner shell) is part of the solid around it. */
size_t solidShells(const Mesh& mesh, const std::vector<std::vector<Index>>& pieces)
{
    size_t n = 0;
    const lila::geo::Triangulation tris = lila::geo::triangulate(mesh);
    for (const std::vector<Index>& faces : pieces)
    {
        double v = 0.0;
        for (Index f : faces)
            for (Index t = tris.faceFirst[f]; t < tris.triangleCount() && tris.faces[t] == f; ++t)
            {
                const glm::dvec3& a = mesh.position(mesh.cornerPoint(tris.corners[t * 3]));
                const glm::dvec3& b = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + 1]));
                const glm::dvec3& c = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + 2]));
                v += glm::dot(a, glm::cross(b, c));
            }
        if (v > 0.0)
            ++n;
    }
    return n;
}

void transformMesh(Mesh& m, const glm::dmat3& r, const glm::dvec3& offset = glm::dvec3(0.0))
{
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, r * m.position(p) + offset);
}

std::string booleanInto(Mesh& base, const Mesh& tool, lila::scene::BooleanOp op)
{
    const lila::scene::BooleanOperand operands[2] = {{&base, glm::dmat4(1.0)}, {&tool, glm::dmat4(1.0)}};
    lila::scene::BooleanOptions o;
    o.op = op;
    lila::scene::BooleanResult r = lila::scene::meshBoolean(operands, glm::dmat4(1.0), o);
    if (!r.ok())
        return r.error;
    base = std::move(r.mesh);
    return {};
}
} // namespace

const std::vector<Printer>& printers()
{
    static const std::vector<Printer> list = {
        {"prusaMk4", "Prusa MK4", {250.0, 210.0, 220.0}, 0.4, 0.8, 45.0},
        {"prusaMini", "Prusa MINI+", {180.0, 180.0, 180.0}, 0.4, 0.8, 45.0},
        {"bambuX1", "Bambu Lab X1 / P1", {256.0, 256.0, 256.0}, 0.4, 0.8, 45.0},
        {"bambuA1Mini", "Bambu Lab A1 mini", {180.0, 180.0, 180.0}, 0.4, 0.8, 45.0},
        {"ender3", "Creality Ender-3", {220.0, 220.0, 250.0}, 0.4, 0.8, 45.0},
        {"voron350", "Voron 2.4 350", {350.0, 350.0, 340.0}, 0.4, 0.8, 45.0},
        {"form4", "Formlabs Form 4", {200.0, 125.0, 210.0}, 0.0, 0.4, 30.0},
        {"saturn4", "Elegoo Saturn 4 Ultra", {218.0, 123.0, 220.0}, 0.0, 0.4, 30.0},
        {"custom", "Custom", {200.0, 200.0, 200.0}, 0.4, 0.8, 45.0},
    };
    return list;
}

const Printer* findPrinter(std::string_view id)
{
    for (const Printer& p : printers())
        if (p.id == id)
            return &p;
    return nullptr;
}

const char* checkName(Check c)
{
    switch (c)
    {
    case Check::NonManifold: return "nonManifold";
    case Check::Holes: return "holes";
    case Check::Flipped: return "flipped";
    case Check::ZeroArea: return "zeroArea";
    case Check::SelfIntersections: return "selfIntersections";
    case Check::Shells: return "shells";
    case Check::ThinWalls: return "thinWalls";
    default: return "";
    }
}

const char* checkLabel(Check c)
{
    switch (c)
    {
    case Check::NonManifold: return "non-manifold edges or points";
    case Check::Holes: return "holes";
    case Check::Flipped: return "flipped faces";
    case Check::ZeroArea: return "zero-area faces";
    case Check::SelfIntersections: return "self-intersecting faces";
    case Check::Shells: return "separate shells";
    case Check::ThinWalls: return "points under the minimum wall";
    default: return "";
    }
}

bool CheckReport::clean() const
{
    for (size_t i = 0; i < kCheckCount; ++i)
        if (Check(i) == Check::Shells ? results[i].count > 1 : results[i].count > 0)
            return false;
    return true;
}

CheckReport checkPrintability(const Mesh& mesh, const CheckOptions& opts)
{
    using lila::retopo::MeshIssue;
    CheckReport out;
    auto on = [&](Check c) { return opts.checks[size_t(c)]; };

    lila::retopo::RepairOptions ro;
    ro.checks.fill(false);
    ro.set(MeshIssue::NonManifoldEdge, on(Check::NonManifold));
    ro.set(MeshIssue::NonManifoldPoint, on(Check::NonManifold));
    ro.set(MeshIssue::Hole, on(Check::Holes));
    ro.set(MeshIssue::ZeroAreaFace, on(Check::ZeroArea));
    ro.set(MeshIssue::Flipped, on(Check::Flipped));
    ro.tolerance = opts.tolerance;
    const lila::retopo::MeshIssues found = lila::retopo::findIssues(mesh, ro);

    CheckResult& nm = out.results[size_t(Check::NonManifold)];
    nm.edges = found.of(MeshIssue::NonManifoldEdge);
    nm.points = found.of(MeshIssue::NonManifoldPoint);
    nm.count = nm.edges.size() + nm.points.size();

    CheckResult& holes = out.results[size_t(Check::Holes)];
    holes.edges = found.of(MeshIssue::Hole);
    if (!holes.edges.empty())
    {
        UnionFind uf(mesh.pointCapacity());
        for (Index e : holes.edges)
            uf.join(mesh.edgePoint(e, 0), mesh.edgePoint(e, 1));
        std::vector<Index> roots;
        for (Index e : holes.edges)
            roots.push_back(uf.find(mesh.edgePoint(e, 0)));
        std::ranges::sort(roots);
        holes.count = size_t(std::unique(roots.begin(), roots.end()) - roots.begin());
    }

    CheckResult& flipped = out.results[size_t(Check::Flipped)];
    flipped.faces = found.of(MeshIssue::Flipped);
    flipped.count = flipped.faces.size();

    CheckResult& zero = out.results[size_t(Check::ZeroArea)];
    zero.faces = found.of(MeshIssue::ZeroAreaFace);
    zero.count = zero.faces.size();

    if (on(Check::SelfIntersections))
    {
        CheckResult& si = out.results[size_t(Check::SelfIntersections)];
        si.faces = selfIntersectingFaces(mesh);
        si.count = si.faces.size();
    }

    if (on(Check::Shells))
    {
        CheckResult& sh = out.results[size_t(Check::Shells)];
        const std::vector<std::vector<Index>> pieces = shells(mesh);
        sh.count = solidShells(mesh, pieces);
        if (sh.count > 1)
            for (size_t i = 1; i < pieces.size(); ++i)
                sh.faces.insert(sh.faces.end(), pieces[i].begin(), pieces[i].end());
        std::ranges::sort(sh.faces);
    }

    if (on(Check::ThinWalls) && opts.minWall > 0.0)
    {
        CheckResult& thin = out.results[size_t(Check::ThinWalls)];
        const std::vector<double> th = wallThickness(mesh);
        for (Index p = 0; p < mesh.pointCapacity(); ++p)
            if (mesh.pointValid(p) && th[p] < opts.minWall)
                thin.points.push_back(p);
        thin.count = thin.points.size();
    }
    return out;
}

std::vector<std::vector<Index>> shells(const Mesh& mesh)
{
    UnionFind uf(mesh.faceCapacity());
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        const Index c0 = mesh.edgeCorner(e);
        if (c0 == kInvalid)
            continue;
        for (Index c = mesh.cornerRadial(c0); c != c0 && c != kInvalid; c = mesh.cornerRadial(c))
            uf.join(mesh.cornerFace(c0), mesh.cornerFace(c));
    }
    std::vector<std::vector<Index>> groups;
    std::vector<Index> slot(mesh.faceCapacity(), kInvalid);
    for (Index f : liveFaces(mesh))
    {
        const Index r = uf.find(f);
        if (slot[r] == kInvalid)
        {
            slot[r] = Index(groups.size());
            groups.emplace_back();
        }
        groups[slot[r]].push_back(f);
    }
    std::ranges::stable_sort(groups, [](const auto& a, const auto& b) { return a.size() > b.size(); });
    return groups;
}

std::vector<Index> selfIntersectingFaces(const Mesh& mesh)
{
    if (mesh.numFaces() < 2)
        return {};
    const lila::geo::MeshBvh bvh(mesh);
    const lila::geo::Triangulation& tris = bvh.triangles();
    const Index n = tris.triangleCount();
    auto tri = [&](Index t) {
        std::array<glm::dvec3, 3> out;
        for (int i = 0; i < 3; ++i)
            out[i] = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + i]));
        return out;
    };
    auto pts = [&](Index t) {
        return std::array<Index, 3>{mesh.cornerPoint(tris.corners[t * 3]), mesh.cornerPoint(tris.corners[t * 3 + 1]),
                                    mesh.cornerPoint(tris.corners[t * 3 + 2])};
    };
    std::vector<char> hit(mesh.faceCapacity(), 0);
    for (Index t = 0; t < n; ++t)
    {
        const std::array<glm::dvec3, 3> a = tri(t);
        lila::geo::Bounds box;
        for (const glm::dvec3& p : a)
        {
            box.min = glm::min(box.min, p);
            box.max = glm::max(box.max, p);
        }
        const std::array<Index, 3> ap = pts(t);
        for (Index u : bvh.overlapping(box))
        {
            if (u <= t || tris.faces[u] == tris.faces[t])
                continue;
            const std::array<Index, 3> bp = pts(u);
            bool shared = false;
            for (Index x : ap)
                shared |= std::ranges::find(bp, x) != bp.end();
            if (shared || !trianglesIntersect(a, tri(u)))
                continue;
            hit[tris.faces[t]] = hit[tris.faces[u]] = 1;
        }
    }
    std::vector<Index> out;
    for (Index f = 0; f < Index(hit.size()); ++f)
        if (hit[f])
            out.push_back(f);
    return out;
}

std::vector<double> wallThickness(const Mesh& mesh)
{
    std::vector<double> out(mesh.pointCapacity(), kInf);
    if (mesh.numFaces() == 0)
        return out;
    const lila::geo::MeshBvh bvh(mesh);
    const double eps = std::max(diagonal(mesh) * 1e-6, 1e-9);
    auto inward = [&](const glm::dvec3& from, const glm::dvec3& n) {
        if (glm::dot(n, n) < 1e-24)
            return kInf;
        lila::geo::BvhHit hit;
        const glm::dvec3 dir = -glm::normalize(n);
        if (!bvh.raycast(from + dir * eps, dir, hit))
            return kInf;
        return hit.t + eps;
    };
    std::vector<double> faceThickness(mesh.faceCapacity(), kInf);
    for (Index f : liveFaces(mesh))
        faceThickness[f] = inward(mesh.faceCentroid(f), mesh.faceNormal(f));
    const std::vector<glm::vec3> normals = lila::geo::pointNormals(mesh);
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        double t = inward(mesh.position(p), glm::dvec3(normals[p]));
        for (Index f : mesh.pointFaces(p))
            t = std::min(t, faceThickness[f]);
        out[p] = t;
    }
    return out;
}

std::vector<double> overhangAngles(const Mesh& mesh, const glm::dvec3& up)
{
    std::vector<double> out(mesh.faceCapacity(), 0.0);
    const glm::dvec3 u = glm::normalize(up);
    double bed = kInf;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            bed = std::min(bed, glm::dot(mesh.position(p), u));
    const double tol = std::max(diagonal(mesh) * 1e-5, 1e-9);
    for (Index f : liveFaces(mesh))
    {
        const double down = -glm::dot(mesh.faceNormal(f), u);
        if (down <= 1e-9)
            continue;
        bool onBed = true;
        for (Index p : mesh.facePoints(f))
            onBed &= glm::dot(mesh.position(p), u) - bed <= tol;
        if (!onBed)
            out[f] = degrees(std::asin(std::clamp(down, 0.0, 1.0)));
    }
    return out;
}

double supportArea(const Mesh& mesh, const glm::dvec3& up, double threshold)
{
    const std::vector<double> angles = overhangAngles(mesh, up);
    const glm::dvec3 u = glm::normalize(up);
    double area = 0.0;
    for (Index f : liveFaces(mesh))
        if (angles[f] > threshold)
            area += -glm::dot(mesh.faceAreaVector(f), u) * 0.5;
    return area;
}

double volume(const Mesh& mesh)
{
    const lila::geo::Triangulation tris = lila::geo::triangulate(mesh);
    double v = 0.0;
    for (Index t = 0; t < tris.triangleCount(); ++t)
    {
        const glm::dvec3& a = mesh.position(mesh.cornerPoint(tris.corners[t * 3]));
        const glm::dvec3& b = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + 1]));
        const glm::dvec3& c = mesh.position(mesh.cornerPoint(tris.corners[t * 3 + 2]));
        v += glm::dot(a, glm::cross(b, c));
    }
    return v / 6.0;
}

double surfaceArea(const Mesh& mesh)
{
    double a = 0.0;
    for (Index f : liveFaces(mesh))
        a += mesh.faceArea(f);
    return a;
}

glm::vec4 heatColor(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const glm::vec3 red(0.85f, 0.12f, 0.10f), yellow(0.95f, 0.80f, 0.12f), green(0.18f, 0.70f, 0.26f);
    const glm::vec3 c = t < 0.5 ? glm::mix(red, yellow, float(t * 2.0)) : glm::mix(yellow, green, float(t * 2.0 - 1.0));
    return {c, 1.0f};
}

Orientation bestOrientation(const Mesh& mesh, const glm::dvec3& up, double threshold)
{
    const glm::dvec3 u = glm::normalize(up);
    std::vector<glm::dvec3> downs = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<Index> faces = liveFaces(mesh);
    std::ranges::sort(faces, [&](Index a, Index b) { return mesh.faceArea(a) > mesh.faceArea(b); });
    for (size_t i = 0; i < std::min<size_t>(faces.size(), 12); ++i)
    {
        const glm::dvec3 n = mesh.faceNormal(faces[i]);
        if (glm::dot(n, n) > 0.5 && std::ranges::none_of(downs, [&](const glm::dvec3& d) { return glm::dot(d, n) > 0.9999; }))
            downs.push_back(n);
    }

    Orientation best;
    best.currentArea = supportArea(mesh, u, threshold);
    double bestScore = kInf;
    Mesh turned = mesh;
    for (const glm::dvec3& d : downs)
    {
        const glm::dmat3 r = rotationBetween(glm::normalize(d), -u);
        for (Index p = 0; p < mesh.pointCapacity(); ++p)
            if (mesh.pointValid(p))
                turned.setPosition(p, r * mesh.position(p));
        const double area = supportArea(turned, u, threshold);
        double lo = kInf, hi = -kInf;
        for (Index p = 0; p < turned.pointCapacity(); ++p)
            if (turned.pointValid(p))
            {
                const double h = glm::dot(turned.position(p), u);
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        // Height breaks ties between equal support areas: lower prints faster and steadier.
        const double score = area + (hi - lo) * 1e-6;
        if (score < bestScore - 1e-9)
        {
            bestScore = score;
            best.rotation = r;
            best.supportArea = area;
        }
    }
    return best;
}

Mesh box(const glm::dvec3& lo, const glm::dvec3& hi)
{
    Mesh m;
    Index p[8];
    for (int i = 0; i < 8; ++i)
        p[i] = m.addPoint({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
    m.addFace({p[0], p[4], p[6], p[2]});
    m.addFace({p[1], p[3], p[7], p[5]});
    m.addFace({p[0], p[1], p[5], p[4]});
    m.addFace({p[2], p[6], p[7], p[3]});
    m.addFace({p[0], p[2], p[3], p[1]});
    m.addFace({p[4], p[5], p[7], p[6]});
    return m;
}

Mesh cylinder(const glm::dvec3& base, const glm::dvec3& dir, double radius, double length, int segments)
{
    segments = std::max(segments, 3);
    const glm::dvec3 w = glm::normalize(dir);
    const glm::dvec3 u = perpendicular(w), v = glm::cross(w, u);
    Mesh m;
    std::vector<Index> bottom, top;
    for (int i = 0; i < segments; ++i)
    {
        const double a = 2.0 * kPi * i / segments;
        const glm::dvec3 r = (u * std::cos(a) + v * std::sin(a)) * radius;
        bottom.push_back(m.addPoint(base + r));
        top.push_back(m.addPoint(base + r + w * length));
    }
    for (int i = 0; i < segments; ++i)
    {
        const int j = (i + 1) % segments;
        m.addFace({bottom[i], bottom[j], top[j], top[i]});
    }
    m.addFace(top);
    std::ranges::reverse(bottom);
    m.addFace(bottom);
    return m;
}

Mesh hollow(const Mesh& mesh, double thickness)
{
    Mesh inner = mesh;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        std::vector<glm::dvec3> normals;
        glm::dvec3 sum(0.0);
        for (Index f : mesh.pointFaces(p))
        {
            const glm::dvec3 n = mesh.faceNormal(f);
            if (glm::dot(n, n) < 0.5)
                continue;
            sum += n;
            if (std::ranges::none_of(normals, [&](const glm::dvec3& o) { return glm::dot(o, n) > 0.9998; }))
                normals.push_back(n);
        }
        if (normals.empty())
            continue;
        glm::dvec3 d = glm::dot(sum, sum) > 1e-24 ? -thickness * glm::normalize(sum) : -thickness * normals[0];
        // Projects onto each face plane in turn (Kaczmarz), so every plane ends up thickness away.
        for (int sweep = 0; sweep < 20; ++sweep)
            for (const glm::dvec3& n : normals)
                d += (-thickness - glm::dot(d, n)) * n;
        if (const double len = glm::length(d); len > thickness * 3.0)
            d *= thickness * 3.0 / len;
        inner.setPosition(p, mesh.position(p) + d);
    }
    for (Index f : liveFaces(inner))
        inner.flipFace(f);
    Mesh out = mesh;
    out.append(inner);
    return out;
}

SurfacePoint lowestPoint(const Mesh& mesh, const glm::dvec3& up)
{
    const glm::dvec3 u = glm::normalize(up);
    SurfacePoint best;
    double lowest = kInf, area = 0.0;
    const double tol = std::max(diagonal(mesh) * 1e-5, 1e-9);
    for (Index f : liveFaces(mesh))
    {
        const glm::dvec3 n = mesh.faceNormal(f);
        if (glm::dot(n, u) >= -1e-6)
            continue;
        const glm::dvec3 c = mesh.faceCentroid(f);
        const double h = glm::dot(c, u), a = mesh.faceArea(f);
        // Among faces at the same height the largest wins, so a flat base drains from its middle.
        if (h < lowest - tol || (h <= lowest + tol && a > area))
        {
            lowest = std::min(lowest, h);
            area = a;
            best = {c, n};
        }
    }
    return best;
}

std::string drainHole(Mesh& mesh, const SurfacePoint& at, double radius, double depth)
{
    if (glm::dot(at.normal, at.normal) < 1e-12)
        return "no surface there";
    const glm::dvec3 n = glm::normalize(at.normal);
    const Mesh cyl = cylinder(at.position + n * radius, -n, radius, depth + radius, 24);
    return booleanInto(mesh, cyl, lila::scene::BooleanOp::Difference);
}

SplitResult splitByPlane(const Mesh& mesh, const glm::dvec3& point, const glm::dvec3& normal, const PegOptions& pegs)
{
    SplitResult out;
    if (glm::dot(normal, normal) < 1e-24)
    {
        out.error = "the plane has no normal";
        return out;
    }
    const glm::dvec3 n = glm::normalize(normal);
    const double size = std::max(diagonal(mesh), 1e-6) * 2.0 + glm::length(point - mesh.bounds().center());
    const glm::dvec3 u = perpendicular(n), v = glm::cross(n, u);
    const glm::dmat3 frame(u, v, n);

    auto halfSpace = [&](double sign) {
        Mesh b = sign > 0.0 ? box({-size, -size, 0.0}, {size, size, size}) : box({-size, -size, -size}, {size, size, 0.0});
        transformMesh(b, frame, point);
        return b;
    };
    out.above = mesh;
    out.below = mesh;
    if (std::string e = booleanInto(out.above, halfSpace(1.0), lila::scene::BooleanOp::Intersection); !e.empty())
    {
        out.error = e;
        return out;
    }
    if (std::string e = booleanInto(out.below, halfSpace(-1.0), lila::scene::BooleanOp::Intersection); !e.empty())
    {
        out.error = e;
        return out;
    }
    if (out.above.numFaces() == 0 || out.below.numFaces() == 0)
    {
        out.error = "the plane misses the mesh";
        return out;
    }
    if (pegs.count <= 0 || pegs.radius <= 0.0 || pegs.length <= 0.0)
        return out;

    // The cut face on the lower part: faces lying in the plane, facing along it.
    const double tol = std::max(diagonal(mesh) * 1e-5, 1e-7);
    std::vector<Index> cap;
    glm::dvec3 centre(0.0);
    double capArea = 0.0;
    for (Index f : liveFaces(out.below))
    {
        if (glm::dot(out.below.faceNormal(f), n) < 0.999)
            continue;
        bool inPlane = true;
        for (Index p : out.below.facePoints(f))
            inPlane &= std::abs(glm::dot(out.below.position(p) - point, n)) <= tol;
        if (!inPlane)
            continue;
        cap.push_back(f);
        const double a = out.below.faceArea(f);
        centre += out.below.faceCentroid(f) * a;
        capArea += a;
    }
    if (cap.empty() || capArea <= 0.0)
        return out;
    centre /= capArea;

    std::vector<glm::dvec3> sites;
    if (pegs.count == 1)
        sites.push_back(centre);
    else
    {
        // Spread along the cut's longer side.
        double lo[2] = {kInf, kInf}, hi[2] = {-kInf, -kInf};
        for (Index f : cap)
            for (Index p : out.below.facePoints(f))
            {
                const glm::dvec3 d = out.below.position(p) - centre;
                lo[0] = std::min(lo[0], glm::dot(d, u));
                hi[0] = std::max(hi[0], glm::dot(d, u));
                lo[1] = std::min(lo[1], glm::dot(d, v));
                hi[1] = std::max(hi[1], glm::dot(d, v));
            }
        const bool alongU = hi[0] - lo[0] >= hi[1] - lo[1];
        const glm::dvec3 axis = alongU ? u : v;
        const double a = alongU ? lo[0] : lo[1], b = alongU ? hi[0] : hi[1];
        for (int i = 0; i < pegs.count; ++i)
            sites.push_back(centre + axis * (a + (b - a) * (i + 1.0) / (pegs.count + 1.0)));
    }

    {
        const lila::geo::MeshBvh capBvh(out.below);
        auto onCap = [&](const glm::dvec3& s) {
            lila::geo::BvhHit hit;
            return capBvh.raycast(s + n * tol * 10.0, -n, hit, tol * 20.0) && std::ranges::find(cap, hit.face) != cap.end();
        };
        // The hole's whole rim has to land on the cut, or the peg breaks through the part's side (or a hollow's cavity).
        const double rim = pegs.radius + pegs.tolerance;
        auto fits = [&](const glm::dvec3& s) {
            if (!onCap(s))
                return false;
            for (int k = 0; k < 12; ++k)
            {
                const double a = 2.0 * kPi * k / 12.0;
                if (!onCap(s + (u * std::cos(a) + v * std::sin(a)) * rim))
                    return false;
            }
            return true;
        };
        std::vector<glm::dvec3> candidates;
        for (Index f : cap)
            candidates.push_back(out.below.faceCentroid(f));
        for (Index f : cap)
            for (Index p : out.below.facePoints(f))
                candidates.push_back((out.below.faceCentroid(f) + out.below.position(p)) * 0.5);
        std::vector<glm::dvec3> placed;
        for (const glm::dvec3& s : sites)
        {
            std::optional<glm::dvec3> at;
            if (fits(s))
                at = s;
            else
            {
                std::ranges::sort(candidates, [&](const glm::dvec3& a, const glm::dvec3& b) { return glm::length(a - s) < glm::length(b - s); });
                for (const glm::dvec3& c : candidates)
                    if (std::ranges::none_of(placed, [&](const glm::dvec3& o) { return glm::length(o - c) < rim * 2.5; }) && fits(c))
                    {
                        at = c;
                        break;
                    }
            }
            if (at)
                placed.push_back(*at);
        }
        sites = std::move(placed);
    }
    for (const glm::dvec3& s : sites)
    {
        const Mesh peg = cylinder(s - n * pegs.radius, n, pegs.radius, pegs.length + pegs.radius, 24);
        const Mesh hole = cylinder(s - n * pegs.radius, n, pegs.radius + pegs.tolerance, pegs.length + pegs.tolerance + pegs.radius, 24);
        if (std::string e = booleanInto(out.below, peg, lila::scene::BooleanOp::Union); !e.empty())
        {
            out.error = e;
            return out;
        }
        if (std::string e = booleanInto(out.above, hole, lila::scene::BooleanOp::Difference); !e.empty())
        {
            out.error = e;
            return out;
        }
        ++out.pegs;
    }
    return out;
}
} // namespace print
