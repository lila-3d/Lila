// Cloth solver: Vertex Block Descent (Chen, Macklin, Yuksel 2024) with Augmented VBD hard constraints (Giles et al.
// 2025). Each vertex takes a 3x3 Newton step on its local energy, Gauss-Seidel across graph colours. Seams, pins and
// strain limits are augmented Lagrangian constraints whose stiffness ramps up while they stay violated.

#include "TailorCore.h"

#include <glm/glm.hpp>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace tailor
{
namespace
{
namespace vbd
{
using glm::mat3;
using glm::vec2;
using glm::vec3;

constexpr float kInf = std::numeric_limits<float>::max();
constexpr float kStrengthen = 10.f;    // stiffness multiplier on Strengthened panels
constexpr float kBoundFraction = 0.4f; // VBD conservative bound: fraction of the distance to the nearest primitive
constexpr float kLambdaDecay = 0.98f;  // AVBD warm start between substeps
constexpr float kStiffnessDecay = 0.99f;
constexpr float kConstraintKMax = 1e6f;
constexpr float kConstraintBeta = 1e6f;
constexpr float kContactStiffness = 1e4f;
constexpr float kAirDrag = 0.2f;       // 1/s
constexpr float kFrictionSlip = 1e-5f; // tangential slip in metres still treated as sticking
constexpr float kClosedGap = 1e-4f;
constexpr int kRedetectEvery = 3;     // iterations between contact re-detections while vertices hit their bounds
constexpr float kDampingScale = 0.01f; // fabric damping 0.01 becomes a 1e-4 s stiffness-proportional (Rayleigh) term

vec3 toVec(const Vec3& v) { return {v[0], v[1], v[2]}; }
Vec3 toArr(const vec3& v) { return {v.x, v.y, v.z}; }
mat3 outer(const vec3& a, const vec3& b) { return glm::outerProduct(a, b); }

/** Closest point to p on triangle abc (Ericson), with its barycentric weights. */
vec3 closestOnTriangle(const vec3& p, const vec3& a, const vec3& b, const vec3& c, vec3& bary)
{
    const vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.f && d2 <= 0.f)
    {
        bary = {1.f, 0.f, 0.f};
        return a;
    }
    const vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.f && d4 <= d3)
    {
        bary = {0.f, 1.f, 0.f};
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
        const float t = d1 / (d1 - d3);
        bary = {1.f - t, t, 0.f};
        return a + t * ab;
    }
    const vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.f && d5 <= d6)
    {
        bary = {0.f, 0.f, 1.f};
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
        const float t = d2 / (d2 - d6);
        bary = {1.f - t, 0.f, t};
        return a + t * ac;
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.f && d4 - d3 >= 0.f && d5 - d6 >= 0.f)
    {
        const float t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        bary = {0.f, 1.f - t, t};
        return b + t * (c - b);
    }
    const float sum = va + vb + vc;
    if (std::abs(sum) < 1e-30f)
    {
        bary = {1.f, 0.f, 0.f};
        return a;
    }
    const float v = vb / sum, w = vc / sum;
    bary = {1.f - v - w, v, w};
    return a + ab * v + ac * w;
}

/** Dihedral angle across edge ab between triangles abc and abd (0 when flat) and its gradient for a, b, c, d. */
bool dihedral(const vec3& a, const vec3& b, const vec3& c, const vec3& d, float& theta, vec3* grad)
{
    const vec3 e = b - a;
    const float el2 = glm::dot(e, e);
    if (el2 < 1e-16f)
        return false;
    const float el = std::sqrt(el2);
    const vec3 n1 = glm::cross(e, c - a), n2 = glm::cross(d - a, e);
    const float l1 = glm::length(n1), l2 = glm::length(n2);
    if (l1 < 1e-12f || l2 < 1e-12f)
        return false;
    const vec3 u1 = n1 / l1, u2 = n2 / l2;
    theta = std::atan2(glm::dot(glm::cross(u1, u2), e) / el, glm::dot(u1, u2));
    if (!grad)
        return true;
    const vec3 gc = -u1 * (el / l1), gd = -u2 * (el / l2);
    const float tc = glm::dot(c - a, e) / el2, td = glm::dot(d - a, e) / el2;
    grad[0] = -(1.f - tc) * gc - (1.f - td) * gd;
    grad[1] = -tc * gc - td * gd;
    grad[2] = gc;
    grad[3] = gd;
    return true;
}

float wrapAngle(float a)
{
    constexpr float kPi = 3.14159265358979f;
    while (a > kPi)
        a -= 2.f * kPi;
    while (a < -kPi)
        a += 2.f * kPi;
    return a;
}

/** Inverse rest shape of a triangle in its fabric's grain frame, plus the stiffnesses that act on it. */
struct Membrane
{
    float b00 = 0.f, b01 = 0.f, b10 = 0.f, b11 = 0.f;
    float area = 0.f;
    float kWarp = 0.f, kWeft = 0.f, kShear = 0.f;
};

/** Bending hinge: edge v[0]-v[1], opposite corners v[2] and v[3]. Energy 0.5 * stiffness * (angle - rest)^2. */
struct Hinge
{
    int v[4] = {-1, -1, -1, -1};
    float stiffness = 0.f;
    float restAngle = 0.f;
};

struct Ref
{
    int index = 0;
    int role = 0;
};

enum class LinkKind : uint8_t
{
    Seam,
    Strain
};

/** Two-vertex hard constraint. Open: |a - b| <= limit (lambda in x). Closed seam: a == b (vector lambda). */
struct Link
{
    LinkKind kind = LinkKind::Seam;
    int a = -1, b = -1;
    float limit = 0.f;
    float rest = 0.f;
    float kStart = 1.f;
    float k = 1.f;
    vec3 lambda{0.f};
    bool closed = false;
};

struct PinState
{
    float kStart = 1.f;
    float k = 1.f;
    vec3 lambda{0.f};
};

/** Vertex-triangle pair within the query radius. `side` keeps the vertex on the side of the triangle it was found on. */
struct Contact
{
    int v = -1;
    int t = -1;
    float side = 1.f;
    float dist = 0.f;
};

uint64_t cellKey(int x, int y, int z)
{
    constexpr int64_t kBias = 1 << 20;
    constexpr uint64_t kMask = (1u << 21) - 1u;
    return ((uint64_t(int64_t(x) + kBias) & kMask) << 42) | ((uint64_t(int64_t(y) + kBias) & kMask) << 21) |
           (uint64_t(int64_t(z) + kBias) & kMask);
}

/** Open-addressed map from a cell key to its run in the key-sorted vertex list. */
struct CellTable
{
    struct Slot
    {
        uint64_t key = ~0ull;
        int begin = 0, end = 0;
    };
    std::vector<Slot> slots;
    uint64_t mask = 0;

    static uint64_t hash(uint64_t k) { return (k ^ (k >> 29)) * 0xbf58476d1ce4e5b9ull; }

