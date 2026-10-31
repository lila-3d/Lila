#include "clay/Clay.h"

#include "core/Profile.h"
#include "geo/ColorSets.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace lila::clay
{
namespace
{
using glm::dvec3;
using glm::ivec3;

constexpr int kBrick = 8;
// Lattice samples per brick axis: the brick's cells, one cell of apron below and one above, so every cell around an
// edge the brick owns is in reach.
constexpr int kSamples = kBrick + 3;
constexpr int kCells = kSamples - 1;
constexpr int kBias = 1 << 19;

uint64_t pack(const ivec3& c)
{
    return (uint64_t(c.x + kBias) & 0xfffff) | ((uint64_t(c.y + kBias) & 0xfffff) << 20) | ((uint64_t(c.z + kBias) & 0xfffff) << 40);
}

/** One brick's share of the surface: its quads over vertices keyed by global cell, so neighbours weld exactly. */
struct Piece
{
    std::vector<uint64_t> cells;
    std::vector<dvec3> points;
    std::vector<glm::vec3> colors;
    std::vector<uint32_t> quads;
};

Bounds brickBounds(const ivec3& b, double h)
{
    return {dvec3(b * kBrick) * h, dvec3((b + 1) * kBrick) * h};
}

/** Whether the brick can hold surface: inside some added bounds and not provably far from the surface. */
bool live(const Field& field, const Field::Local& local, const ivec3& b, double h)
{
    const Bounds box = brickBounds(b, h);
    const Bounds reach = box.grown(2.0 * h);
    if (std::none_of(field.addBounds().begin(), field.addBounds().end(), [&](const Bounds& a) { return a.overlaps(reach); }))
        return false;
    const double halfDiagonal = 0.5 * std::sqrt(3.0) * kBrick * h;
    // Slack over the half diagonal: smooth joins and scaled or ellipsoid distances are not exact.
    return std::abs(local.eval(0.5 * (box.min + box.max)).distance) <= 1.5 * halfDiagonal + 2.0 * h;
}

dvec3 gradient(const Field::Local& f, const dvec3& p, double eps)
{
    return dvec3(f.eval(p + dvec3(eps, 0, 0)).distance - f.eval(p - dvec3(eps, 0, 0)).distance,
                 f.eval(p + dvec3(0, eps, 0)).distance - f.eval(p - dvec3(0, eps, 0)).distance,
                 f.eval(p + dvec3(0, 0, eps)).distance - f.eval(p - dvec3(0, 0, eps)).distance) /
           (2.0 * eps);
}

/**
 * A cell's edge along axis a at offsets (u, v) in the other two axes (y z for x, x z for y, x y for z), as 0-11. The
 * corner where it starts has a = 0.
 */
int edgeId(int a, int u, int v) { return a * 4 + u + 2 * v; }

ivec3 edgeStart(int e)
{
    const int a = e / 4, u = e & 1, v = (e >> 1) & 1;
    return a == 0 ? ivec3(0, u, v) : a == 1 ? ivec3(u, 0, v) : ivec3(u, v, 0);
}

/**
 * Which of a cell's crossing edges share a patch of surface, numbered from 0, -1 for edges the surface doesn't cross.
 * A cell the surface passes through twice gets a vertex per patch, so every edge of the mesh keeps two faces. A face
 * crossed on all four edges is split by the asymptotic decider, which both cells sharing it compute alike.
 */
std::array<int8_t, 12> patches(const double corner[8])
{
    auto value = [&](const ivec3& c) { return corner[c.x + 2 * c.y + 4 * c.z]; };
    std::array<int8_t, 12> parent;
    for (int e = 0; e < 12; ++e)
        parent[size_t(e)] = int8_t(e);
    auto root = [&](int e) {
        while (parent[size_t(e)] != e)
            e = parent[size_t(e)];
        return e;
    };
    auto join = [&](int a, int b) {
        a = root(a);
        b = root(b);
        if (a != b)
            parent[size_t(std::max(a, b))] = int8_t(std::min(a, b));
    };
    for (int n = 0; n < 3; ++n)
        for (int side = 0; side < 2; ++side)
        {
            const int pa = n == 0 ? 1 : 0, qa = n == 2 ? 1 : 2;
            ivec3 c[4];
            for (int k = 0; k < 4; ++k)
            {
                c[k][n] = side;
                c[k][pa] = (k == 1 || k == 2) ? 1 : 0;
                c[k][qa] = k >= 2 ? 1 : 0;
            }
            // Edge k runs from corner k to corner k + 1 round the face.
            int edge[4];
            bool cross[4];
            int crossings = 0;
            for (int k = 0; k < 4; ++k)
            {
                const ivec3 a = c[k], b = c[(k + 1) % 4];
                const ivec3 lo = glm::min(a, b);
                const int axis = a.x != b.x ? 0 : a.y != b.y ? 1 : 2;
                const int u = axis == 0 ? lo.y : lo.x, v = axis == 2 ? lo.y : lo.z;
                edge[k] = edgeId(axis, u, v);
                cross[k] = (value(a) < 0.0) != (value(b) < 0.0);
                crossings += cross[k] ? 1 : 0;
            }
            if (crossings == 2)
            {
                int first = -1;
                for (int k = 0; k < 4; ++k)
                    if (cross[k] && first < 0)
                        first = edge[k];
                    else if (cross[k])
                        join(first, edge[k]);
            }
            else if (crossings == 4)
            {
                const double v0 = value(c[0]), v1 = value(c[1]), v2 = value(c[2]), v3 = value(c[3]);
                const double den = v0 + v2 - v1 - v3;
                const double centre = den != 0.0 ? (v0 * v2 - v1 * v3) / den : 0.25 * (v0 + v1 + v2 + v3);
                // The corners whose sign the centre doesn't share are cut off on their own.
                if ((centre < 0.0) == (v0 < 0.0))
                {
                    join(edge[0], edge[1]);
                    join(edge[2], edge[3]);
                }
                else
                {
                    join(edge[3], edge[0]);
                    join(edge[1], edge[2]);
                }
            }
        }
    std::array<int8_t, 12> out;
    out.fill(-1);
    std::array<int8_t, 12> number;
    number.fill(-1);
    int8_t next = 0;
    for (int e = 0; e < 12; ++e)
    {
        const ivec3 a = edgeStart(e);
        ivec3 b = a;
        b[e / 4] = 1;
        if ((value(a) < 0.0) == (value(b) < 0.0))
            continue;
        const int r = root(e);
        if (number[size_t(r)] < 0)
            number[size_t(r)] = next++;
        out[size_t(e)] = number[size_t(r)];
    }
    return out;
}

Piece meshBrick(const Field::Local& f, const ivec3& b, double h, const MeshOptions& o)
{
    const ivec3 origin = b * kBrick - 1; // global lattice index of sample 0
    std::vector<double> d(size_t(kSamples) * kSamples * kSamples);
    auto at = [](int i, int j, int k) { return size_t(i) + size_t(kSamples) * (size_t(j) + size_t(kSamples) * size_t(k)); };
    for (int k = 0; k < kSamples; ++k)
        for (int j = 0; j < kSamples; ++j)
            for (int i = 0; i < kSamples; ++i)
                d[at(i, j, k)] = f.eval(dvec3(origin + ivec3(i, j, k)) * h).distance;
    auto solid = [&](int i, int j, int k) { return d[at(i, j, k)] < 0.0; };

    struct Cell
    {
        bool ready = false;
        std::array<int8_t, 12> patch{};
        int vert[6] = {-1, -1, -1, -1, -1, -1};
    };
    Piece piece;
    std::vector<Cell> cells(size_t(kCells) * kCells * kCells);
    // The vertex of the patch in cell (i, j, k) that the cell's edge e crosses.
    auto vertex = [&](int i, int j, int k, int e) -> uint32_t {
        Cell& cell = cells[size_t(i) + size_t(kCells) * (size_t(j) + size_t(kCells) * size_t(k))];
        double corner[8];
        for (int n = 0; n < 8; ++n)
            corner[n] = d[at(i + (n & 1), j + ((n >> 1) & 1), k + ((n >> 2) & 1))];
        if (!cell.ready)
        {
            cell.patch = patches(corner);
            cell.ready = true;
        }
        const int comp = cell.patch[size_t(e)];
        int& v = cell.vert[comp];
        if (v >= 0)
            return uint32_t(v);
        dvec3 sum(0.0);
        int count = 0;
        for (int edge = 0; edge < 12; ++edge)
        {
            if (cell.patch[size_t(edge)] != comp)
                continue;
            const ivec3 s = edgeStart(edge);
            ivec3 t = s;
            t[edge / 4] = 1;
            const double ds = corner[s.x + 2 * s.y + 4 * s.z], dt = corner[t.x + 2 * t.y + 4 * t.z];
            dvec3 p(s);
            p[edge / 4] += ds / (ds - dt);
            sum += p;
            ++count;
        }
        dvec3 p = (dvec3(origin + ivec3(i, j, k)) + sum / double(std::max(count, 1))) * h;
        for (int step = 0; step < o.projectSteps; ++step)
        {
            const dvec3 g = gradient(f, p, 0.05 * h);
            const double g2 = glm::dot(g, g);
            if (g2 < 1e-12)
                break;
            dvec3 move = -f.eval(p).distance * g / g2;
            if (double len = glm::length(move); len > h)
                move *= h / len;
            p += move;
        }
        v = int(piece.points.size());
        piece.cells.push_back(pack(origin + ivec3(i, j, k)) * 8 + uint64_t(comp));
        piece.points.push_back(p);
        piece.colors.push_back(f.eval(p).color);
        return uint32_t(v);
    };
    // Each crossing edge's quad joins the four cells around it; offsets give the edge's place in each cell.
    auto quad = [&](bool lowSolid, int axis, const std::array<ivec3, 4>& cell, const std::array<glm::ivec2, 4>& offset) {
        uint32_t q[4];
        for (int n = 0; n < 4; ++n)
            q[n] = vertex(cell[size_t(n)].x, cell[size_t(n)].y, cell[size_t(n)].z, edgeId(axis, offset[size_t(n)].x, offset[size_t(n)].y));
        if (!lowSolid)
            std::swap(q[1], q[3]);
        piece.quads.insert(piece.quads.end(), q, q + 4);
    };
    using C = std::array<ivec3, 4>;
    using O = std::array<glm::ivec2, 4>;
    // The brick owns the edges leaving its own lattice points (samples 1 to kBrick), each quad facing from solid to empty.
    for (int k = 1; k <= kBrick; ++k)
        for (int j = 1; j <= kBrick; ++j)
            for (int i = 1; i <= kBrick; ++i)
            {
                const bool here = solid(i, j, k);
                if (here != solid(i + 1, j, k))
                    quad(here, 0, C{ivec3(i, j - 1, k - 1), ivec3(i, j, k - 1), ivec3(i, j, k), ivec3(i, j - 1, k)},
                         O{glm::ivec2(1, 1), glm::ivec2(0, 1), glm::ivec2(0, 0), glm::ivec2(1, 0)});
                if (here != solid(i, j + 1, k))
                    quad(here, 1, C{ivec3(i - 1, j, k - 1), ivec3(i - 1, j, k), ivec3(i, j, k), ivec3(i, j, k - 1)},
                         O{glm::ivec2(1, 1), glm::ivec2(1, 0), glm::ivec2(0, 0), glm::ivec2(0, 1)});
                if (here != solid(i, j, k + 1))
                    quad(here, 2, C{ivec3(i - 1, j - 1, k), ivec3(i, j - 1, k), ivec3(i, j, k), ivec3(i - 1, j, k)},
                         O{glm::ivec2(1, 1), glm::ivec2(0, 1), glm::ivec2(0, 0), glm::ivec2(1, 0)});
            }
    return piece;
}

struct IvecHash
{
    size_t operator()(const ivec3& v) const { return size_t(pack(v) * 0x9E3779B97F4A7C15ull); }
};
} // namespace

struct Mesher::State
{
    std::optional<Field> field;
    MeshOptions options;
    std::unordered_map<ivec3, Piece, IvecHash> bricks;
    size_t rebuilt = 0;
    geo::Mesh last;
};

Mesher::Mesher() : mState(std::make_unique<State>()) {}
Mesher::~Mesher() = default;
Mesher::Mesher(Mesher&&) noexcept = default;
Mesher& Mesher::operator=(Mesher&&) noexcept = default;

size_t Mesher::lastRebuilt() const { return mState->rebuilt; }
size_t Mesher::brickCount() const { return mState->bricks.size(); }

geo::Mesh Mesher::update(const Field& field, const MeshOptions& options)
{
    LILA_ZONE();

    State& s = *mState;
    const double h = options.voxelSize;
    if (!(h > 0.0))
        throw std::invalid_argument("clay: voxel size must be above 0");

    const bool sameMeshing = s.field && s.options.voxelSize == h && s.options.projectSteps == options.projectSteps;
    std::optional<std::vector<Bounds>> regions;
    if (sameMeshing)
        regions = Field::changes(*s.field, field);
    if (sameMeshing && regions && regions->empty() && s.options.colors == options.colors)
    {
        s.rebuilt = 0;
        return s.last;
    }

    // The bricks to (re)build: every brick the change can reach, or every brick near an added bound.
    auto bricksOver = [&](const Bounds& r, std::vector<ivec3>& out) {
        const ivec3 lo(glm::floor(r.min / (kBrick * h)));
        const ivec3 hi(glm::floor(r.max / (kBrick * h)));
        const glm::dvec3 span = glm::dvec3(hi - lo) + 1.0;
        if (span.x * span.y * span.z > double(options.maxBricks))
            throw std::runtime_error("clay: voxel size too small for the clay's size; raise it");
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x)
                    out.emplace_back(x, y, z);
    };
    std::vector<ivec3> todo;
    const bool full = !sameMeshing || !regions;
    if (full)
    {
        s.bricks.clear();
        for (const Bounds& a : field.addBounds())
            bricksOver(a.grown(2.0 * h), todo);
    }
    else
        for (const Bounds& r : *regions)
            bricksOver(r.grown(3.0 * h), todo);
    std::sort(todo.begin(), todo.end(), [](const ivec3& a, const ivec3& b) { return pack(a) < pack(b); });
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
    if (todo.size() > options.maxBricks)
        throw std::runtime_error("clay: voxel size too small for the clay's size; raise it");

    std::vector<std::optional<Piece>> built(todo.size());
    tbb::parallel_for(size_t(0), todo.size(), [&](size_t n) {
        const ivec3 b = todo[n];
        const Field::Local local = field.local(brickBounds(b, h), 3.0 * h);
        if (live(field, local, b, h))
            built[n] = meshBrick(local, b, h, options);
    });
    for (size_t n = 0; n < todo.size(); ++n)
        if (built[n] && !built[n]->quads.empty())
            s.bricks[todo[n]] = std::move(*built[n]);
        else
            s.bricks.erase(todo[n]);
    if (s.bricks.size() > options.maxBricks)
        throw std::runtime_error("clay: voxel size too small for the clay's size; raise it");

    s.field = field;
    s.options = options;
    s.rebuilt = todo.size();

    // Weld in brick order, so a rebuild and an update give the same mesh.
    std::vector<ivec3> keys;
    keys.reserve(s.bricks.size());
    for (const auto& [k, piece] : s.bricks)
        keys.push_back(k);
    std::sort(keys.begin(), keys.end(), [](const ivec3& a, const ivec3& b) { return pack(a) < pack(b); });

    std::unordered_map<uint64_t, geo::Index> index;
    std::vector<dvec3> points;
    std::vector<glm::vec4> colors;
    std::vector<geo::Index> quads;
    bool tinted = false;
    for (const ivec3& k : keys)
    {
        const Piece& piece = s.bricks.at(k);
        std::vector<geo::Index> local(piece.points.size());
        for (size_t v = 0; v < piece.points.size(); ++v)
        {
            auto [it, fresh] = index.try_emplace(piece.cells[v], geo::Index(points.size()));
            if (fresh)
            {
                points.push_back(piece.points[v]);
                colors.emplace_back(piece.colors[v], 1.0f);
                tinted = tinted || piece.colors[v] != glm::vec3(1.0f);
            }
            local[v] = it->second;
        }
        for (uint32_t q : piece.quads)
            quads.push_back(local[q]);
    }
    std::vector<geo::Index> sizes(quads.size() / 4, 4);
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, quads);
    if (options.colors && tinted && m.pointCapacity() == points.size())
    {
        geo::colors::add(m, "clay");
        geo::colors::setFromPoints(m, "clay", colors);
    }
    s.last = m;
    return m;
}

geo::Mesh mesh(const Field& field, const MeshOptions& options)
{
    Mesher m;
    return m.update(field, options);
}
} // namespace lila::clay
