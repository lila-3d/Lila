// Baked detail for walls: a shared atlas the stones' far LODs read their worn edges from, and low poly shells per
// ground chunk with the stones baked onto them.

#include "MasonryBake.h"

#include <lila/Bake.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numeric>

namespace masonry
{
namespace
{
using json = nlohmann::json;
namespace bake = lila::bake;

/** The two axes a box face's texture runs along: z and y for an x face, x and z for a y face, x and y for a z face. */
std::array<int, 2> faceAxes(int axis) { return axis == 0 ? std::array<int, 2>{2, 1} : axis == 1 ? std::array<int, 2>{0, 2} : std::array<int, 2>{0, 1}; }

int dominantAxis(const glm::dvec3& n)
{
    const glm::dvec3 a = glm::abs(n);
    return a.x >= a.y && a.x >= a.z ? 0 : (a.y >= a.z ? 1 : 2);
}

/** Moves uv0 to uv1, then fills uv0 per corner from fn(face, point). */
template <class Fn> void rewriteUv(geo::Mesh& m, Fn&& fn)
{
    // uv1 first: adding a layer may move the others.
    m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(1));
    std::span<glm::vec2> uv1 = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
    std::span<glm::vec2> uv0 = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        for (geo::Index c : m.faceCorners(f))
        {
            uv1[c] = uv0[c];
            uv0[c] = glm::vec2(fn(f, m.position(m.cornerPoint(c))));
        }
    }
}

/** Every face its own island, projected flat onto its plane and packed at one density into uv0. */
void unwrapFaces(geo::Mesh& m, double pad)
{
    struct Island
    {
        glm::dvec3 u, v, lo;
        glm::dvec2 size;
    };
    std::vector<Island> islands(m.faceCapacity());
    std::vector<glm::dvec2> sizes;
    std::vector<geo::Index> faces;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = m.facePoints(f);
        const glm::dvec3 n = glm::normalize(m.faceNormal(f));
        glm::dvec3 u = m.position(pts[1]) - m.position(pts[0]);
        u = glm::normalize(u - n * glm::dot(u, n));
        const glm::dvec3 v = glm::cross(n, u);
        glm::dvec2 lo(1e300), hi(-1e300);
        for (geo::Index p : pts)
        {
            const glm::dvec2 q(glm::dot(m.position(p), u), glm::dot(m.position(p), v));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
        islands[f] = {u, v, glm::dvec3(lo, 0.0), hi - lo};
        sizes.push_back(hi - lo);
        faces.push_back(f);
    }
    const Packed packed = shelfPack(sizes, pad);
    std::vector<glm::dvec2> at(m.faceCapacity());
    for (size_t k = 0; k < faces.size(); ++k)
        at[faces[k]] = packed.origin[k];
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f : faces)
    {
        const Island& is = islands[f];
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::dvec3& p = m.position(m.cornerPoint(c));
            const glm::dvec2 q(glm::dot(p, is.u) - is.lo.x, glm::dot(p, is.v) - is.lo.y);
            uv[c] = glm::vec2(at[f] + q * packed.scale);
        }
    }
}

std::unique_ptr<bake::Backend> backendFor(const std::string& name)
{
    if (name == "cpu")
        return bake::createBackend("cpu");
    const std::vector<std::string> names = bake::backendNames();
    if (std::ranges::find(names, std::string("gpu")) != names.end())
        if (auto b = bake::createBackend("gpu"))
            return b;
    if (name == "gpu")
        throw std::runtime_error("no gpu bake backend in this session");
    return bake::createBackend("cpu");
}

bake::ProjectSettings projectSettings(const BakeSettings& s, const std::string& name, double aoDistance)
{
    bake::ProjectSettings ps = bake::ProjectSettings::defaults();
    ps.output.folder = s.folder.generic_string();
    ps.output.name = name;
    ps.output.format = "png";
    ps.output.width = ps.output.height = std::clamp(s.size, 64, 8192);
    ps.output.padding = std::max(1, s.padding / 2);
    ps.maps.clear();
    for (const std::string& m : s.maps)
    {
        bake::MapSettings ms;
        ms.map = m;
        // Occlusion only matters at the scale of chips and joints; the default reach and ray count cost far more.
        if (m == "ao")
            ms.options = {{"rays", 24}, {"distance", aoDistance}};
        ps.maps.push_back(ms);
    }
    return ps;
}