    void build(const std::vector<std::pair<uint64_t, int>>& sorted)
    {
        size_t size = 64;
        while (size < sorted.size() * 2)
            size *= 2;
        slots.assign(size, {});
        mask = size - 1;
        for (size_t i = 0; i < sorted.size();)
        {
            size_t j = i;
            while (j < sorted.size() && sorted[j].first == sorted[i].first)
                ++j;
            uint64_t h = hash(sorted[i].first) & mask;
            while (slots[h].key != ~0ull)
                h = (h + 1) & mask;
            slots[h] = {sorted[i].first, int(i), int(j)};
            i = j;
        }
    }

    const Slot* find(uint64_t key) const
    {
        for (uint64_t h = hash(key) & mask;; h = (h + 1) & mask)
        {
            if (slots[h].key == key)
                return &slots[h];
            if (slots[h].key == ~0ull)
                return nullptr;
        }
    }
};

int findRoot(std::vector<int>& parent, int i)
{
    while (parent[i] != i)
    {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}
} // namespace vbd
} // namespace

struct ClothSolver::Impl
{
    using vec3 = glm::vec3;
    using mat3 = glm::mat3;

    SolverSettings s;
    float h = 1.f / 120.f;
    int n = 0;

    std::vector<std::array<int, 3>> tris;
    std::vector<vbd::Membrane> membranes;
    std::vector<vbd::Hinge> hinges;
    std::vector<vbd::Link> links;
    std::vector<vbd::PinState> pinStates;
    std::vector<int> pinOf, pinVerts;

    std::vector<float> mass, damping, friction, thickness;
    std::vector<float> bodyK, bodyLambda; // collider contact as an AVBD constraint: stiffness and normal force per vertex
    std::vector<uint8_t> movable, collidable, triCollides;
    std::vector<int> weld, vPanel;
    std::vector<vec3> restFlat; // pattern position (x, y, 0); empty without UVs
    std::vector<std::vector<int>> nearRing; // per weld root: roots within two rings, sorted; never self-contacts
    std::vector<std::vector<vbd::Ref>> vTris, vHinges, vLinks;
    std::vector<std::vector<int>> colours;

    float queryRadius = 0.01f;
    float cellSize = 0.02f;
    std::vector<vbd::Contact> contacts;
    std::vector<std::vector<vbd::Contact>> perTri;
    std::vector<std::vector<int>> asVertex, asTriangle;
    std::vector<std::pair<uint64_t, int>> cells;
    vbd::CellTable cellTable;
    std::vector<float> bound, nearV, nearT;

    std::vector<vec3> x, v, xStart, xDetect, y, xNew;
    std::vector<uint8_t> clamped;

    void build(const ClothMesh& mesh, const Garment& g);
    void syncPins(const std::vector<Pin>& pins);
    void advanceSeams();
    void substep(const std::vector<Pin>& pins, const std::vector<const Sdf*>& colliders);
    void detect();
    void updateDuals(const std::vector<Pin>& pins, const std::vector<const Sdf*>& colliders);
    /** Deepest penetration of vertex i into the colliders and the push-out normal; false when clear. */
    bool bodyContact(int i, const vec3& p, const std::vector<const Sdf*>& colliders, float& pen, vec3& nrm) const;
    vec3 solveVertex(int i, const std::vector<Pin>& pins, const std::vector<const Sdf*>& colliders) const;
    bool contactGap(const vbd::Contact& c, float& pen, vec3& normal, vec3& bary) const;

    float contactStiffness(int i) const { return vbd::kContactStiffness + 1e3f * mass[i] / (h * h); }
    float selfRadius(int i) const { return s.collisionThickness + thickness[i]; }

    vec3 clampToBound(int i, const vec3& p)
    {
        const vec3 d = p - xDetect[i];
        const float l2 = glm::dot(d, d);
        const float b = bound[i];
        if (l2 <= b * b)
            return p;
        clamped[i] = 1;
        return xDetect[i] + d * (b / std::sqrt(l2));
    }
};

