#include "ClothGpu.h"

// TODO(sdk): the Lila SDK (sdk/include/lila) has no GPU compute surface, so this file is written against Lila's
// internal render API (render/Compute.h, render/Renderer.h, render/VulkanContext.h), VMA and raw Vulkan. Tailor
// cannot build as an SDK-only extension until that surface exists.
#include "render/Compute.h"
#include "render/Renderer.h"
#include "render/VulkanContext.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <thread>

namespace tailor
{
namespace
{
namespace rc = lila::render::compute;
using lila::render::Renderer;
using lila::render::VulkanContext;

using F4 = std::array<float, 4>;

constexpr uint32_t kGroup = 64;
constexpr uint32_t kMaxColliders = 4;
constexpr uint32_t kMaxVt = 16; // must match cloth_vbd.slang
constexpr uint32_t kMaxTv = 32;
constexpr uint32_t kMovable = 1;
constexpr uint32_t kSeam = 0;
constexpr uint32_t kStrain = 1;
constexpr uint32_t kMaxIndirectItems = 65535u * kGroup;
constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

struct GpuTri
{
    uint32_t v[4];
    float b[4]; // b00, b01, b10, b11
    float rest[4]; // area, warp, weft, shear
};
struct GpuHinge
{
    uint32_t v[4];
    float p[4];
};
struct GpuLink
{
    uint32_t v[4]; // a, b, kind, closed
    float p[4];    // limit, rest, kStart
};

enum Buf
{
    BX,
    BXStart,
    BXNew,
    BXDetect,
    BY,
    BVel,
    BVInfo,
    BVExtra,
    BPin,
    BPinDual,
    BTriOff,
    BTriAdj,
    BHingeOff,
    BHingeAdj,
    BLinkOff,
    BLinkAdj,
    BColour,
    BWeld,
    BTris,
    BHinges,
    BLinks,
    BLinkDual,
    BColliders,
    BCellHead,
    BCellNext,
    BVtCount,
    BVt,
    BTvCount,
    BTv,
    BDminV,
    BDminT,
    BBound,
    BState,
    BReserved,
    BufCount
};

/** Mirrors ClothScene in cloth_vbd.slang. */
struct GpuScene
{
    VkDeviceAddress ptr[BufCount];
    uint32_t nVerts, nTris, nHinges, nLinks;
    uint32_t nColliders, hashMask, selfCollision, nMovable;
    float h, lambdaDecay, stiffnessDecay, beta;
    float kMax, thickness, queryRadius, cellSize;
    float boundFraction, frictionSlip, drag, seamKeep;
    float closedGap, pad0, pad1, pad2;
    float gravity[4];
};
static_assert(BufCount == 34);
static_assert(offsetof(GpuScene, gravity) == 368 && sizeof(GpuScene) == 384);

struct ClothPush
{
    VkDeviceAddress scene;
    uint32_t first, count, pass, pad;
};
static_assert(sizeof(ClothPush) == 24);

/** state[]: the clamp flag, then 4 VkDispatchIndirectCommand for re-detection from word 4. */
constexpr uint32_t kStateWords = 4 + 4 * 3;

enum Kernel
{
    KAdvanceSeams,
    KBeginStep,
    KBeginLinks,
    KInitGuess,
    KSolve,
    KCommit,
    KDualLinks,
    KDualPins,
    KEndStep,
    KRedetectArgs,
    KHashClear,
    KHashInsert,
    KHashQuery,
    KContactFinalize,
    KernelCount
};
constexpr const char* kEntries[KernelCount] = {"advanceSeamsCS", "beginStepCS", "beginLinksCS",   "initGuessCS", "solveCS",
                                               "commitCS",       "dualLinksCS", "dualPinsCS",     "endStepCS",   "redetectArgsCS",
                                               "hashClearCS",    "hashInsertCS", "hashQueryCS",   "contactFinalizeCS"};

float bitsToFloat(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

using D3 = std::array<double, 3>;
D3 toD(const Vec3& a) { return {a[0], a[1], a[2]}; }
D3 d3Sub(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 d3Cross(const D3& a, const D3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double d3Dot(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
double d3Len(const D3& a) { return std::sqrt(d3Dot(a, a)); }

/** ClothSolver's dihedral angle across ab between abc and abd; 0 when degenerate. */
float dihedralAngle(const Vec3& pa, const Vec3& pb, const Vec3& pc, const Vec3& pd)
{
    const D3 a = toD(pa), e = d3Sub(toD(pb), a);
    const double el = d3Len(e);
    const D3 n1 = d3Cross(e, d3Sub(toD(pc), a)), n2 = d3Cross(d3Sub(toD(pd), a), e);
    const double l1 = d3Len(n1), l2 = d3Len(n2);
    if (el < 1e-8 || l1 < 1e-12 || l2 < 1e-12)
        return 0.f;
    const D3 u1{n1[0] / l1, n1[1] / l1, n1[2] / l1}, u2{n2[0] / l2, n2[1] / l2, n2[2] / l2};
    return float(std::atan2(d3Dot(d3Cross(u1, u2), e) / el, d3Dot(u1, u2)));
}

void toCsr(const std::vector<std::vector<uint32_t>>& lists, std::vector<uint32_t>& off, std::vector<uint32_t>& adj)
{
    off.assign(lists.size() + 1, 0);
    for (size_t i = 0; i < lists.size(); ++i)
        off[i + 1] = off[i] + uint32_t(lists[i].size());
    adj.clear();
    adj.reserve(off.back());
    for (const auto& l : lists)
        adj.insert(adj.end(), l.begin(), l.end());
}

int weldRoot(std::vector<int>& parent, int i)
{
    while (parent[size_t(i)] != i)
    {
        parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        i = parent[size_t(i)];
    }
    return i;
}

/** Everything on the GPU that stays fixed between resets, built on the caller's thread exactly as ClothSolver builds it. */
struct Topology
{
    uint32_t n = 0;
    std::vector<F4> x, vinfo, vextra;
    std::vector<float> mass;
    std::vector<uint8_t> movable;
    std::vector<GpuTri> tris;
    std::vector<GpuHinge> hinges;
    std::vector<GpuLink> links;
    std::vector<uint32_t> triOff, triAdj, hingeOff, hingeAdj, linkOff, linkAdj, weld;
    std::vector<uint32_t> colour, colourOff;
    float queryRadius = 0.01f;
    float cellSize = 0.02f;
};

Topology buildTopology(const ClothMesh& mesh, const Garment& g, const SolverSettings& s, const GpuTuning& tune, float h)
{
    Topology t;
    const int n = int(mesh.rest3d.size());
    t.n = uint32_t(n);
    static const Fabric kDefaultFabric;
    auto panelOf = [&](int p) -> const Panel* { return p >= 0 && p < int(g.panels.size()) ? &g.panels[size_t(p)] : nullptr; };
    auto vertPanel = [&](int i) { return i < int(mesh.vertPanel.size()) ? mesh.vertPanel[size_t(i)] : 0; };
    auto fabricOf = [&](const Panel* p) -> const Fabric& {
        if (p && p->fabric >= 0 && p->fabric < int(g.fabrics.size()))
            return g.fabrics[size_t(p->fabric)];
        return g.fabrics.empty() ? kDefaultFabric : g.fabrics.front();
    };

    t.mass.assign(size_t(n), 0.f);
    t.movable.assign(size_t(n), 0);
    std::vector<float> thickness(size_t(n), 0.f), friction(size_t(n), 0.f), damping(size_t(n), 0.f);
    for (int i = 0; i < n; ++i)
    {
        const Panel* p = panelOf(vertPanel(i));
        const Fabric& f = fabricOf(p);
        const PanelState state = p ? p->state : PanelState::Active;
        damping[size_t(i)] = f.damping;
        friction[size_t(i)] = f.friction;
        thickness[size_t(i)] = f.thickness;
        t.movable[size_t(i)] = state == PanelState::Active || state == PanelState::Strengthened;
    }

    const bool hasUv = mesh.uv.size() == mesh.rest3d.size();
    std::vector<std::array<int, 3>> tris;
    std::vector<int> triPanel;
    for (size_t f = 0; f < mesh.tris.size(); ++f)
    {
        const auto& tri = mesh.tris[f];
        bool ok = true;
        for (int k = 0; k < 3; ++k)
            ok = ok && tri[size_t(k)] >= 0 && tri[size_t(k)] < n;
        if (!ok || tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
            continue;
        tris.push_back(tri);
        triPanel.push_back(f < mesh.triPanel.size() ? mesh.triPanel[f] : vertPanel(tri[0]));
    }
    const int nt = int(tris.size());

    // Rest corners in the grain frame: warp along x, scaled by the fabric's shrink.
    using V2 = std::array<float, 2>;
    std::vector<std::array<V2, 3>> restCorners(size_t(nt));
    std::vector<std::vector<uint32_t>> vTri(size_t(n)), vHinge(size_t(n)), vLink(size_t(n));
    t.tris.assign(size_t(nt), GpuTri{});
    for (int f = 0; f < nt; ++f)
    {
        const auto& tri = tris[size_t(f)];
        const Panel* p = panelOf(triPanel[size_t(f)]);
        const Fabric& fab = fabricOf(p);
        auto& rcn = restCorners[size_t(f)];
        if (hasUv)
        {
            const float ga = p ? p->grainAngle : 0.f;
            const float wx = std::cos(ga), wy = std::sin(ga);
            for (int k = 0; k < 3; ++k)
            {
                const Vec2& u = mesh.uv[size_t(tri[size_t(k)])];
                rcn[size_t(k)] = {(u[0] * wx + u[1] * wy) * fab.shrinkWarp, (-u[0] * wy + u[1] * wx) * fab.shrinkWeft};
            }
        }
        else
        {
            const D3 r0 = toD(mesh.rest3d[size_t(tri[0])]);
            const D3 e1 = d3Sub(toD(mesh.rest3d[size_t(tri[1])]), r0), e2 = d3Sub(toD(mesh.rest3d[size_t(tri[2])]), r0);
            const double l1 = d3Len(e1);
            const D3 nrm = d3Cross(e1, e2);
            if (l1 > 1e-12 && d3Len(nrm) > 1e-20)
            {
                const D3 ax{e1[0] / l1, e1[1] / l1, e1[2] / l1};
                D3 ay = d3Cross(nrm, ax);
                const double la = d3Len(ay);
                ay = {ay[0] / la, ay[1] / la, ay[2] / la};
                rcn = {V2{0.f, 0.f}, V2{float(l1), 0.f}, V2{float(d3Dot(e2, ax)), float(d3Dot(e2, ay))}};
            }
            else
                rcn = {V2{0.f, 0.f}, V2{0.f, 0.f}, V2{0.f, 0.f}};
        }
        const float d1x = rcn[1][0] - rcn[0][0], d1y = rcn[1][1] - rcn[0][1];
        const float d2x = rcn[2][0] - rcn[0][0], d2y = rcn[2][1] - rcn[0][1];
        const float det = d1x * d2y - d2x * d1y;
        GpuTri& gt = t.tris[size_t(f)];
        for (int k = 0; k < 3; ++k)
            gt.v[k] = uint32_t(tri[size_t(k)]);
        if (std::abs(det) > 1e-14f)
        {
            gt.b[0] = d2y / det;
            gt.b[1] = -d2x / det;
            gt.b[2] = -d1y / det;
            gt.b[3] = d1x / det;
            const float area = 0.5f * std::abs(det);
            const float mult = p && p->state == PanelState::Strengthened ? tune.strengthen : 1.f;
            gt.rest[0] = area;
            gt.rest[1] = fab.stretchWarp * mult;
            gt.rest[2] = fab.stretchWeft * mult;
            gt.rest[3] = fab.shear * mult;
            for (int k = 0; k < 3; ++k)
                t.mass[size_t(tri[size_t(k)])] += fab.density * area / 3.f;
        }
        for (uint32_t k = 0; k < 3; ++k)
            vTri[size_t(tri[k])].push_back(uint32_t(f) << 2 | k);
    }
    for (int i = 0; i < n; ++i)
        t.movable[size_t(i)] = t.movable[size_t(i)] && t.mass[size_t(i)] > 0.f;

    struct EdgeRec
    {
        uint64_t key;
        int tri;
        int local;
    };
    std::vector<EdgeRec> edges;
    edges.reserve(size_t(nt) * 3);
    for (int f = 0; f < nt; ++f)
        for (int k = 0; k < 3; ++k)
        {
            const int a = tris[size_t(f)][size_t(k)], b = tris[size_t(f)][size_t((k + 1) % 3)];
            edges.push_back({(uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b)), f, k});
        }
    std::sort(edges.begin(), edges.end(), [](const EdgeRec& l, const EdgeRec& r) { return l.key != r.key ? l.key < r.key : l.tri < r.tri; });
    std::vector<uint64_t> creases;
    for (const auto& ce : mesh.creaseEdges)
        creases.push_back((uint64_t(std::min(ce[0], ce[1])) << 32) | uint64_t(std::max(ce[0], ce[1])));
    std::sort(creases.begin(), creases.end());

    const float h2 = h * h;
    auto addLink = [&](int a, int b, uint32_t kind, float limit, float rest, bool closed) {
        GpuLink l{};
        l.v[0] = uint32_t(a);
        l.v[1] = uint32_t(b);
        l.v[2] = kind;
        l.v[3] = closed ? 1u : 0u;
        l.p[0] = limit;
        l.p[1] = rest;
        l.p[2] = std::max(1.f, 10.f * 0.5f * (t.mass[size_t(a)] + t.mass[size_t(b)]) / h2);
        t.links.push_back(l);
    };

    double edgeSum = 0.0;
    size_t edgeCount = 0;
    for (size_t e = 0; e < edges.size();)
    {
        size_t end = e;
        while (end < edges.size() && edges[end].key == edges[e].key)
            ++end;
        const EdgeRec& r1 = edges[e];
        const auto& t1 = tris[size_t(r1.tri)];
        const int a = t1[size_t(r1.local)], b = t1[size_t((r1.local + 1) % 3)];
        const Panel* p1 = panelOf(triPanel[size_t(r1.tri)]);
        const Fabric& f1 = fabricOf(p1);
        const auto& rc1 = restCorners[size_t(r1.tri)];
        const float rex = rc1[size_t((r1.local + 1) % 3)][0] - rc1[size_t(r1.local)][0];
        const float rey = rc1[size_t((r1.local + 1) % 3)][1] - rc1[size_t(r1.local)][1];
        const float restLen = std::sqrt(rex * rex + rey * rey);
        edgeSum += d3Len(d3Sub(toD(mesh.rest3d[size_t(a)]), toD(mesh.rest3d[size_t(b)])));
        ++edgeCount;

        if (end - e == 2 && restLen > 1e-9f)
        {
            const EdgeRec& r2 = edges[e + 1];
            GpuHinge hg{};
            hg.v[0] = uint32_t(a);
            hg.v[1] = uint32_t(b);
            hg.v[2] = uint32_t(t1[size_t((r1.local + 2) % 3)]);
            hg.v[3] = uint32_t(tris[size_t(r2.tri)][size_t((r2.local + 2) % 3)]);
            float kBend = 0.5f * (f1.bendWarp + f1.bendWeft);
            if (hasUv)
            {
                const float c2 = rex * rex / (restLen * restLen), s2 = 1.f - c2;
                kBend = f1.bendWeft * c2 * c2 + f1.bendWarp * s2 * s2 + 2.f * f1.bendBias * c2 * s2;
            }
            if (p1 && p1->state == PanelState::Strengthened)
                kBend *= tune.strengthen;
            const float areas = t.tris[size_t(r1.tri)].rest[0] + t.tris[size_t(r2.tri)].rest[0];
            if (areas > 1e-14f)
            {
                hg.p[1] = 6.f * kBend * restLen * restLen / areas;
                const bool crease = std::binary_search(creases.begin(), creases.end(), r1.key);
                if (crease || !hasUv)
                    hg.p[0] = dihedralAngle(mesh.rest3d[hg.v[0]], mesh.rest3d[hg.v[1]], mesh.rest3d[hg.v[2]], mesh.rest3d[hg.v[3]]);
                const uint32_t hi = uint32_t(t.hinges.size());
                t.hinges.push_back(hg);
                for (uint32_t r = 0; r < 4; ++r)
                    vHinge[hg.v[r]].push_back(hi << 2 | r);
            }
        }
        if (f1.strainLimit > 0.f && restLen > 1e-9f && (t.movable[size_t(a)] || t.movable[size_t(b)]))
            addLink(a, b, kStrain, (1.f + f1.strainLimit) * restLen, 0.f, false);
        e = end;
    }

    for (size_t i = 0; i < mesh.seamPairs.size(); ++i)
    {
        const int a = mesh.seamPairs[i][0], b = mesh.seamPairs[i][1];
        if (a < 0 || b < 0 || a >= n || b >= n || a == b || (!t.movable[size_t(a)] && !t.movable[size_t(b)]))
            continue;
        const float rest = i < mesh.seamRestLength.size() ? std::max(0.f, mesh.seamRestLength[i]) : 0.f;
        float limit = std::max(rest, float(d3Len(d3Sub(toD(mesh.rest3d[size_t(a)]), toD(mesh.rest3d[size_t(b)])))));
        const bool closed = rest <= 1e-6f && limit <= tune.closedGap;
        if (closed)
            limit = 0.f;
        addLink(a, b, kSeam, limit, rest, closed);
    }
    for (uint32_t k = 0; k < uint32_t(t.links.size()); ++k)
    {
        vLink[t.links[k].v[0]].push_back(k << 1);
        vLink[t.links[k].v[1]].push_back(k << 1 | 1);
    }

    // Seam partners act as one vertex for self collision.
    std::vector<int> weld(size_t(n));
    std::iota(weld.begin(), weld.end(), 0);
    for (const GpuLink& l : t.links)
        if (l.v[2] == kSeam)
        {
            const int ra = weldRoot(weld, int(l.v[0])), rb = weldRoot(weld, int(l.v[1]));
            weld[size_t(std::max(ra, rb))] = std::min(ra, rb);
        }
    t.weld.resize(size_t(n));
    for (int i = 0; i < n; ++i)
        t.weld[size_t(i)] = uint32_t(weldRoot(weld, i));

    // Greedy colouring in vertex order over triangles, hinges and links, as ClothSolver does.
    std::vector<std::vector<int>> adj(size_t(n));
    auto connect = [&](int a, int b) {
        adj[size_t(a)].push_back(b);
        adj[size_t(b)].push_back(a);
    };
    for (const auto& tri : tris)
    {
        connect(tri[0], tri[1]);
        connect(tri[1], tri[2]);
        connect(tri[2], tri[0]);
    }
    for (const GpuHinge& hg : t.hinges)
        connect(int(hg.v[2]), int(hg.v[3]));
    for (const GpuLink& l : t.links)
        connect(int(l.v[0]), int(l.v[1]));
    std::vector<int> col(size_t(n), -1), usedBy;
    std::vector<std::vector<uint32_t>> colours;
    for (int i = 0; i < n; ++i)
    {
        if (!t.movable[size_t(i)])
            continue;
        for (int nb : adj[size_t(i)])
            if (col[size_t(nb)] >= 0)
            {
                if (int(usedBy.size()) <= col[size_t(nb)])
                    usedBy.resize(size_t(col[size_t(nb)]) + 1, -1);
                usedBy[size_t(col[size_t(nb)])] = i;
            }
        int c = 0;
        while (c < int(usedBy.size()) && usedBy[size_t(c)] == i)
            ++c;
        col[size_t(i)] = c;
        if (int(colours.size()) <= c)
            colours.resize(size_t(c) + 1);
        colours[size_t(c)].push_back(uint32_t(i));
    }
    toCsr(colours, t.colourOff, t.colour);

    float maxThickness = 0.f;
    for (float th : thickness)
        maxThickness = std::max(maxThickness, th);
    const float avgEdge = edgeCount ? float(edgeSum / double(edgeCount)) : 0.02f;
    t.queryRadius = std::max(3.f * (s.collisionThickness + maxThickness), 0.5f * avgEdge);
    t.cellSize = std::max(t.queryRadius, avgEdge);

    t.x.resize(size_t(n));
    t.vinfo.resize(size_t(n));
    t.vextra.resize(size_t(n));
    for (int i = 0; i < n; ++i)
    {
        const size_t u = size_t(i);
        t.x[u] = {mesh.rest3d[u][0], mesh.rest3d[u][1], mesh.rest3d[u][2], 0.f};
        t.vinfo[u] = {t.mass[u], bitsToFloat(t.movable[u] ? kMovable : 0u), friction[u], damping[u]};
        t.vextra[u] = {thickness[u], tune.contactStiffness + 1e3f * t.mass[u] / h2, std::max(1.f, 10.f * t.mass[u] / h2), 0.f};
    }
    toCsr(vTri, t.triOff, t.triAdj);
    toCsr(vHinge, t.hingeOff, t.hingeAdj);
    toCsr(vLink, t.linkOff, t.linkAdj);
    return t;
}

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

void destroy(VulkanContext& vk, Image& img)
{
    if (img.view)
        vkDestroyImageView(vk.device, img.view, nullptr);
    if (img.image)
        vmaDestroyImage(vk.allocator, img.image, img.alloc);
    img = {};
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

/** A 3D R32F distance grid (x fastest, as Sdf::values), filled by a copy recorded into cmd. Read with texel loads. */
Image uploadGrid(VulkanContext& vk, VkCommandBuffer cmd, int nx, int ny, int nz, const float* values, std::vector<rc::Buffer>& staging)
{
    Image img;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_3D;
    ici.format = VK_FORMAT_R32_SFLOAT;
    ici.extent = {uint32_t(nx), uint32_t(ny), uint32_t(nz)};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(vk.allocator, &ici, &aci, &img.image, &img.alloc, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Tailor: could not allocate a collider grid");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = ici.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(vk.device, &vci, nullptr, &img.view) != VK_SUCCESS)
    {
        destroy(vk, img);
        throw std::runtime_error("Tailor: could not create a collider grid view");
    }
    staging.push_back(rc::hostBuffer(vk, values, size_t(nx) * ny * nz * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = ici.extent;
    vkCmdCopyBufferToImage(cmd, staging.back().buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return img;
}

uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1024;
    while (p < v && p < (1u << 30))
        p <<= 1;
    return p;
}

bool validGrid(const Sdf& s) { return s.nx >= 2 && s.ny >= 2 && s.nz >= 2 && s.cell > 0.f && s.values.size() >= size_t(s.nx) * s.ny * s.nz; }
} // namespace

struct ClothGpu::Impl
{
    std::mutex mutex;
    GpuTuning tune;
    SolverSettings settings;
    float h = 1.f / 120.f;
    Topology topo;
    std::vector<Vec3> positions;
    bool ready = false;

    // Host-side changes, uploaded at the next step.
    std::vector<Sdf> colliders;
    std::array<std::array<float, 12>, kMaxColliders> colliderXf{};
    bool collidersDirty = false;
    std::vector<Pin> pins;
    std::vector<int> pinVerts; // the pinned set the duals belong to
    bool pinsDirty = false;
    bool pinDualsReset = false;

    // Render thread, under mutex.
    rc::Buffer buf[BufCount]{};
    rc::Buffer scene{}, readback{};
    Image grids[kMaxColliders]{};
    Image dummy{};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipes[KernelCount]{};
    std::vector<uint32_t> code;
    uint64_t codeHash = 0;
    rc::Submitter submit;
    uint32_t hashSize = 0;

    Impl()
    {
        for (auto& xf : colliderXf)
            xf = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    }

    void releaseLocked(VulkanContext& vk)
    {
        for (rc::Buffer& b : buf)
            rc::destroy(vk, b);
        rc::destroy(vk, scene);
        rc::destroy(vk, readback);
        for (Image& g : grids)
            destroy(vk, g);
        destroy(vk, dummy);
        if (pool)
            vkDestroyDescriptorPool(vk.device, pool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(vk.device, setLayout, nullptr);
        rc::destroy(vk, layout);
        submit.destroy(vk);
        // Pipelines belong to the shared cache, which the renderer clears when it stops.
        pool = VK_NULL_HANDLE;
        set = VK_NULL_HANDLE;
        setLayout = VK_NULL_HANDLE;
        layout = VK_NULL_HANDLE;
        for (VkPipeline& p : pipes)
            p = VK_NULL_HANDLE;
        ready = false;
    }

    /** Set layout, descriptor set, the dummy grid and the pipelines, made once per device. */
    void ensureDevice(VulkanContext& vk, VkCommandBuffer cmd, std::vector<rc::Buffer>& staging)
    {
        if (!setLayout)
        {
            VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxColliders, kCompute, nullptr};
            VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            lci.bindingCount = 1;
            lci.pBindings = &b;
            vkCreateDescriptorSetLayout(vk.device, &lci, nullptr, &setLayout);
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxColliders};
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.maxSets = 1;
            pci.poolSizeCount = 1;
            pci.pPoolSizes = &size;
            vkCreateDescriptorPool(vk.device, &pci, nullptr, &pool);
            if (setLayout && pool)
            {
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = pool;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &setLayout;
                vkAllocateDescriptorSets(vk.device, &ai, &set);
            }
            if (!set)
                throw std::runtime_error("Tailor: could not create the collider descriptor set");
        }
        if (!dummy.image)
        {
            const float far[8] = {1e4f, 1e4f, 1e4f, 1e4f, 1e4f, 1e4f, 1e4f, 1e4f};
            dummy = uploadGrid(vk, cmd, 2, 2, 2, far, staging);
            writeDescriptors(vk);
        }
        if (!layout)
        {
            const VkDescriptorSetLayout sets[] = {setLayout};
            layout = rc::pipelineLayout(vk, sizeof(ClothPush), kCompute, sets);
        }
        if (code.empty())
        {
            std::string err;
            code = rc::loadSpirv("cloth_vbd.spv", &err);
            if (code.empty())
                throw std::runtime_error(err);
            codeHash = rc::spirvHash(code);
        }
        const uint64_t key = rc::layoutKey(sizeof(ClothPush), kCompute, "tailor.cloth.sdf4");
        for (int k = 0; k < KernelCount; ++k)
            pipes[k] = rc::sharedPipelines().get(vk, layout, key, code, codeHash, kEntries[k]);
    }

    void writeDescriptors(VulkanContext& vk)
    {
        VkDescriptorImageInfo infos[kMaxColliders];
        for (uint32_t c = 0; c < kMaxColliders; ++c)
            infos[c] = {VK_NULL_HANDLE, grids[c].view ? grids[c].view : dummy.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = kMaxColliders;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = infos;
        vkUpdateDescriptorSets(vk.device, 1, &w, 0, nullptr);
    }

    void upload(VulkanContext& vk, VkCommandBuffer cmd, Buf id, const void* data, size_t bytes, std::vector<rc::Buffer>& staging,
                VkBufferUsageFlags usage = 0)
    {
        rc::destroy(vk, buf[id]);
        buf[id] = rc::makeBuffer(vk, std::max<size_t>(bytes, 16), usage, rc::Host::None);
        if (data && bytes)
        {
            staging.push_back(rc::hostBuffer(vk, data, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
            rc::copy(cmd, staging.back(), buf[id], bytes);
        }
    }

    template <class T> void upload(VulkanContext& vk, VkCommandBuffer cmd, Buf id, const std::vector<T>& v, std::vector<rc::Buffer>& staging)
    {
        upload(vk, cmd, id, v.data(), v.size() * sizeof(T), staging);
    }

    /** Pin targets for every vertex; only movable vertices take pins, as in ClothSolver. */
    std::vector<F4> pinTargets() const
    {
        std::vector<F4> p(topo.n, F4{0, 0, 0, 0});
        for (const Pin& pin : pins)
            if (pin.vertex >= 0 && uint32_t(pin.vertex) < topo.n && topo.movable[size_t(pin.vertex)])
                p[size_t(pin.vertex)] = {pin.target[0], pin.target[1], pin.target[2], 1.f};
        return p;
    }

    std::vector<F4> freshPinDuals() const
    {
        std::vector<F4> d(topo.n);
        for (uint32_t i = 0; i < topo.n; ++i)
            d[i] = {0.f, 0.f, 0.f, topo.vextra[i][2]};
        return d;
    }

    /** Uploads the whole topology and state. Render thread. */
    void uploadAll(VulkanContext& vk, VkCommandBuffer cmd, std::vector<rc::Buffer>& staging)
    {
        const uint32_t n = topo.n, nt = uint32_t(topo.tris.size());
        upload(vk, cmd, BX, topo.x, staging);
        upload(vk, cmd, BXStart, topo.x, staging);
        upload(vk, cmd, BXNew, topo.x, staging);
        upload(vk, cmd, BXDetect, topo.x, staging);
        upload(vk, cmd, BY, topo.x, staging);
        upload(vk, cmd, BVel, std::vector<F4>(n, F4{0, 0, 0, 0}), staging);
        upload(vk, cmd, BVInfo, topo.vinfo, staging);
        upload(vk, cmd, BVExtra, topo.vextra, staging);
        upload(vk, cmd, BPin, pinTargets(), staging);
        upload(vk, cmd, BPinDual, freshPinDuals(), staging);
        upload(vk, cmd, BTriOff, topo.triOff, staging);
        upload(vk, cmd, BTriAdj, topo.triAdj, staging);
        upload(vk, cmd, BHingeOff, topo.hingeOff, staging);
        upload(vk, cmd, BHingeAdj, topo.hingeAdj, staging);
        upload(vk, cmd, BLinkOff, topo.linkOff, staging);
        upload(vk, cmd, BLinkAdj, topo.linkAdj, staging);
        upload(vk, cmd, BColour, topo.colour, staging);
        upload(vk, cmd, BWeld, topo.weld, staging);
        upload(vk, cmd, BTris, topo.tris, staging);
        upload(vk, cmd, BHinges, topo.hinges, staging);
        upload(vk, cmd, BLinks, topo.links, staging);
        std::vector<F4> linkDual(topo.links.size());
        for (size_t k = 0; k < topo.links.size(); ++k)
            linkDual[k] = {0.f, 0.f, 0.f, topo.links[k].p[2]};
        upload(vk, cmd, BLinkDual, linkDual, staging);
        upload(vk, cmd, BColliders, nullptr, size_t(kMaxColliders) * 5 * sizeof(F4), staging);
        upload(vk, cmd, BCellHead, nullptr, size_t(hashSize) * 4, staging);
        upload(vk, cmd, BCellNext, nullptr, size_t(n) * 4, staging);
        upload(vk, cmd, BVtCount, std::vector<uint32_t>(n, 0), staging);
        upload(vk, cmd, BVt, nullptr, size_t(n) * kMaxVt * 4, staging);
        upload(vk, cmd, BTvCount, std::vector<uint32_t>(n, 0), staging);
        upload(vk, cmd, BTv, nullptr, size_t(n) * kMaxTv * 8, staging);
        upload(vk, cmd, BDminV, nullptr, size_t(n) * 4, staging);
        upload(vk, cmd, BDminT, nullptr, size_t(nt) * 4, staging);
        upload(vk, cmd, BBound, std::vector<float>(n, 1e30f), staging);
        const std::vector<uint32_t> state(kStateWords, 0);
        upload(vk, cmd, BState, state.data(), state.size() * 4, staging, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        upload(vk, cmd, BReserved, nullptr, 16, staging);
        rc::destroy(vk, scene);
        scene = rc::makeBuffer(vk, sizeof(GpuScene), 0, rc::Host::None);
        rc::destroy(vk, readback);
        readback = rc::makeBuffer(vk, std::max<size_t>(size_t(n) * sizeof(F4), 16), 0, rc::Host::Read);
    }

    void uploadColliders(VulkanContext& vk, VkCommandBuffer cmd, std::vector<rc::Buffer>& staging)
    {
        for (Image& g : grids)
            destroy(vk, g);
        for (size_t c = 0; c < colliders.size() && c < kMaxColliders; ++c)
            if (validGrid(colliders[c]))
                grids[c] = uploadGrid(vk, cmd, colliders[c].nx, colliders[c].ny, colliders[c].nz, colliders[c].values.data(), staging);
        writeDescriptors(vk);
    }

    GpuScene sceneData() const
    {
        GpuScene g{};
        for (int b = 0; b < BufCount; ++b)
            g.ptr[b] = buf[b].address;
        g.nVerts = topo.n;
        g.nTris = uint32_t(topo.tris.size());
        g.nHinges = uint32_t(topo.hinges.size());
        g.nLinks = uint32_t(topo.links.size());
        uint32_t live = 0;
        for (uint32_t c = 0; c < kMaxColliders; ++c)
            if (grids[c].image)
                live = c + 1;
        g.nColliders = live;
        g.hashMask = hashSize - 1;
        g.selfCollision = settings.selfCollision ? 1u : 0u;
        g.nMovable = uint32_t(topo.colour.size());
        g.h = h;
        g.lambdaDecay = tune.lambdaDecay;
        g.stiffnessDecay = tune.stiffnessDecay;
        g.beta = tune.beta;
        g.kMax = tune.kMax;
        g.thickness = settings.collisionThickness;
        g.queryRadius = topo.queryRadius;
        g.cellSize = topo.cellSize;
        g.boundFraction = tune.boundFraction;
        g.frictionSlip = tune.frictionSlip;
        g.drag = 1.f / (1.f + tune.airDrag * h);
        g.seamKeep = 1.f - std::clamp(settings.seamCloseRate, 0.f, 1.f);
        g.closedGap = tune.closedGap;
        for (int k = 0; k < 3; ++k)
            g.gravity[k] = settings.gravity[size_t(k)];
        return g;
    }

    /** Grid transform rows, origin and cell, dims (as uint bits) per collider; an invalid grid gets zero dims. */
    std::vector<F4> colliderRecords() const
    {
        std::vector<F4> r(size_t(kMaxColliders) * 5, F4{0, 0, 0, 0});
        for (size_t c = 0; c < colliders.size() && c < kMaxColliders; ++c)
        {
            const Sdf& s = colliders[c];
            const auto& xf = colliderXf[c];
            for (size_t row = 0; row < 3; ++row)
                r[c * 5 + row] = {xf[row * 4], xf[row * 4 + 1], xf[row * 4 + 2], xf[row * 4 + 3]};
            r[c * 5 + 3] = {s.origin[0], s.origin[1], s.origin[2], s.cell};
            if (validGrid(s))
                r[c * 5 + 4] = {bitsToFloat(uint32_t(s.nx)), bitsToFloat(uint32_t(s.ny)), bitsToFloat(uint32_t(s.nz)), 0.f};
        }
        return r;
    }

    void bind(VkCommandBuffer cmd, Kernel k) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[k]); }

    void dispatch(VkCommandBuffer cmd, Kernel k, uint32_t first, uint32_t count)
    {
        if (!count)
            return;
        bind(cmd, k);
        ClothPush push{scene.address, 0, 0, 0, 0};
        rc::dispatchChunks(cmd, layout, push, first, count, kGroup, [](ClothPush& p, uint32_t f, uint32_t c) {
            p.first = f;
            p.count = c;
        });
        rc::memoryBarrier(cmd);
    }

    /** Re-detection sized by redetectArgsCS: zero groups unless a vertex hit its bound in the last iteration. */
    void dispatchIndirect(VkCommandBuffer cmd, Kernel k, uint32_t count, uint32_t slot)
    {
        bind(cmd, k);
        ClothPush push{scene.address, 0, count, 0, 0};
        vkCmdPushConstants(cmd, layout, kCompute, 0, sizeof push, &push);
        vkCmdDispatchIndirect(cmd, buf[BState].buffer, VkDeviceSize(4 + slot * 3) * 4);
        rc::memoryBarrier(cmd);
    }

    void detect(VkCommandBuffer cmd)
    {
        dispatch(cmd, KHashClear, 0, hashSize);
        dispatch(cmd, KHashInsert, 0, topo.n);
        dispatch(cmd, KHashQuery, 0, uint32_t(topo.tris.size()));
        dispatch(cmd, KContactFinalize, 0, topo.n);
    }

    void record(VkCommandBuffer cmd)
    {
        const uint32_t n = topo.n, nl = uint32_t(topo.links.size()), nt = uint32_t(topo.tris.size());
        const bool self = settings.selfCollision;
        const int iterations = std::max(1, settings.iterations);
        // Indirect dispatches take no chunking; past their limit re-detection waits for the next substep.
        const bool indirect = std::max({hashSize, n, nt}) <= kMaxIndirectItems;

        dispatch(cmd, KAdvanceSeams, 0, nl);
        for (int sub = 0; sub < std::max(1, settings.substeps); ++sub)
        {
            dispatch(cmd, KBeginStep, 0, n);
            dispatch(cmd, KBeginLinks, 0, nl);
            if (self)
                detect(cmd);
            dispatch(cmd, KInitGuess, 0, n);
            for (int it = 0; it < iterations; ++it)
            {
                for (size_t c = 0; c + 1 < topo.colourOff.size(); ++c)
                {
                    const uint32_t first = topo.colourOff[c], count = topo.colourOff[c + 1] - first;
                    dispatch(cmd, KSolve, first, count);
                    dispatch(cmd, KCommit, first, count);
                }
                dispatch(cmd, KDualLinks, 0, nl);
                dispatch(cmd, KDualPins, 0, n);
                if (self && indirect && it + 1 < iterations)
                {
                    // dispatch() ends in a barrier over all commands, indirect argument reads included.
                    dispatch(cmd, KRedetectArgs, 0, 1);
                    dispatchIndirect(cmd, KHashClear, hashSize, 0);
                    dispatchIndirect(cmd, KHashInsert, n, 1);
                    dispatchIndirect(cmd, KHashQuery, nt, 2);
                    dispatchIndirect(cmd, KContactFinalize, n, 3);
                }
            }
            dispatch(cmd, KEndStep, 0, n);
        }
        const VkBufferCopy toHost{0, 0, VkDeviceSize(n) * sizeof(F4)};
        vkCmdCopyBuffer(cmd, buf[BX].buffer, readback.buffer, 1, &toHost);
    }

    /** A reset's upload (uploadTopology) or one frame (simulate). Render thread, under mutex. */
    void run(VulkanContext& vk, bool uploadTopology, bool simulate)
    {
        submit.init(vk);
        std::vector<rc::Buffer> staging;
        auto freeStaging = [&] {
            for (rc::Buffer& b : staging)
                rc::destroy(vk, b);
            staging.clear();
        };
        try
        {
            submit.run(vk, [&](VkCommandBuffer cmd) {
                ensureDevice(vk, cmd, staging);
                if (uploadTopology)
                    uploadAll(vk, cmd, staging);
                if (collidersDirty || uploadTopology)
                {
                    uploadColliders(vk, cmd, staging);
                    collidersDirty = false;
                }
                if (pinsDirty && !uploadTopology)
                {
                    upload(vk, cmd, BPin, pinTargets(), staging);
                    if (pinDualsReset)
                        upload(vk, cmd, BPinDual, freshPinDuals(), staging);
                }
                pinsDirty = pinDualsReset = false;
                const GpuScene g = sceneData();
                staging.push_back(rc::hostBuffer(vk, &g, sizeof g, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
                rc::copy(cmd, staging.back(), scene, sizeof g);
                const std::vector<F4> rec = colliderRecords();
                staging.push_back(rc::hostBuffer(vk, rec.data(), rec.size() * sizeof(F4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
                rc::copy(cmd, staging.back(), buf[BColliders], rec.size() * sizeof(F4));
                rc::memoryBarrier(cmd);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                if (simulate)
                    record(cmd);
            });
        }
        catch (...)
        {
            freeStaging();
            throw;
        }
        freeStaging();
        if (simulate)
        {
            vmaInvalidateAllocation(vk.allocator, readback.alloc, 0, VK_WHOLE_SIZE);
            const F4* x = static_cast<const F4*>(readback.mapped);
            positions.resize(topo.n);
            for (uint32_t i = 0; i < topo.n; ++i)
                positions[i] = {x[i][0], x[i][1], x[i][2]};
        }
    }
};

namespace
{
std::once_flag gStopHook;
std::mutex gLiveMutex;
std::atomic<std::thread::id> gRenderThread;

/** Releasers of live solvers, run when the renderer stops; each returns false once its solver is gone. */
std::vector<std::function<bool(VulkanContext&)>>& liveSolvers()
{
    static std::vector<std::function<bool(VulkanContext&)>> live;
    return live;
}

/** An extension DLL links its own volk, whose entry points stay null until loaded for Lila's device. */
void ensureVolk(VulkanContext& vk)
{
    if (volkGetLoadedDevice() == vk.device)
        return;
    volkInitialize();
    volkLoadInstance(vk.instance);
    volkLoadDevice(vk.device);
}

/** Runs fn on the render thread and waits; false with err when the renderer is not running or the task threw. */
template <class F> bool onRenderThread(F&& fn, std::string* err)
{
    Renderer& r = Renderer::get();
    const char* problem = !r.running()                                          ? "Tailor: the GPU solver needs the renderer"
                          : std::this_thread::get_id() == gRenderThread.load() ? "Tailor: the GPU solver cannot wait on the render thread"
                                                                                : nullptr;
    if (problem)
    {
        if (err)
            *err = problem;
        return false;
    }
    try
    {
        r.runTask([&](VulkanContext& vk) {
             gRenderThread = std::this_thread::get_id();
             ensureVolk(vk);
             fn(vk);
             return true;
         })
            .get();
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = e.what();
        return false;
    }
    return true;
}
} // namespace

ClothGpu::ClothGpu() : impl_(std::make_shared<Impl>())
{
    std::call_once(gStopHook, [] {
        Renderer::get().addStopHook([](VulkanContext& vk) {
            std::scoped_lock live(gLiveMutex);
            auto& l = liveSolvers();
            l.erase(std::remove_if(l.begin(), l.end(), [&](auto& release) { return !release(vk); }), l.end());
        });
    });
    std::weak_ptr<Impl> weak = impl_;
    std::scoped_lock live(gLiveMutex);
    liveSolvers().push_back([weak](VulkanContext& vk) {
        auto impl = weak.lock();
        if (!impl)
            return false;
        std::scoped_lock lock(impl->mutex);
        impl->releaseLocked(vk);
        return true;
    });
}

ClothGpu::~ClothGpu()
{
    std::scoped_lock lock(impl_->mutex);
    if (impl_->submit.pool || impl_->scene.buffer)
        onRenderThread([&](VulkanContext& vk) { impl_->releaseLocked(vk); }, nullptr);
}

bool ClothGpu::available() { return Renderer::get().running(); }

bool ClothGpu::reset(const ClothMesh& mesh, const Garment& g, const SolverSettings& s, std::string* err)
{
    std::scoped_lock lock(impl_->mutex);
    Impl& m = *impl_;
    m.settings = s;
    m.settings.substeps = std::max(1, s.substeps);
    m.settings.iterations = std::max(1, s.iterations);
    m.h = std::max(s.dt, 1e-6f) / float(m.settings.substeps);
    m.topo = buildTopology(mesh, g, m.settings, m.tune, m.h);
    m.positions = mesh.rest3d;
    m.hashSize = nextPow2(std::max<uint32_t>(2 * m.topo.n, 1));
    m.pinVerts.clear();
    for (const Pin& p : m.pins)
        m.pinVerts.push_back(p.vertex);
    m.ready = false;
    if (m.topo.n == 0)
        return true;
    if (!onRenderThread([&](VulkanContext& vk) { m.run(vk, true, false); }, err))
        return false;
    m.ready = true;
    return true;
}

void ClothGpu::setColliders(std::vector<const Sdf*> colliders)
{
    std::scoped_lock lock(impl_->mutex);
    impl_->colliders.clear();
    for (const Sdf* s : colliders)
        if (s && impl_->colliders.size() < kMaxColliders)
            impl_->colliders.push_back(*s);
    impl_->collidersDirty = true;
}

void ClothGpu::setColliderTransform(int index, const std::array<float, 12>& worldToGrid)
{
    std::scoped_lock lock(impl_->mutex);
    if (index >= 0 && index < int(kMaxColliders))
        impl_->colliderXf[size_t(index)] = worldToGrid;
}

void ClothGpu::setPins(std::vector<Pin> pins)
{
    std::scoped_lock lock(impl_->mutex);
    Impl& m = *impl_;
    std::vector<int> verts;
    for (const Pin& p : pins)
        verts.push_back(p.vertex);
    // Moving a pin keeps its dual warm; a different pinned set starts over, as ClothSolver::syncPins does.
    if (verts != m.pinVerts)
    {
        m.pinVerts = std::move(verts);
        m.pinDualsReset = true;
    }
    m.pins = std::move(pins);
    m.pinsDirty = true;
}

bool ClothGpu::step(std::string* err)
{
    std::scoped_lock lock(impl_->mutex);
    Impl& m = *impl_;
    if (m.topo.n == 0)
        return true;
    if (!m.ready)
    {
        if (err)
            *err = "Tailor: the GPU solver has no device state; reset it";
        return false;
    }
    if (!onRenderThread([&](VulkanContext& vk) { m.run(vk, false, true); }, err))
    {
        m.ready = false;
        return false;
    }
    return true;
}

const std::vector<Vec3>& ClothGpu::positions() const { return impl_->positions; }

std::vector<float> ClothGpu::strainMap() const
{
    std::scoped_lock lock(impl_->mutex);
    const Impl& m = *impl_;
    const auto& x = m.positions;
    std::vector<float> out(x.size(), 1.f), sum(x.size(), 0.f), count(x.size(), 0.f);
    for (const GpuTri& t : m.topo.tris)
    {
        if (t.rest[0] <= 0.f || t.v[0] >= x.size() || t.v[1] >= x.size() || t.v[2] >= x.size())
            continue;
        const D3 x0 = toD(x[t.v[0]]), e1 = d3Sub(toD(x[t.v[1]]), x0), e2 = d3Sub(toD(x[t.v[2]]), x0);
        const D3 f0{e1[0] * t.b[0] + e2[0] * t.b[2], e1[1] * t.b[0] + e2[1] * t.b[2], e1[2] * t.b[0] + e2[2] * t.b[2]};
        const D3 f1{e1[0] * t.b[1] + e2[0] * t.b[3], e1[1] * t.b[1] + e2[1] * t.b[3], e1[2] * t.b[1] + e2[2] * t.b[3]};
        const double a = d3Dot(f0, f0), b = d3Dot(f0, f1), c = d3Dot(f1, f1);
        const double half = 0.5 * (a + c);
        const float stretch = float(std::sqrt(std::max(0.0, half + std::sqrt(std::max(0.0, half * half - (a * c - b * b))))));
        for (int k = 0; k < 3; ++k)
        {
            sum[t.v[k]] += stretch;
            count[t.v[k]] += 1.f;
        }
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (count[i] > 0.f)
            out[i] = sum[i] / count[i];
    return out;
}

GpuTuning& ClothGpu::tuning() { return impl_->tune; }

int ClothGpu::colourCount() const { return impl_->topo.colourOff.empty() ? 0 : int(impl_->topo.colourOff.size()) - 1; }
} // namespace tailor