bake::GroupInputs group(std::string name, const geo::Mesh& low, std::span<const geo::Mesh> highs, double frontal, double rear)
{
    bake::GroupInputs g;
    g.name = name;
    g.settings.lows = {name};
    g.settings.frontal = frontal;
    g.settings.rear = rear;
    g.frontal = float(frontal);
    g.rear = float(rear);
    g.lows.push_back(bake::lowFromMesh(low, glm::dmat4(1.0), name));
    g.highMin = glm::vec3(1e30f);
    g.highMax = glm::vec3(-1e30f);
    for (size_t k = 0; k < highs.size(); ++k)
    {
        bake::HighInput h;
        h.soup = bake::soupFromMesh(highs[k], glm::dmat4(1.0), name + "_high" + std::to_string(k));
        h.source = h.soup.name;
        h.sourceTriangles = h.soup.triangleCount();
        g.highMin = glm::min(g.highMin, h.soup.boundsMin);
        g.highMax = glm::max(g.highMax, h.soup.boundsMax);
        bake::HighEntry e;
        e.node = h.source;
        g.settings.highs.push_back(e);
        g.highs.push_back(std::move(h));
    }
    return g;
}

json run(bake::BakeInputs inputs, const BakeSettings& s, const std::string& name, double aoDistance)
{
    LILA_ZONE();

    inputs.highMin = glm::vec3(1e30f);
    inputs.highMax = glm::vec3(-1e30f);
    for (const bake::GroupInputs& g : inputs.groups)
    {
        inputs.highMin = glm::min(inputs.highMin, g.highMin);
        inputs.highMax = glm::max(inputs.highMax, g.highMax);
    }
    const bake::ProjectSettings ps = projectSettings(s, name, aoDistance);
    bake::BakeOptions opt;
    opt.folder = s.folder;
    std::unique_ptr<bake::Backend> backend = backendFor(s.backend);
    try
    {
        backend->prepare(inputs);
    }
    catch (const std::exception& e)
    {
        if (s.backend != "auto" || backend->name() == "cpu")
            throw;
        lila::log::warn("Masonry bake: {} backend failed ({}), baking on the cpu", backend->name(), e.what());
        backend = bake::createBackend("cpu");
        backend->prepare(inputs);
    }
    bake::BakeResult r = bake::runBake(inputs, ps, opt, *backend);
    r.backend = backend->name();
    return r.toJson();
}
} // namespace

Packed shelfPack(std::span<const glm::dvec2> sizes, double pad)
{
    Packed out;
    out.origin.assign(sizes.size(), glm::dvec2(0.0));
    if (sizes.empty())
        return out;
    std::vector<size_t> order(sizes.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sizes[a].y > sizes[b].y; });
    double largest = 0.0;
    for (const glm::dvec2& s : sizes)
        largest = std::max({largest, s.x, s.y});
    if (largest <= 0.0)
        return out;
    auto place = [&](double scale, std::vector<glm::dvec2>* at) {
        double x = pad, y = pad, shelf = 0.0;
        for (size_t k : order)
        {
            const glm::dvec2 s = sizes[k] * scale;
            if (s.x + 2.0 * pad > 1.0)
                return false;
            if (x + s.x + pad > 1.0)
            {
                y += shelf + pad;
                x = pad;
                shelf = 0.0;
            }
            if (at)
                (*at)[k] = {x, y};
            x += s.x + pad;
            shelf = std::max(shelf, s.y);
        }
        return y + shelf + pad <= 1.0;
    };
    double lo = 0.0, hi = 1.0 / largest;
    for (int it = 0; it < 48; ++it)
    {
        const double mid = (lo + hi) * 0.5;
        (place(mid, nullptr) ? lo : hi) = mid;
    }
    out.scale = lo;
    place(lo, &out.origin);
    return out;
}