void ClothSolver::Impl::build(const ClothMesh& mesh, const Garment& g)
{
    using namespace vbd;
    n = int(mesh.rest3d.size());
    static const Fabric kDefaultFabric;
    auto panelOf = [&](int p) -> const Panel* { return p >= 0 && p < int(g.panels.size()) ? &g.panels[p] : nullptr; };
    auto vertPanel = [&](int i) { return i < int(mesh.vertPanel.size()) ? mesh.vertPanel[i] : 0; };
    auto fabricOf = [&](const Panel* p) -> const Fabric& {
        if (p && p->fabric >= 0 && p->fabric < int(g.fabrics.size()))
            return g.fabrics[p->fabric];
        return g.fabrics.empty() ? kDefaultFabric : g.fabrics.front();
    };

    mass.assign(n, 0.f);
    damping.assign(n, 0.f);
    friction.assign(n, 0.f);
    thickness.assign(n, 0.f);
    movable.assign(n, 0);
    collidable.assign(n, 0);
    for (int i = 0; i < n; ++i)
    {
        const Panel* p = panelOf(vertPanel(i));
        const Fabric& f = fabricOf(p);
        const PanelState state = p ? p->state : PanelState::Active;
        damping[i] = f.damping;
        friction[i] = f.friction;
        thickness[i] = f.thickness;
        movable[i] = state == PanelState::Active || state == PanelState::Strengthened;
        collidable[i] = state != PanelState::Deactivated;
    }

    const bool hasUv = mesh.uv.size() == mesh.rest3d.size();
    vPanel.resize(n);
    restFlat.clear();
    for (int i = 0; i < n; ++i)
    {
        vPanel[i] = vertPanel(i);
        if (hasUv)
            restFlat.push_back(vec3(mesh.uv[i][0], mesh.uv[i][1], 0.f));
    }
    tris.clear();
    std::vector<int> triPanel;
    for (size_t t = 0; t < mesh.tris.size(); ++t)
    {
        const auto& tri = mesh.tris[t];
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            ok = ok && tri[k] >= 0 && tri[k] < n;
        if (!ok || tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
            continue;
        tris.push_back(tri);
        triPanel.push_back(t < mesh.triPanel.size() ? mesh.triPanel[t] : vertPanel(tri[0]));
    }
    const int nt = int(tris.size());

    // Rest corners in the grain frame: warp along x, scaled by the fabric's shrink.
    std::vector<std::array<vec2, 3>> restCorners(nt);
    membranes.assign(nt, {});
    triCollides.assign(nt, 0);
    vTris.assign(n, {});
    for (int t = 0; t < nt; ++t)
    {
        const auto& tri = tris[t];
        const Panel* p = panelOf(triPanel[t]);
        const Fabric& f = fabricOf(p);
        std::array<vec2, 3>& rc = restCorners[t];
        if (hasUv)
        {
            const float ga = p ? p->grainAngle : 0.f;
            const vec2 warp(std::cos(ga), std::sin(ga)), weft(-std::sin(ga), std::cos(ga));
            for (int k = 0; k < 3; ++k)
            {
                const vec2 u(mesh.uv[tri[k]][0], mesh.uv[tri[k]][1]);
                rc[k] = {glm::dot(u, warp) * f.shrinkWarp, glm::dot(u, weft) * f.shrinkWeft};
            }
        }
        else
        {
            const vec3 r0 = toVec(mesh.rest3d[tri[0]]), e1 = toVec(mesh.rest3d[tri[1]]) - r0,
                       e2 = toVec(mesh.rest3d[tri[2]]) - r0;
            const float l1 = glm::length(e1);
            const vec3 nrm = glm::cross(e1, e2);
            if (l1 > 1e-12f && glm::length(nrm) > 1e-20f)
            {
                const vec3 ax = e1 / l1, ay = glm::normalize(glm::cross(nrm, ax));
                rc = {vec2(0.f), vec2(l1, 0.f), vec2(glm::dot(e2, ax), glm::dot(e2, ay))};
            }
            else
                rc = {vec2(0.f), vec2(0.f), vec2(0.f)};
        }
        const vec2 d1 = rc[1] - rc[0], d2 = rc[2] - rc[0];
        const float det = d1.x * d2.y - d2.x * d1.y;
        Membrane& m = membranes[t];
        if (std::abs(det) > 1e-14f)
        {
            m.b00 = d2.y / det;
            m.b01 = -d2.x / det;
            m.b10 = -d1.y / det;
            m.b11 = d1.x / det;
            m.area = 0.5f * std::abs(det);
            const float mult = p && p->state == PanelState::Strengthened ? kStrengthen : 1.f;
            m.kWarp = f.stretchWarp * mult;
            m.kWeft = f.stretchWeft * mult;
            m.kShear = f.shear * mult;
            for (int k = 0; k < 3; ++k)
                mass[tri[k]] += f.density * m.area / 3.f;
        }
        bool coll = true;
        for (int k = 0; k < 3; ++k)
        {
            vTris[tri[k]].push_back({t, k});
            coll = coll && collidable[tri[k]];
        }
        triCollides[t] = coll;
    }
    for (int i = 0; i < n; ++i)
        movable[i] = movable[i] && mass[i] > 0.f;

    // Edges: (key, tri, local edge index), grouped by key so interior edges become hinges.
    struct EdgeRec
    {
        uint64_t key;
        int tri;
        int local;
    };
    std::vector<EdgeRec> edges;
    edges.reserve(size_t(nt) * 3);
    for (int t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const int a = tris[t][k], b = tris[t][(k + 1) % 3];
            edges.push_back({(uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b)), t, k});
        }
    std::sort(edges.begin(), edges.end(), [](const EdgeRec& l, const EdgeRec& r) {
        return l.key != r.key ? l.key < r.key : l.tri < r.tri;
    });
    std::vector<uint64_t> creases;
    for (const auto& ce : mesh.creaseEdges)
        creases.push_back((uint64_t(std::min(ce[0], ce[1])) << 32) | uint64_t(std::max(ce[0], ce[1])));
    std::sort(creases.begin(), creases.end());

    hinges.clear();
    links.clear();
    double edgeSum = 0.0;
    size_t edgeCount = 0;
    for (size_t e = 0; e < edges.size();)
    {
        size_t end = e;
        while (end < edges.size() && edges[end].key == edges[e].key)
            ++end;
        const EdgeRec& r1 = edges[e];
        const int a = tris[r1.tri][r1.local], b = tris[r1.tri][(r1.local + 1) % 3];
        const Panel* p1 = panelOf(triPanel[r1.tri]);
        const Fabric& f1 = fabricOf(p1);
        const vec2 restEdge = restCorners[r1.tri][(r1.local + 1) % 3] - restCorners[r1.tri][r1.local];
        const float restLen = glm::length(restEdge);
        edgeSum += glm::length(toVec(mesh.rest3d[a]) - toVec(mesh.rest3d[b]));
        ++edgeCount;

        if (end - e == 2 && restLen > 1e-9f)
        {
            const EdgeRec& r2 = edges[e + 1];
            Hinge hg;
            hg.v[0] = a;
            hg.v[1] = b;
            hg.v[2] = tris[r1.tri][(r1.local + 2) % 3];
            hg.v[3] = tris[r2.tri][(r2.local + 2) % 3];
            float kBend = 0.5f * (f1.bendWarp + f1.bendWeft);
            if (hasUv)
            {
                // A fold line running along the warp bends the weft, and the reverse; bias peaks at 45 degrees.
                const float c2 = restEdge.x * restEdge.x / (restLen * restLen), s2 = 1.f - c2;
                kBend = f1.bendWeft * c2 * c2 + f1.bendWarp * s2 * s2 + 2.f * f1.bendBias * c2 * s2;
            }
            if (p1 && p1->state == PanelState::Strengthened)
                kBend *= kStrengthen;
            const float areas = membranes[r1.tri].area + membranes[r2.tri].area;
            if (areas > 1e-14f)
            {
                hg.stiffness = 6.f * kBend * restLen * restLen / areas;
                const bool crease = std::binary_search(creases.begin(), creases.end(), r1.key);
                if (crease || !hasUv)
                    dihedral(toVec(mesh.rest3d[hg.v[0]]), toVec(mesh.rest3d[hg.v[1]]), toVec(mesh.rest3d[hg.v[2]]),
                             toVec(mesh.rest3d[hg.v[3]]), hg.restAngle, nullptr);
                hinges.push_back(hg);
            }
        }
        if (f1.strainLimit > 0.f && restLen > 1e-9f && (movable[a] || movable[b]))
        {
            Link l;
            l.kind = LinkKind::Strain;
            l.a = a;
            l.b = b;
            l.limit = (1.f + f1.strainLimit) * restLen;
            links.push_back(l);
        }
        e = end;
    }

    // Folded seams: hinges across the sewn edge with the seam's rest angle.
    for (const SeamHinge& sh : seamHinges(mesh, g))
    {
        auto restArea = [&](int t) {
            const auto& tri = mesh.tris[size_t(t)];
            const vec2 p0 = hasUv ? vec2(mesh.uv[tri[0]][0], mesh.uv[tri[0]][1]) : vec2(0.f);
            const vec2 e1 = hasUv ? vec2(mesh.uv[tri[1]][0], mesh.uv[tri[1]][1]) - p0 : vec2(0.f);
            const vec2 e2 = hasUv ? vec2(mesh.uv[tri[2]][0], mesh.uv[tri[2]][1]) - p0 : vec2(0.f);
            return 0.5f * std::abs(e1.x * e2.y - e2.x * e1.y);
        };
        const float areas = restArea(sh.tris[0]) + restArea(sh.tris[1]);
        const float restLen = glm::length(toVec(mesh.rest3d[sh.v[0]]) - toVec(mesh.rest3d[sh.v[1]]));
        if (areas <= 1e-14f || restLen <= 1e-9f)
            continue;
        Hinge hg;
        for (int k = 0; k < 4; ++k)
            hg.v[k] = sh.v[k];
        hg.stiffness = 6.f * sh.bend * restLen * restLen / areas;
        hg.restAngle = sh.restAngle;
        hinges.push_back(hg);
    }

    for (size_t i = 0; i < mesh.seamPairs.size(); ++i)
    {
        const int a = mesh.seamPairs[i][0], b = mesh.seamPairs[i][1];
        if (a < 0 || b < 0 || a >= n || b >= n || a == b || (!movable[a] && !movable[b]))
            continue;
        Link l;
        l.kind = LinkKind::Seam;
        l.a = a;
        l.b = b;
        l.rest = i < mesh.seamRestLength.size() ? std::max(0.f, mesh.seamRestLength[i]) : 0.f;
        l.limit = std::max(l.rest, glm::length(toVec(mesh.rest3d[a]) - toVec(mesh.rest3d[b])));
        l.closed = l.rest <= 1e-6f && l.limit <= kClosedGap;
        if (l.closed)
            l.limit = 0.f;
        links.push_back(l);
    }
    for (const SeamShrink& sk : seamShrinks(mesh, g))
    {
        if (!movable[sk.a] && !movable[sk.b])
            continue;
        Link l;
        l.kind = LinkKind::Strain;
        l.a = sk.a;
        l.b = sk.b;
        l.limit = sk.limit;
        links.push_back(l);
    }

    vHinges.assign(n, {});
    for (int k = 0; k < int(hinges.size()); ++k)
        for (int r = 0; r < 4; ++r)
            vHinges[hinges[k].v[r]].push_back({k, r});
    vLinks.assign(n, {});
    const float h2 = h * h;
    for (int k = 0; k < int(links.size()); ++k)
    {
        Link& l = links[k];
        l.kStart = std::max(1.f, 10.f * 0.5f * (mass[l.a] + mass[l.b]) / h2);
        l.k = l.kStart;
        vLinks[l.a].push_back({k, 0});
        vLinks[l.b].push_back({k, 1});
    }

    // Seam partners act as one vertex for self collision.
    weld.resize(n);
    std::iota(weld.begin(), weld.end(), 0);
    for (const Link& l : links)
        if (l.kind == LinkKind::Seam)
        {
            const int ra = findRoot(weld, l.a), rb = findRoot(weld, l.b);
            weld[std::max(ra, rb)] = std::min(ra, rb);
        }
    for (int i = 0; i < n; ++i)
        weld[i] = findRoot(weld, i);

    // A vertex near triangles of its own neighbourhood is resting fabric, not a collision.
    std::vector<std::vector<int>> ring1(n);
    for (const auto& tri : tris)
        for (int k = 0; k < 3; ++k)
            for (int l = 0; l < 3; ++l)
                if (k != l)
                    ring1[weld[tri[k]]].push_back(weld[tri[l]]);
    for (auto& r : ring1)
    {
        std::sort(r.begin(), r.end());
        r.erase(std::unique(r.begin(), r.end()), r.end());
    }
    nearRing.assign(n, {});
    for (int i = 0; i < n; ++i)
    {
        if (weld[i] != i)
            continue;
        std::vector<int>& out = nearRing[i];
        out = ring1[i];
        for (int nb : ring1[i])
            out.insert(out.end(), ring1[nb].begin(), ring1[nb].end());
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    // Greedy colouring over the stencils that couple vertices: triangles, hinges and links.
    std::vector<std::vector<int>> adj(n);
    auto connect = [&](int a, int b) {
        adj[a].push_back(b);
        adj[b].push_back(a);
    };
    for (const auto& tri : tris)
    {
        connect(tri[0], tri[1]);
        connect(tri[1], tri[2]);
        connect(tri[2], tri[0]);
    }
    for (const Hinge& hg : hinges)
        connect(hg.v[2], hg.v[3]);
    for (const Link& l : links)
        connect(l.a, l.b);
    std::vector<int> colour(n, -1), usedBy;
    colours.clear();
    for (int i = 0; i < n; ++i)
    {
        if (!movable[i])
            continue;
        for (int nb : adj[i])
            if (colour[nb] >= 0)
            {
                if (int(usedBy.size()) <= colour[nb])
                    usedBy.resize(colour[nb] + 1, -1);
                usedBy[colour[nb]] = i;
            }
        int c = 0;
        while (c < int(usedBy.size()) && usedBy[c] == i)
            ++c;
        colour[i] = c;
        if (int(colours.size()) <= c)
            colours.resize(c + 1);
        colours[c].push_back(i);
    }

    float maxThickness = 0.f;
    for (float t : thickness)
        maxThickness = std::max(maxThickness, t);
    const float avgEdge = edgeCount ? float(edgeSum / double(edgeCount)) : 0.02f;
    queryRadius = std::max(3.f * (s.collisionThickness + maxThickness), 0.5f * avgEdge);
    cellSize = std::max(queryRadius, avgEdge);

    pinStates.clear();
    pinVerts.clear();
    pinOf.assign(n, -1);
    bound.assign(n, kInf);
    nearV.assign(n, 0.f);
    nearT.assign(nt, 0.f);
    asVertex.assign(n, {});
    asTriangle.assign(n, {});
    perTri.assign(nt, {});
    xStart.assign(n, vec3(0.f));
    xDetect.assign(n, vec3(0.f));
    y.assign(n, vec3(0.f));
    xNew.assign(n, vec3(0.f));
    clamped.assign(n, 0);
    bodyK.assign(n, 0.f);
    for (int i = 0; i < n; ++i)
        bodyK[i] = contactStiffness(i);
    bodyLambda.assign(n, 0.f);
}

void ClothSolver::Impl::syncPins(const std::vector<Pin>& pins)
{
    bool same = pinVerts.size() == pins.size();
    for (size_t i = 0; same && i < pins.size(); ++i)
        same = pinVerts[i] == pins[i].vertex;
    if (same)
        return;
    pinVerts.resize(pins.size());
    pinStates.assign(pins.size(), {});
    pinOf.assign(n, -1);
    for (size_t i = 0; i < pins.size(); ++i)
    {
        const int vi = pins[i].vertex;
        pinVerts[i] = vi;
        if (vi < 0 || vi >= n || !movable[vi])
            continue;
        pinOf[vi] = int(i);
        pinStates[i].kStart = std::max(1.f, 10.f * mass[vi] / (h * h));
        pinStates[i].k = pinStates[i].kStart;
    }
}

void ClothSolver::Impl::advanceSeams()
{
    const float keep = 1.f - std::clamp(s.seamCloseRate, 0.f, 1.f);
    for (vbd::Link& l : links)
    {
        if (l.kind != vbd::LinkKind::Seam || l.closed)
            continue;
        l.limit = l.rest + (l.limit - l.rest) * keep;
        if (l.rest <= 1e-6f && l.limit <= vbd::kClosedGap)
        {
            const vec3 d = x[l.a] - x[l.b];
            const float len = glm::length(d);
            l.lambda = len > 1e-9f ? d * (l.lambda.x / len) : vec3(0.f);
            l.limit = 0.f;
            l.closed = true;
        }
    }
}

bool ClothSolver::Impl::contactGap(const vbd::Contact& c, float& pen, vec3& normal, vec3& bary) const
{
    const auto& t = tris[c.t];
    const vec3 a = x[t[0]], b = x[t[1]], cc = x[t[2]], p = x[c.v];
    vec3 nt = glm::cross(b - a, cc - a);
    const float l = glm::length(nt);
    if (l < 1e-12f)
        return false;
    nt *= c.side / l;
    const vec3 q = vbd::closestOnTriangle(p, a, b, cc, bary);
    const vec3 d = p - q;
    float dist = glm::length(d);
    const bool interior = bary.x > 1e-4f && bary.y > 1e-4f && bary.z > 1e-4f;
    if (!interior && dist > 1e-7f)
        normal = d / dist;
    else
    {
        normal = nt;
        dist = glm::dot(d, nt);
    }
    pen = selfRadius(c.v) - dist;
    return pen > 0.f;
}

void ClothSolver::Impl::detect()
{
    using namespace vbd;
    xDetect = x;
    contacts.clear();
    for (auto& l : asVertex)
        l.clear();
    for (auto& l : asTriangle)
        l.clear();
    if (!s.selfCollision)
    {
        std::fill(bound.begin(), bound.end(), kInf);
        return;
    }

    const float R = queryRadius;
    const float inv = 1.f / cellSize;
    auto cellOf = [&](float c) { return int(std::floor(c * inv)); };
    cells.clear();
    for (int i = 0; i < n; ++i)
        if (collidable[i])
            cells.emplace_back(cellKey(cellOf(x[i].x), cellOf(x[i].y), cellOf(x[i].z)), i);
    std::sort(cells.begin(), cells.end());
    cellTable.build(cells);

    const int nt = int(tris.size());
    tbb::parallel_for(tbb::blocked_range<int>(0, nt, 32), [&](const tbb::blocked_range<int>& range) {
        for (int t = range.begin(); t != range.end(); ++t)
        {
            std::vector<Contact>& found = perTri[t];
            found.clear();
            if (!triCollides[t])
                continue;
            const auto& tri = tris[t];
            const vec3 a = x[tri[0]], b = x[tri[1]], c = x[tri[2]];
            const bool triMoves = movable[tri[0]] || movable[tri[1]] || movable[tri[2]];
            const vec3 lo = glm::min(a, glm::min(b, c)) - vec3(R), hi = glm::max(a, glm::max(b, c)) + vec3(R);
            vec3 nrm = glm::cross(b - a, c - a);
            const float nl = glm::length(nrm);
            if (nl < 1e-12f)
                continue;
            nrm /= nl;
            const int w0[3] = {weld[tri[0]], weld[tri[1]], weld[tri[2]]};
            for (int cz = cellOf(lo.z); cz <= cellOf(hi.z); ++cz)
                for (int cy = cellOf(lo.y); cy <= cellOf(hi.y); ++cy)
                    for (int cx = cellOf(lo.x); cx <= cellOf(hi.x); ++cx)
                    {
                        const CellTable::Slot* slot = cellTable.find(cellKey(cx, cy, cz));
                        if (!slot)
                            continue;
                        for (int k = slot->begin; k < slot->end; ++k)
                        {
                            const int vi = cells[k].second;
                            const int wv = weld[vi];
                            if (wv == w0[0] || wv == w0[1] || wv == w0[2] || (!movable[vi] && !triMoves))
                                continue;
                            if (!restFlat.empty() && vPanel[vi] == vPanel[tri[0]] && vPanel[vi] == vPanel[tri[1]] &&
                                vPanel[vi] == vPanel[tri[2]])
                            {
                                // Closer in the flat pattern than the query radius: the same stretch of fabric.
                                vec3 rb;
                                const vec3 rq = closestOnTriangle(restFlat[vi], restFlat[tri[0]], restFlat[tri[1]], restFlat[tri[2]], rb);
                                if (glm::length(restFlat[vi] - rq) < 2.f * R)
                                    continue;
                            }
                            const std::vector<int>& ring = nearRing[wv];
                            if (std::binary_search(ring.begin(), ring.end(), w0[0]) ||
                                std::binary_search(ring.begin(), ring.end(), w0[1]) ||
                                std::binary_search(ring.begin(), ring.end(), w0[2]))
                                continue;
                            const vec3& xv = x[vi];
                            if (xv.x < lo.x || xv.y < lo.y || xv.z < lo.z || xv.x > hi.x || xv.y > hi.y || xv.z > hi.z)
                                continue;
                            vec3 bary;
                            const vec3 q = closestOnTriangle(x[vi], a, b, c, bary);
                            const float dist = glm::length(x[vi] - q);
                            if (dist >= R)
                                continue;
                            found.push_back({vi, t, glm::dot(x[vi] - q, nrm) >= 0.f ? 1.f : -1.f, dist});
                        }
                    }
        }
    });

    std::fill(nearV.begin(), nearV.end(), R);
    std::fill(nearT.begin(), nearT.end(), R);
    for (int t = 0; t < nt; ++t)
        for (const Contact& c : perTri[t])
        {
            const int idx = int(contacts.size());
            contacts.push_back(c);
            asVertex[c.v].push_back(idx);
            for (int k = 0; k < 3; ++k)
                asTriangle[tris[t][k]].push_back(idx);
            nearV[c.v] = std::min(nearV[c.v], c.dist);
            nearT[t] = std::min(nearT[t], c.dist);
        }
    for (int i = 0; i < n; ++i)
    {
        float d = nearV[i];
        for (const Ref& r : vTris[i])
            d = std::min(d, nearT[r.index]);
        // A small floor keeps touching vertices from locking in place.
        bound[i] = std::max(kBoundFraction * d, 0.02f * R);
    }
}

glm::vec3 ClothSolver::Impl::solveVertex(int i, const std::vector<Pin>& pins,
                                         const std::vector<const Sdf*>& colliders) const
{
    using namespace vbd;
    const vec3 xi = x[i];
    const float h2 = h * h;
    const float inertia = mass[i] / h2;
    vec3 f = -inertia * (xi - y[i]);
    mat3 H(inertia);

    vec3 fe(0.f);
    mat3 He(0.f), Hg(0.f);
    for (const Ref& r : vTris[i])
    {
        const Membrane& m = membranes[r.index];
        if (m.area <= 0.f)
            continue;
        const auto& t = tris[r.index];
        const vec3 x0 = x[t[0]], e1 = x[t[1]] - x0, e2 = x[t[2]] - x0;
        const vec3 f0 = e1 * m.b00 + e2 * m.b10, f1 = e1 * m.b01 + e2 * m.b11;
        const float E00 = 0.5f * (glm::dot(f0, f0) - 1.f), E11 = 0.5f * (glm::dot(f1, f1) - 1.f),
                    E01 = 0.5f * glm::dot(f0, f1);
        const float S00 = m.kWarp * E00, S11 = m.kWeft * E11, S01 = 2.f * m.kShear * E01;
        float c0, c1;
        if (r.role == 1)
            c0 = m.b00, c1 = m.b01;
        else if (r.role == 2)
            c0 = m.b10, c1 = m.b11;
        else
            c0 = -(m.b00 + m.b10), c1 = -(m.b01 + m.b11);
        fe -= m.area * (f0 * (S00 * c0 + S01 * c1) + f1 * (S01 * c0 + S11 * c1));
        const vec3 w = c1 * f0 + c0 * f1;
        const float geometric = std::max(0.f, S00 * c0 * c0 + 2.f * S01 * c0 * c1 + S11 * c1 * c1);
        He += m.area * (m.kWarp * c0 * c0 * outer(f0, f0) + m.kWeft * c1 * c1 * outer(f1, f1) +
                        m.kShear * outer(w, w));
        Hg += mat3(m.area * geometric);
    }
    for (const Ref& r : vHinges[i])
    {
        const Hinge& hg = hinges[r.index];
        float theta = 0.f;
        vec3 grad[4];
        if (!dihedral(x[hg.v[0]], x[hg.v[1]], x[hg.v[2]], x[hg.v[3]], theta, grad))
            continue;
        const vec3& gi = grad[r.role];
        fe -= hg.stiffness * wrapAngle(theta - hg.restAngle) * gi;
        He += hg.stiffness * outer(gi, gi);
    }
    // Rayleigh damping on the elastic terms, against the motion since the substep started.
    const float kd = damping[i] * kDampingScale / h;
    fe -= kd * (He * (xi - xStart[i]));
    He *= 1.f + kd;
    f += fe;
    H += He + Hg;

    for (const Ref& r : vLinks[i])
    {
        const Link& l = links[r.index];
        const float sign = r.role == 0 ? 1.f : -1.f;
        const vec3 d = x[l.a] - x[l.b];
        if (l.closed)
        {
            f -= sign * (l.k * d + l.lambda);
            H += mat3(l.k);
            continue;
        }
        const float len = glm::length(d);
        if (len < 1e-9f)
            continue;
        const float F = l.k * (len - l.limit) + l.lambda.x;
        if (F <= 0.f)
            continue;
        const vec3 nd = d / len;
        f -= sign * F * nd;
        H += l.k * outer(nd, nd) + (F / len) * (mat3(1.f) - outer(nd, nd));
    }
    if (pinOf[i] >= 0)
    {
        const PinState& ps = pinStates[pinOf[i]];
        f -= ps.k * (xi - toVec(pins[pinOf[i]].target)) + ps.lambda;
        H += mat3(ps.k);
    }

    float pen;
    vec3 nrm;
    if (bodyContact(i, xi, colliders, pen, nrm))
    {
        const float kc = bodyK[i];
        const float fn = std::max(0.f, kc * pen + bodyLambda[i]);
        f += fn * nrm;
        const mat3 nn = outer(nrm, nrm);
        H += kc * nn;
        // Lagged friction against the tangential slip this substep, sticking below kFrictionSlip.
        const vec3 slip = xi - xStart[i];
        const vec3 u = slip - nrm * glm::dot(nrm, slip);
        const float scale = friction[i] * fn / std::max(glm::length(u), kFrictionSlip);
        f -= scale * u;
        H += scale * (mat3(1.f) - nn);
    }

    for (int ci : asVertex[i])
    {
        const Contact& c = contacts[ci];
        vec3 bary;
        if (!contactGap(c, pen, nrm, bary))
            continue;
        const float kc = contactStiffness(c.v);
        f += kc * pen * nrm;
        H += kc * outer(nrm, nrm);
    }
    for (int ci : asTriangle[i])
    {
        const Contact& c = contacts[ci];
        vec3 bary;
        if (!contactGap(c, pen, nrm, bary))
            continue;
        const auto& t = tris[c.t];
        const float w = t[0] == i ? bary.x : t[1] == i ? bary.y : bary.z;
        const float kc = contactStiffness(c.v);
        f -= kc * pen * w * nrm;
        H += kc * w * w * outer(nrm, nrm);
    }

    const float det = glm::determinant(H);
    if (!(std::abs(det) > 1e-30f) || !std::isfinite(det))
        return xi;
    const vec3 dx = glm::inverse(H) * f;
    if (!std::isfinite(dx.x) || !std::isfinite(dx.y) || !std::isfinite(dx.z))
        return xi;
    return xi + dx;
}

bool ClothSolver::Impl::bodyContact(int i, const vec3& p, const std::vector<const Sdf*>& colliders, float& pen,
                                    vec3& nrm) const
{
    pen = 0.f;
    const float r = s.collisionThickness + 0.5f * thickness[i];
    for (const Sdf* sdf : colliders)
    {
        if (!sdf)
            continue;
        Vec3 grad;
        const float phi = sdf->sample(vbd::toArr(p), &grad);
        if (phi >= r || r - phi <= pen)
            continue;
        const vec3 g = vbd::toVec(grad);
        const float gl = glm::length(g);
        if (gl < 1e-8f)
            continue;
        pen = r - phi;
        nrm = g / gl;
    }
    if (const float floorPen = r - (p.y - s.floorY); floorPen > pen)
    {
        pen = floorPen;
        nrm = vec3(0.f, 1.f, 0.f);
    }
    return pen > 0.f;
}

void ClothSolver::Impl::updateDuals(const std::vector<Pin>& pins, const std::vector<const Sdf*>& colliders)
{
    using namespace vbd;
    if (!colliders.empty() || s.floorY > -1e29f)
        tbb::parallel_for(tbb::blocked_range<int>(0, n, 256), [&](const tbb::blocked_range<int>& r) {
            for (int i = r.begin(); i != r.end(); ++i)
            {
                float pen;
                vec3 nrm;
                if (!movable[i] || !bodyContact(i, x[i], colliders, pen, nrm))
                {
                    bodyLambda[i] = 0.f;
                    continue;
                }
                bodyLambda[i] = std::max(0.f, bodyK[i] * pen + bodyLambda[i]);
                bodyK[i] = std::min(bodyK[i] + kConstraintBeta * pen, kConstraintKMax);
            }
        });
    for (Link& l : links)
    {
        const vec3 d = x[l.a] - x[l.b];
        if (l.closed)
        {
            l.lambda += l.k * d;
            l.k = std::min(l.k + kConstraintBeta * glm::length(d), kConstraintKMax);
            continue;
        }
        const float C = glm::length(d) - l.limit;
        const float F = l.k * C + l.lambda.x;
        if (F > 0.f)
        {
            l.lambda.x = F;
            l.k = std::min(l.k + kConstraintBeta * std::abs(C), kConstraintKMax);
        }
        else
            l.lambda.x = 0.f;
    }
    for (size_t p = 0; p < pins.size() && p < pinStates.size(); ++p)
    {
        const int vi = pins[p].vertex;
        if (vi < 0 || vi >= n || pinOf[vi] != int(p))
            continue;
        PinState& ps = pinStates[p];
        const vec3 C = x[vi] - toVec(pins[p].target);
        ps.lambda += ps.k * C;
        ps.k = std::min(ps.k + kConstraintBeta * glm::length(C), kConstraintKMax);
    }
}

void ClothSolver::Impl::substep(const std::vector<Pin>& pins, const std::vector<const Sdf*>& colliders)
{
    using namespace vbd;
    const vec3 gravity = toVec(s.gravity);
    xStart = x;
    for (int i = 0; i < n; ++i)
        y[i] = movable[i] ? x[i] + h * v[i] + h * h * gravity : x[i];
    for (Link& l : links)
    {
        l.lambda *= kLambdaDecay;
        l.k = std::max(l.kStart, l.k * kStiffnessDecay);
    }
    for (PinState& ps : pinStates)
    {
        ps.lambda *= kLambdaDecay;
        ps.k = std::max(ps.kStart, ps.k * kStiffnessDecay);
    }
    for (int i = 0; i < n; ++i)
    {
        bodyLambda[i] *= kLambdaDecay;
        bodyK[i] = std::max(contactStiffness(i), bodyK[i] * kStiffnessDecay);
    }

    detect();
    for (int i = 0; i < n; ++i)
        if (movable[i])
            x[i] = clampToBound(i, y[i]);

    for (int it = 0; it < s.iterations; ++it)
    {
        std::fill(clamped.begin(), clamped.end(), 0);
        for (const std::vector<int>& colour : colours)
        {
            // Same-colour vertices only meet through contacts; buffering keeps those Jacobi and the result deterministic.
            tbb::parallel_for(tbb::blocked_range<size_t>(0, colour.size(), 64), [&](const tbb::blocked_range<size_t>& r) {
                for (size_t k = r.begin(); k != r.end(); ++k)
                {
                    const int i = colour[k];
                    xNew[i] = clampToBound(i, solveVertex(i, pins, colliders));
                }
            });
            for (int i : colour)
                x[i] = xNew[i];
        }
        updateDuals(pins, colliders);
        if (s.selfCollision && it % kRedetectEvery == kRedetectEvery - 1 && it + 1 < s.iterations &&
            std::find(clamped.begin(), clamped.end(), uint8_t(1)) != clamped.end())
            detect();
    }

    const float drag = 1.f / (1.f + kAirDrag * h);
    for (int i = 0; i < n; ++i)
        v[i] = movable[i] ? (x[i] - xStart[i]) * (drag / h) : vec3(0.f);
}

void ClothSolver::reset(const ClothMesh& mesh, const Garment& g, const SolverSettings& s)
{
    impl_ = std::make_shared<Impl>();
    Impl& m = *impl_;
    m.s = s;
    m.s.substeps = std::max(1, s.substeps);
    m.s.iterations = std::max(1, s.iterations);
    m.h = std::max(s.dt, 1e-6f) / float(m.s.substeps);
    x_ = mesh.rest3d;
    v_.assign(x_.size(), Vec3{0.f, 0.f, 0.f});
    xPrev_ = x_;
    m.build(mesh, g);
    m.x.resize(m.n);
    m.v.assign(m.n, glm::vec3(0.f));
    for (int i = 0; i < m.n; ++i)
        m.x[i] = vbd::toVec(x_[i]);
}

void ClothSolver::step()
{
    if (!impl_ || int(x_.size()) != impl_->n)
        return;
    Impl& m = *impl_;
    v_.resize(x_.size(), Vec3{0.f, 0.f, 0.f});
    for (int i = 0; i < m.n; ++i)
    {
        m.x[i] = vbd::toVec(x_[i]);
        m.v[i] = vbd::toVec(v_[i]);
    }
    xPrev_ = x_;
    m.syncPins(pins_);
    m.advanceSeams();
    for (int sub = 0; sub < m.s.substeps; ++sub)
        m.substep(pins_, colliders_);
    for (int i = 0; i < m.n; ++i)
    {
        x_[i] = vbd::toArr(m.x[i]);
        v_[i] = vbd::toArr(m.v[i]);
    }
}

std::vector<float> ClothSolver::strainMap() const
{
    std::vector<float> out(x_.size(), 1.f);
    if (!impl_ || int(x_.size()) != impl_->n)
        return out;
    const Impl& m = *impl_;
    std::vector<float> sum(out.size(), 0.f), count(out.size(), 0.f);
    for (size_t t = 0; t < m.tris.size(); ++t)
    {
        const vbd::Membrane& mb = m.membranes[t];
        if (mb.area <= 0.f)
            continue;
        const auto& tri = m.tris[t];
        const glm::vec3 x0 = vbd::toVec(x_[tri[0]]), e1 = vbd::toVec(x_[tri[1]]) - x0, e2 = vbd::toVec(x_[tri[2]]) - x0;
        const glm::vec3 f0 = e1 * mb.b00 + e2 * mb.b10, f1 = e1 * mb.b01 + e2 * mb.b11;
        const float a = glm::dot(f0, f0), b = glm::dot(f0, f1), c = glm::dot(f1, f1);
        const float half = 0.5f * (a + c);
        const float stretch = std::sqrt(std::max(0.f, half + std::sqrt(std::max(0.f, half * half - (a * c - b * b)))));
        for (int k = 0; k < 3; ++k)
        {
            sum[tri[k]] += stretch;
            count[tri[k]] += 1.f;
        }
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (count[i] > 0.f)
            out[i] = sum[i] / count[i];
    return out;
}

float Sdf::sample(const Vec3& p, Vec3* gradient) const
{
    if (nx < 2 || ny < 2 || nz < 2 || cell <= 0.f || values.size() < size_t(nx) * ny * nz)
    {
        if (gradient)
            *gradient = {0.f, 1.f, 0.f};
        return vbd::kInf;
    }
    const int dims[3] = {nx, ny, nz};
    int i0[3];
    float fr[3];
    float outside2 = 0.f;
    for (int a = 0; a < 3; ++a)
    {
        const float u = (p[a] - origin[a]) / cell;
        const float uc = std::clamp(u, 0.f, float(dims[a] - 1));
        outside2 += (u - uc) * (u - uc) * cell * cell;
        i0[a] = std::min(int(uc), dims[a] - 2);
        fr[a] = uc - float(i0[a]);
    }
    auto at = [&](int di, int dj, int dk) {
        return values[(size_t(i0[2] + dk) * ny + size_t(i0[1] + dj)) * nx + size_t(i0[0] + di)];
    };
    const float c000 = at(0, 0, 0), c100 = at(1, 0, 0), c010 = at(0, 1, 0), c110 = at(1, 1, 0);
    const float c001 = at(0, 0, 1), c101 = at(1, 0, 1), c011 = at(0, 1, 1), c111 = at(1, 1, 1);
    const float fx = fr[0], fy = fr[1], fz = fr[2];
    const float c00 = c000 + (c100 - c000) * fx, c10 = c010 + (c110 - c010) * fx;
    const float c01 = c001 + (c101 - c001) * fx, c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy, c1 = c01 + (c11 - c01) * fy;
    const float value = c0 + (c1 - c0) * fz;
    if (gradient)
    {
        const float gx = ((c100 - c000) * (1 - fy) * (1 - fz) + (c110 - c010) * fy * (1 - fz) +
                          (c101 - c001) * (1 - fy) * fz + (c111 - c011) * fy * fz);
        const float gy = (c10 - c00) * (1 - fz) + (c11 - c01) * fz;
        const float gz = c1 - c0;
        *gradient = {gx / cell, gy / cell, gz / cell};
    }
    return value + std::sqrt(outside2);
}

Sdf sdfFromMesh(const std::vector<Vec3>& points, const std::vector<std::array<int, 3>>& tris, float cell, float padding)
{
    using glm::vec3;
    Sdf out;
    if (points.empty() || cell <= 0.f)
        return out;
    vec3 lo(vbd::kInf), hi(-vbd::kInf);
    for (const Vec3& p : points)
    {
        lo = glm::min(lo, vbd::toVec(p));
        hi = glm::max(hi, vbd::toVec(p));
    }
    lo -= vec3(padding);
    hi += vec3(padding);
    out.origin = vbd::toArr(lo);
    out.cell = cell;
    out.nx = std::max(2, int(std::ceil((hi.x - lo.x) / cell)) + 1);
    out.ny = std::max(2, int(std::ceil((hi.y - lo.y) / cell)) + 1);
    out.nz = std::max(2, int(std::ceil((hi.z - lo.z) / cell)) + 1);
    const int dims[3] = {out.nx, out.ny, out.nz};
    const size_t total = size_t(out.nx) * out.ny * out.nz;
    auto index = [&](int i, int j, int k) { return (size_t(k) * out.ny + size_t(j)) * out.nx + size_t(i); };
    auto node = [&](int i, int j, int k) { return lo + vec3(float(i), float(j), float(k)) * cell; };

    std::vector<std::array<int, 3>> valid;
    for (const auto& t : tris)
        if (t[0] >= 0 && t[1] >= 0 && t[2] >= 0 && t[0] < int(points.size()) && t[1] < int(points.size()) &&
            t[2] < int(points.size()))
            valid.push_back(t);
    if (valid.empty())
    {
        out.values.assign(total, cell * float(out.nx + out.ny + out.nz));
        return out;
    }

    std::vector<float> dist(total, vbd::kInf);
    std::vector<int> nearest(total, -1);
    auto triDistance = [&](int t, const vec3& p) {
        vec3 bary;
        const auto& tri = valid[t];
        return glm::length(p - vbd::closestOnTriangle(p, vbd::toVec(points[tri[0]]), vbd::toVec(points[tri[1]]),
                                                         vbd::toVec(points[tri[2]]), bary));
    };

    // Exact distances in a narrow band around each triangle, then closest-triangle propagation by fast sweeping.
    for (int t = 0; t < int(valid.size()); ++t)
    {
        const vec3 a = vbd::toVec(points[valid[t][0]]), b = vbd::toVec(points[valid[t][1]]),
                   c = vbd::toVec(points[valid[t][2]]);
        const vec3 tlo = (glm::min(a, glm::min(b, c)) - lo) / cell, thi = (glm::max(a, glm::max(b, c)) - lo) / cell;
        int r0[3], r1[3];
        for (int ax = 0; ax < 3; ++ax)
        {
            r0[ax] = std::clamp(int(std::floor(tlo[ax])) - 1, 0, dims[ax] - 1);
            r1[ax] = std::clamp(int(std::ceil(thi[ax])) + 1, 0, dims[ax] - 1);
        }
        for (int k = r0[2]; k <= r1[2]; ++k)
            for (int j = r0[1]; j <= r1[1]; ++j)
                for (int i = r0[0]; i <= r1[0]; ++i)
                {
                    const size_t id = index(i, j, k);
                    const float d = triDistance(t, node(i, j, k));
                    if (d < dist[id])
                    {
                        dist[id] = d;
                        nearest[id] = t;
                    }
                }
    }
    for (int pass = 0; pass < 8; ++pass)
    {
        const int di = pass & 1 ? -1 : 1, dj = pass & 2 ? -1 : 1, dk = pass & 4 ? -1 : 1;
        for (int kk = 0; kk < out.nz; ++kk)
        {
            const int k = dk > 0 ? kk : out.nz - 1 - kk;
            for (int jj = 0; jj < out.ny; ++jj)
            {
                const int j = dj > 0 ? jj : out.ny - 1 - jj;
                for (int ii = 0; ii < out.nx; ++ii)
                {
                    const int i = di > 0 ? ii : out.nx - 1 - ii;
                    const size_t id = index(i, j, k);
                    const int nbs[3][3] = {{i - di, j, k}, {i, j - dj, k}, {i, j, k - dk}};
                    for (const auto& nb : nbs)
                    {
                        if (nb[0] < 0 || nb[1] < 0 || nb[2] < 0 || nb[0] >= out.nx || nb[1] >= out.ny ||
                            nb[2] >= out.nz)
                            continue;
                        const int t = nearest[index(nb[0], nb[1], nb[2])];
                        if (t < 0 || t == nearest[id])
                            continue;
                        const float d = triDistance(t, node(i, j, k));
                        if (d < dist[id])
                        {
                            dist[id] = d;
                            nearest[id] = t;
                        }
                    }
                }
            }
        }
    }

    // Inside test: ray parity along each axis, majority of three, so small holes in the mesh don't flip whole regions.
    std::vector<uint8_t> votes(total, 0);
    for (int axis = 0; axis < 3; ++axis)
    {
        const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
        // Odd offsets keep rays off mesh edges and vertices that sit exactly on grid lines.
        const double off1 = 1.234567e-4 * cell, off2 = 2.345678e-4 * cell;
        std::vector<std::vector<float>> hits(size_t(dims[a1]) * dims[a2]);
        for (const auto& tri : valid)
        {
            glm::dvec3 p[3];
            for (int k = 0; k < 3; ++k)
                p[k] = glm::dvec3(vbd::toVec(points[tri[k]]));
            const double min1 = std::min({p[0][a1], p[1][a1], p[2][a1]}), max1 = std::max({p[0][a1], p[1][a1], p[2][a1]});
            const double min2 = std::min({p[0][a2], p[1][a2], p[2][a2]}), max2 = std::max({p[0][a2], p[1][a2], p[2][a2]});
            const int j0 = std::max(0, int(std::ceil((min1 - lo[a1] - off1) / cell)));
            const int j1 = std::min(dims[a1] - 1, int(std::floor((max1 - lo[a1] - off1) / cell)));
            const int k0 = std::max(0, int(std::ceil((min2 - lo[a2] - off2) / cell)));
            const int k1 = std::min(dims[a2] - 1, int(std::floor((max2 - lo[a2] - off2) / cell)));
            for (int j = j0; j <= j1; ++j)
                for (int k = k0; k <= k1; ++k)
                {
                    const double q1 = lo[a1] + j * double(cell) + off1, q2 = lo[a2] + k * double(cell) + off2;
                    auto cross2 = [&](const glm::dvec3& u, const glm::dvec3& w) {
                        return (u[a1] - q1) * (w[a2] - q2) - (u[a2] - q2) * (w[a1] - q1);
                    };
                    const double w0 = cross2(p[1], p[2]), w1 = cross2(p[2], p[0]), w2 = cross2(p[0], p[1]);
                    if (!((w0 > 0 && w1 > 0 && w2 > 0) || (w0 < 0 && w1 < 0 && w2 < 0)))
                        continue;
                    const double along = (w0 * p[0][axis] + w1 * p[1][axis] + w2 * p[2][axis]) / (w0 + w1 + w2);
                    hits[size_t(j) * dims[a2] + k].push_back(float((along - lo[axis]) / cell));
                }
        }
        for (int j = 0; j < dims[a1]; ++j)
            for (int k = 0; k < dims[a2]; ++k)
            {
                std::vector<float>& line = hits[size_t(j) * dims[a2] + k];
                std::sort(line.begin(), line.end());
                size_t crossed = 0;
                for (int i = 0; i < dims[axis]; ++i)
                {
                    while (crossed < line.size() && line[crossed] < float(i))
                        ++crossed;
                    if (crossed % 2 == 0)
                        continue;
                    int c[3];
                    c[axis] = i;
                    c[a1] = j;
                    c[a2] = k;
                    ++votes[index(c[0], c[1], c[2])];
                }
            }
    }

    out.values.resize(total);
    for (size_t id = 0; id < total; ++id)
        out.values[id] = votes[id] >= 2 ? -dist[id] : dist[id];
    return out;
}

} // namespace tailor
