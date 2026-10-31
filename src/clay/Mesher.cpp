#include "clay/Clay.h"

#include "core/Profile.h"
#include "geo/ColorSets.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace lila::clay
{
namespace
{
using glm::dvec3;
using glm::ivec3;

constexpr int kBrick = kBrickCells;
// Lattice samples per brick axis: the brick's cells, one cell of apron below and one above, so every cell around an
// edge the brick owns is in reach.
constexpr int kSamples = kBrickSamples;
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

/**
 * Dual contouring's vertex for one patch of a cell: the point closest to the tangent planes at its edge crossings (the
 * field's normals there), pulled toward their average so flat patches stay put, and kept inside the cell. Corners and
 * edges of hard shapes come out sharp.
 */
dvec3 sharpVertex(const Field::Local& f, const double corner[8], const std::array<int8_t, 12>& patch, int comp, const ivec3& cell, double h,
                  const dvec3& average)
{
    glm::dmat3 ata(0.0);
    dvec3 atb(0.0);
    const dvec3 base = dvec3(cell) * h;
    const dvec3 mass = base + average * h;
    const double eps = 0.02 * h;
    for (int edge = 0; edge < 12; ++edge)
    {
        if (patch[size_t(edge)] != comp)
            continue;
        const ivec3 s = edgeStart(edge);
        ivec3 t = s;
        t[edge / 4] = 1;
        const double ds = corner[s.x + 2 * s.y + 4 * s.z], dt = corner[t.x + 2 * t.y + 4 * t.z];
        // The crossing found on the field itself: interpolating the corner values drifts where distance curves round a corner.
        const dvec3 a = base + dvec3(s) * h, b = base + dvec3(t) * h;
        double lo = 0.0, hi = 1.0, flo = ds, fhi = dt;
        for (int it = 0; it < 6; ++it)
        {
            const double x = std::clamp(lo + (hi - lo) * flo / (flo - fhi), lo + 0.01 * (hi - lo), hi - 0.01 * (hi - lo));
            const double fx = f.eval(glm::mix(a, b, x)).distance;
            ((fx < 0.0) == (flo < 0.0) ? lo : hi) = x;
            ((fx < 0.0) == (flo < 0.0) ? flo : fhi) = fx;
        }
        const dvec3 q = glm::mix(a, b, lo + (hi - lo) * flo / (flo - fhi));
        dvec3 n(f.eval(q + dvec3(eps, 0, 0)).distance - f.eval(q - dvec3(eps, 0, 0)).distance,
                f.eval(q + dvec3(0, eps, 0)).distance - f.eval(q - dvec3(0, eps, 0)).distance,
                f.eval(q + dvec3(0, 0, eps)).distance - f.eval(q - dvec3(0, 0, eps)).distance);
        const double len = glm::length(n);
        if (len < 1e-12)
            continue;
        n /= len;
        ata += glm::outerProduct(n, n);
        atb += n * glm::dot(n, q - mass);
    }
    // Solved about the average, with a small pull toward it so a flat or curved patch doesn't run off along its plane.
    const double pull = 0.05;
    const dvec3 x = mass + glm::inverse(ata + glm::dmat3(pull)) * atb;
    // A margin inside the cell: on its boundary, neighbouring cells can land on one point and leave zero-area faces.
    return glm::clamp(x, base + 0.05 * h, base + 0.95 * h);
}

/** The brick's lattice of distances, x fastest, sampled on the CPU. */
std::vector<double> cpuSamples(const Field::Local& f, const ivec3& b, double h)
{
    const ivec3 origin = b * kBrick - 1; // global lattice index of sample 0
    std::vector<double> d(size_t(kSamples) * kSamples * kSamples, std::numeric_limits<double>::quiet_NaN());
    auto at = [](int i, int j, int k) { return size_t(i) + size_t(kSamples) * (size_t(j) + size_t(kSamples) * size_t(k)); };
    auto sample = [&](int i, int j, int k) {
        double& v = d[at(i, j, k)];
        if (std::isnan(v))
            v = f.eval(dvec3(origin + ivec3(i, j, k)) * h).distance;
        return v;
    };
    // Sub-blocks of 2^3 cells, sampled at their corners first: one whose corners all sit further from the surface than
    // its diagonal (with slack, as in live()) holds no surface, so its inside only needs the corners' sign.
    constexpr int kBlocks = (kSamples - 1) / 2;
    const double empty = 1.5 * 2.0 * std::sqrt(3.0) * h;
    std::vector<int8_t> blockSign(size_t(kBlocks) * kBlocks * kBlocks, 0);
    for (int c = 0; c < kBlocks; ++c)
        for (int b2 = 0; b2 < kBlocks; ++b2)
            for (int a = 0; a < kBlocks; ++a)
            {
                double nearest = 1e300;
                int negative = 0;
                for (int n = 0; n < 8; ++n)
                {
                    const double v = sample(2 * a + 2 * (n & 1), 2 * b2 + ((n >> 1) & 1) * 2, 2 * c + ((n >> 2) & 1) * 2);
                    nearest = std::min(nearest, std::abs(v));
                    negative += v < 0.0 ? 1 : 0;
                }
                if (nearest > empty && (negative == 0 || negative == 8))
                    blockSign[size_t(a) + size_t(kBlocks) * (size_t(b2) + size_t(kBlocks) * size_t(c))] = negative ? -1 : 1;
            }
    for (int c = 0; c < kBlocks; ++c)
        for (int b2 = 0; b2 < kBlocks; ++b2)
            for (int a = 0; a < kBlocks; ++a)
                if (blockSign[size_t(a) + size_t(kBlocks) * (size_t(b2) + size_t(kBlocks) * size_t(c))] == 0)
                    for (int n = 0; n < 27; ++n)
                        sample(2 * a + n % 3, 2 * b2 + (n / 3) % 3, 2 * c + n / 9);
    // Whatever is left lies only in empty sub-blocks, all on one side of the surface.
    for (int k = 0; k < kSamples; ++k)
        for (int j = 0; j < kSamples; ++j)
            for (int i = 0; i < kSamples; ++i)
                if (double& v = d[at(i, j, k)]; std::isnan(v))
                {
                    const int a = std::min(i / 2, kBlocks - 1), b2 = std::min(j / 2, kBlocks - 1), c = std::min(k / 2, kBlocks - 1);
                    v = empty * blockSign[size_t(a) + size_t(kBlocks) * (size_t(b2) + size_t(kBlocks) * size_t(c))];
                }
    return d;
}

Piece meshBrick(const Field::Local& f, const ivec3& b, double h, const MeshOptions& o, std::vector<double> d)
{
    const ivec3 origin = b * kBrick - 1;
    auto at = [](int i, int j, int k) { return size_t(i) + size_t(kSamples) * (size_t(j) + size_t(kSamples) * size_t(k)); };
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
        const dvec3 t = sum / double(std::max(count, 1));
        dvec3 p = (dvec3(origin + ivec3(i, j, k)) + t) * h;
        // The gradient of the cell's trilinear field: no extra samples, and close enough to step onto the surface.
        dvec3 g(0.0);
        for (int n = 0; n < 8; ++n)
        {
            const dvec3 c(n & 1, (n >> 1) & 1, (n >> 2) & 1);
            const dvec3 w = c * t + (1.0 - c) * (1.0 - t);
            const dvec3 sign = 2.0 * c - 1.0;
            g += corner[n] * dvec3(sign.x * w.y * w.z, w.x * sign.y * w.z, w.x * w.y * sign.z);
        }
        g /= h;
        Sample here = f.eval(p);
        if (o.sharp)
        {
            p = sharpVertex(f, corner, cell.patch, comp, origin + ivec3(i, j, k), h, t);
            here = f.eval(p);
        }
        for (int step = 0; step < (o.sharp ? 0 : o.projectSteps); ++step)
        {
            const double g2 = glm::dot(g, g);
            if (g2 < 1e-12)
                break;
            dvec3 move = -here.distance * g / g2;
            if (double len = glm::length(move); len > h)
                move *= h / len;
            p += move;
            if (step + 1 < o.projectSteps)
                here = f.eval(p);
        }
        // Inside the cell, short of its faces: vertices of neighbouring cells could otherwise meet and leave zero-area faces.
        const dvec3 cellMin = dvec3(origin + ivec3(i, j, k)) * h;
        p = glm::clamp(p, cellMin + 0.001 * h, cellMin + 0.999 * h);
        v = int(piece.points.size());
        piece.cells.push_back(pack(origin + ivec3(i, j, k)) * 8 + uint64_t(comp));
        piece.points.push_back(p);
        piece.colors.push_back(here.color);
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

std::mutex gBackendMutex;
std::shared_ptr<SampleBackend> gBackend;

std::shared_ptr<SampleBackend> sampleBackend()
{
    std::scoped_lock lock(gBackendMutex);
    return gBackend;
}
} // namespace

void setSampleBackend(std::shared_ptr<SampleBackend> backend)
{
    std::scoped_lock lock(gBackendMutex);
    gBackend = std::move(backend);
}

struct Mesher::State
{
    std::optional<Field> field;
    MeshOptions options;
    bool gpu = false; // how the cached bricks were sampled
    std::unordered_map<ivec3, Piece, IvecHash> bricks;
    size_t rebuilt = 0;
    std::shared_ptr<const geo::Mesh> last;
};

Mesher::Mesher() : mState(std::make_unique<State>()) {}
Mesher::~Mesher() = default;
Mesher::Mesher(Mesher&&) noexcept = default;
Mesher& Mesher::operator=(Mesher&&) noexcept = default;

size_t Mesher::lastRebuilt() const { return mState->rebuilt; }
size_t Mesher::brickCount() const { return mState->bricks.size(); }

std::shared_ptr<const geo::Mesh> Mesher::update(const Field& field, const MeshOptions& options)
{
    LILA_ZONE();

    State& s = *mState;
    const double h = options.voxelSize;
    if (!(h > 0.0))
        throw std::invalid_argument("clay: voxel size must be above 0");

    std::shared_ptr<SampleBackend> backend = options.gpu ? sampleBackend() : nullptr;
    // Bricks sampled one way never weld to bricks sampled the other: float and double disagree on signs at the surface.
    const bool sameMeshing = s.field && s.options.voxelSize == h && s.options.projectSteps == options.projectSteps &&
                             s.options.sharp == options.sharp && s.gpu == bool(backend);
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
            bricksOver(r.grown(4.0 * h), todo);
    std::sort(todo.begin(), todo.end(), [](const ivec3& a, const ivec3& b) { return pack(a) < pack(b); });
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
    if (todo.size() > options.maxBricks)
        throw std::runtime_error("clay: voxel size too small for the clay's size; raise it");

    std::vector<std::optional<Field::Local>> locals(todo.size());
    tbb::parallel_for(size_t(0), todo.size(), [&](size_t n) {
        Field::Local local = field.local(brickBounds(todo[n], h), 4.0 * h);
        if (live(field, local, todo[n], h))
            locals[n] = std::move(local);
    });
    std::vector<float> gpuSamples;
    std::vector<size_t> gpuSlot(todo.size(), SIZE_MAX);
    if (backend)
    {
        std::vector<float> prims;
        std::vector<int32_t> code, localCode;
        std::vector<ivec3> bricks;
        std::vector<uint32_t> start, count;
        field.gpuProgram(prims, code);
        for (size_t n = 0; n < todo.size(); ++n)
            if (locals[n])
            {
                gpuSlot[n] = bricks.size();
                bricks.push_back(todo[n]);
                start.push_back(uint32_t(localCode.size()));
                count.push_back(uint32_t(locals[n]->code().size()));
                localCode.insert(localCode.end(), locals[n]->code().begin(), locals[n]->code().end());
            }
        if (!bricks.empty() && !backend->sample(prims, code, field.symmetric(), bricks, localCode, start, count, h, gpuSamples))
        {
            // The GPU declined: everything again on the CPU, so no brick mixes the two.
            MeshOptions cpu = options;
            cpu.gpu = false;
            s.field.reset();
            return update(field, cpu);
        }
    }
    std::vector<std::optional<Piece>> built(todo.size());
    tbb::parallel_for(size_t(0), todo.size(), [&](size_t n) {
        if (!locals[n])
            return;
        std::vector<double> d;
        if (gpuSlot[n] != SIZE_MAX)
        {
            const float* from = gpuSamples.data() + gpuSlot[n] * size_t(kSamples * kSamples * kSamples);
            d.assign(from, from + size_t(kSamples * kSamples * kSamples));
        }
        else
            d = cpuSamples(*locals[n], todo[n], h);
        built[n] = meshBrick(*locals[n], todo[n], h, options, std::move(d));
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
    s.gpu = bool(backend);
    s.rebuilt = todo.size();

    // Weld in brick order, so a rebuild and an update give the same mesh.
    std::vector<ivec3> keys;
    keys.reserve(s.bricks.size());
    for (const auto& [k, piece] : s.bricks)
        keys.push_back(k);
    std::sort(keys.begin(), keys.end(), [](const ivec3& a, const ivec3& b) { return pack(a) < pack(b); });

    std::vector<const Piece*> pieces;
    size_t candidates = 0, quadCount = 0;
    for (const ivec3& k : keys)
    {
        pieces.push_back(&s.bricks.at(k));
        candidates += pieces.back()->points.size();
        quadCount += pieces.back()->quads.size();
    }
    // Open addressing on the cell keys: neighbouring bricks repeat their shared cells, and this runs on every edit.
    size_t slots = 1;
    while (slots < 2 * candidates + 1)
        slots <<= 1;
    std::vector<uint64_t> slotKey(slots, ~uint64_t(0));
    std::vector<geo::Index> slotIndex(slots);
    std::vector<dvec3> points;
    std::vector<glm::vec4> colors;
    std::vector<geo::Index> quads;
    points.reserve(candidates);
    colors.reserve(candidates);
    quads.reserve(quadCount);
    bool tinted = false;
    std::vector<geo::Index> local;
    for (const Piece* piece : pieces)
    {
        local.resize(piece->points.size());
        for (size_t v = 0; v < piece->points.size(); ++v)
        {
            const uint64_t key = piece->cells[v];
            size_t slot = size_t((key * 0x9E3779B97F4A7C15ull) >> 17) & (slots - 1);
            while (slotKey[slot] != key && slotKey[slot] != ~uint64_t(0))
                slot = (slot + 1) & (slots - 1);
            if (slotKey[slot] != key)
            {
                slotKey[slot] = key;
                slotIndex[slot] = geo::Index(points.size());
                points.push_back(piece->points[v]);
                colors.emplace_back(piece->colors[v], 1.0f);
                tinted = tinted || piece->colors[v] != glm::vec3(1.0f);
            }
            local[v] = slotIndex[slot];
        }
        for (uint32_t q : piece->quads)
            quads.push_back(local[q]);
    }
    std::vector<geo::Index> sizes(quads.size() / 4, 4);
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, quads);
    if (options.colors && tinted && m.pointCapacity() == points.size())
    {
        geo::colors::add(m, "clay");
        geo::colors::setFromPoints(m, "clay", colors);
    }
    s.last = std::make_shared<const geo::Mesh>(std::move(m));
    return s.last;
}

geo::Mesh mesh(const Field& field, const MeshOptions& options)
{
    Mesher m;
    return *m.update(field, options);
}
} // namespace lila::clay