double atlasStones(Built& b, int size, int padding)
{
    LILA_ZONE();

    if (b.lods.size() != b.meshes.size())
        return 0.0;
    // Every mesh's six faces, from the box level's extents.
    std::vector<glm::dvec2> sizes;
    std::vector<glm::dvec3> half(b.lods.size());
    for (size_t r = 0; r < b.lods.size(); ++r)
    {
        const geo::Bounds bb = b.lods[r][2].bounds();
        half[r] = (bb.max - bb.min) * 0.5;
        for (int axis = 0; axis < 3; ++axis)
        {
            const std::array<int, 2> uv = faceAxes(axis);
            for (int side = 0; side < 2; ++side)
                sizes.push_back({2.0 * half[r][uv[0]], 2.0 * half[r][uv[1]]});
        }
    }
    const double texels = double(std::max(64, size));
    const Packed packed = shelfPack(sizes, double(std::max(1, padding)) / texels);
    for (size_t r = 0; r < b.lods.size(); ++r)
        for (size_t level = 0; level < b.lods[r].size(); ++level)
        {
            geo::Mesh& m = b.lods[r][level];
            rewriteUv(m, [&](geo::Index f, const glm::dvec3& p) {
                const glm::dvec3 n = m.faceNormal(f);
                const int axis = dominantAxis(n);
                const int side = n[axis] >= 0.0 ? 1 : 0;
                const std::array<int, 2> uv = faceAxes(axis);
                const glm::dvec2 local(std::clamp(p[uv[0]] + half[r][uv[0]], 0.0, 2.0 * half[r][uv[0]]),
                                       std::clamp(p[uv[1]] + half[r][uv[1]], 0.0, 2.0 * half[r][uv[1]]));
                return packed.origin[r * 6 + size_t(axis * 2 + side)] + local * packed.scale;
            });
            if (level > 0)
                lila::scene::materials::assign(m, kStoneFarSlot, {});
        }
    return packed.scale * texels;
}

std::vector<Shell> lowPolyShells(std::span<const Footprint> footprints, const WallParams& p, double size, int texture, int padding)
{
    LILA_ZONE();

    const double cellSize = std::max(size, 1.0);
    const double half = std::max(p.thickness, 1.0) * 0.5;
    auto cellOf = [&](const glm::dvec3& q) { return glm::ivec2(int(std::floor(q.x / cellSize)), int(std::floor(q.z / cellSize))); };
    auto less = [](const glm::ivec2& a, const glm::ivec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; };
    std::map<glm::ivec2, Shell, decltype(less)> cells(less);

    for (const Footprint& f : footprints)
    {
        if (f.points.size() < 2)
            continue;
        double base = 1e300;
        for (const glm::dvec3& q : f.points)
            base = std::min(base, q.y);
        // The footprint with a point wherever it crosses a grid line.
        std::vector<glm::dvec3> q;
        const size_t n = f.points.size();
        const size_t segs = f.closed ? n : n - 1;
        for (size_t s = 0; s < segs; ++s)
        {
            const glm::dvec3 a(f.points[s].x, base, f.points[s].z), b(f.points[(s + 1) % n].x, base, f.points[(s + 1) % n].z);
            q.push_back(a);
            std::vector<double> ts;
            for (int axis : {0, 2})
            {
                const double lo = std::min(a[axis], b[axis]), hi = std::max(a[axis], b[axis]);
                for (double line = std::ceil(lo / cellSize) * cellSize; line < hi; line += cellSize)
                    if (const double t = (line - a[axis]) / (b[axis] - a[axis]); t > 1e-6 && t < 1.0 - 1e-6)
                        ts.push_back(t);
            }
            std::sort(ts.begin(), ts.end());
            for (double t : ts)
                q.push_back(glm::mix(a, b, t));
        }
        if (!f.closed)
            q.push_back(glm::dvec3(f.points.back().x, base, f.points.back().z));
        const size_t m = q.size();
        const size_t subs = f.closed ? m : m - 1;
        auto dir = [&](size_t i) {
            const glm::dvec3 d = q[(i + 1) % m] - q[i % m];
            return glm::length(d) > 1e-12 ? glm::normalize(d) : glm::dvec3(1, 0, 0);
        };
        auto offsetAt = [&](size_t i) {
            const bool hasIn = f.closed || i > 0, hasOut = f.closed || i + 1 < m;
            const glm::dvec3 dOut = hasOut ? dir(i) : dir((i + m - 1) % m);
            const glm::dvec3 dIn = hasIn ? dir((i + m - 1) % m) : dOut;
            return miterOffset(dIn, dOut, half);
        };
        for (size_t s = 0; s < subs; ++s)
        {
            const size_t a = s, b = (s + 1) % m;
            const glm::ivec2 cell = cellOf((q[a] + q[b]) * 0.5);
            Shell& sh = cells[cell];
            sh.cell = cell;
            const glm::dvec3 oa = offsetAt(a), ob = offsetAt(b);
            const std::array<glm::dvec3, 4> ground = {q[a] + oa, q[b] + ob, q[b] - ob, q[a] - oa};
            geo::Mesh piece = prismMesh(ground, base, base + p.height);
            sh.mesh.append(piece);
            sh.hulls.push_back(std::move(piece));
            sh.origin = {(cell.x + 0.5) * cellSize, base, (cell.y + 0.5) * cellSize};
        }
    }

    std::vector<Shell> out;
    const double pad = double(std::max(1, padding)) / double(std::max(64, texture));
    for (auto& [cell, sh] : cells)
    {
        unwrapFaces(sh.mesh, pad);
        std::span<bool> hard = sh.mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        std::fill(hard.begin(), hard.end(), true);
        lila::scene::materials::assign(sh.mesh, kShellSlot, {});
        out.push_back(std::move(sh));
    }
    return out;
}

std::vector<Shell> shellsFromCells(std::span<const geo::Mesh> cells, double size, int texture, int padding)
{
    LILA_ZONE();

    const double cellSize = std::max(size, 1.0);
    auto less = [](const glm::ivec2& a, const glm::ivec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; };
    std::map<glm::ivec2, Shell, decltype(less)> chunks(less);
    for (const geo::Mesh& c : cells)
    {
        const geo::Bounds b = c.bounds();
        const glm::dvec3 mid = (b.min + b.max) * 0.5;
        const glm::ivec2 at(int(std::floor(mid.x / cellSize)), int(std::floor(mid.z / cellSize)));
        Shell& sh = chunks[at];
        if (sh.hulls.empty())
            sh.origin = {(at.x + 0.5) * cellSize, b.min.y, (at.y + 0.5) * cellSize};
        sh.cell = at;
        sh.origin.y = std::min(sh.origin.y, b.min.y);
        sh.mesh.append(c);
        sh.hulls.push_back(c);
    }
    std::vector<Shell> out;
    const double pad = double(std::max(1, padding)) / double(std::max(64, texture));
    for (auto& [at, sh] : chunks)
    {
        unwrapFaces(sh.mesh, pad);
        std::span<bool> hard = sh.mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
        std::fill(hard.begin(), hard.end(), true);
        lila::scene::materials::assign(sh.mesh, kShellSlot, {});
        out.push_back(std::move(sh));
    }
    return out;
}

json bakeAtlas(const Built& b, double bevel, const std::string& project, const BakeSettings& s)
{
    LILA_ZONE();

    bake::BakeInputs inputs;
    inputs.project = project;
    for (size_t r = 0; r < b.lods.size(); ++r)
    {
        const geo::Bounds bb = b.lods[r][2].bounds();
        const glm::dvec3 ext = bb.max - bb.min;
        const double minHalf = std::min({ext.x, ext.y, ext.z}) * 0.5;
        const geo::Mesh& worn = b.lods[r][0];
        inputs.groups.push_back(group(project + "_" + std::to_string(r), b.lods[r][2], std::span<const geo::Mesh>(&worn, 1),
                                      std::max(0.2, bevel * 0.5), std::max(0.5, minHalf)));
    }
    const BakeSettings& settings = s;
    return run(std::move(inputs), settings, "T_" + project + "_{map}", std::max(1.0, bevel * 4.0));
}

json bakeShells(const Built& b, std::span<const Shell> shells, const WallParams& p, const std::string& project, const BakeSettings& s)
{
    LILA_ZONE();

    bake::BakeInputs inputs;
    inputs.project = project;
    const double frontal = std::max(1.0, p.protrude + p.stone.joint + 1.0);
    const double rear = std::max(2.0, p.recess + p.stone.joint + 2.0);
    for (const Shell& sh : shells)
    {
        // Every stone that reaches the shell, whichever chunk its centre is in.
        geo::Bounds near = sh.mesh.bounds();
        const double reach = std::max({p.stone.length, p.stone.depth, p.thickness}) + frontal;
        near.min -= glm::dvec3(reach);
        near.max += glm::dvec3(reach);
        std::vector<int> members;
        for (size_t k = 0; k < b.stones.size(); ++k)
        {
            const glm::dvec3& c = b.stones[k].center;
            if (glm::all(glm::greaterThanEqual(c, near.min)) && glm::all(glm::lessThanEqual(c, near.max)))
                members.push_back(int(k));
        }
        std::vector<geo::Mesh> highs;
        if (!members.empty())
            highs.push_back(mergeStones(b, members, 0, glm::dvec3(0.0)));
        highs.push_back(b.core);
        inputs.groups.push_back(group(std::format("{}_{}_{}", project, sh.cell.x, sh.cell.y), sh.mesh, highs, frontal, rear));
    }
    return run(std::move(inputs), s, "T_{group}_{map}", std::max(3.0, (p.stone.joint + p.recess) * 4.0));
}
} // namespace masonry
