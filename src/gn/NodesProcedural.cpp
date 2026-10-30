#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_map>

namespace lila::gn::nodes::procedural
{
using D3 = glm::dvec3;
inline constexpr D3 kUp{0.0, 1.0, 0.0};

std::string trimText(std::string_view s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos)
        return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

/** Pieces between any of the separators, trimmed; empty pieces dropped. */
std::vector<std::string> splitText(std::string_view s, std::string_view seps)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t k = 0; k <= s.size(); ++k)
        if (k == s.size() || seps.find(s[k]) != std::string_view::npos)
        {
            std::string piece = trimText(s.substr(start, k - start));
            if (!piece.empty())
                out.push_back(std::move(piece));
            start = k + 1;
        }
    return out;
}

std::vector<double> parseNumbers(std::string_view s)
{
    std::vector<double> out;
    for (const std::string& p : splitText(s, ", \t\n;"))
    {
        try
        {
            out.push_back(std::stod(p));
        }
        catch (const std::exception&)
        {
            throw ExecError("not a number: " + p);
        }
    }
    return out;
}

/** Positions of every mesh point, cloud point and curve point, and every instance's translation. */
std::vector<D3> allPositions(const Geometry& g)
{
    std::vector<D3> out;
    for (const auto* m : {g.mesh.get(), g.points.get()})
        if (m)
            for (Index p = 0; p < m->pointCapacity(); ++p)
                if (m->pointValid(p))
                    out.push_back(m->position(p));
    if (g.curves)
        out.insert(out.end(), g.curves->positions.begin(), g.curves->positions.end());
    if (g.instances)
        for (const glm::dmat4& xf : g.instances->transforms)
            out.push_back(D3(xf[3]));
    return out;
}

/** Indices bucketed by cell, for neighbour queries within one cell. */
struct CellGrid
{
    double cell = 1.0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

    static uint64_t key(int64_t x, int64_t y, int64_t z) { return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff); }
    glm::i64vec3 coord(const D3& p) const { return glm::i64vec3(glm::floor(p / cell)); }
    void add(const D3& p, uint32_t i)
    {
        glm::i64vec3 c = coord(p);
        cells[key(c.x, c.y, c.z)].push_back(i);
    }
    template <class F> void around(const D3& p, F&& fn) const
    {
        glm::i64vec3 c = coord(p);
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                    if (auto it = cells.find(key(c.x + dx, c.y + dy, c.z + dz)); it != cells.end())
                        for (uint32_t i : it->second)
                            fn(i);
    }
};

glm::dvec3 matrixToEulerCols(const D3& x, const D3& y, const D3& z) { return matrixToEuler(glm::dmat3(x, y, z)); }

// ---------------------------------------------------------------------------------------------------------------------
// Poisson disk

struct Candidate
{
    D3 pos;
    D3 normal;
    double radius = 0.0;
    double density = 1.0;
    int cls = -1;
    uint32_t hash = 0;
};

void poissonDisk()
{
    registerDecl({
        .type = "PoissonDiskPoints",
        .label = "Poisson Disk Points",
        .category = "Procedural",
        .doc = "Points spaced by their radius (two points keep the mean of their radii apart), on the mesh's faces or in a box (flat "
               "when its Y size is 0). With classes, larger radii place first and each point gets a \"class\". Points stay clear of avoid "
               "by its radius attribute or avoidRadius, or their own radius when larger. On a surface, the radius and density fields are read at the mesh's points and blended across faces.",
        .props = {prop("mode", {"surface", "box"}, "surface")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                Sockets s;
                if (findDecl("PoissonDiskPoints")->propValue(n, "mode") == "box")
                {
                    s.inputs.push_back(in("size", T::Vector, vec(1000, 0, 1000), "Box size, cm."));
                    s.inputs.push_back(in("center", T::Vector, vec(0, 0, 0)));
                }
                else
                    s.inputs.push_back(geoIn("mesh"));
                s.inputs.push_back(bounded(fin("radius", T::Float, 50.0, Implicit::None, "Spacing per point, cm."), 0.0));
                s.inputs.push_back(bounded(fin("density", T::Float, 1.0, Implicit::None, "Chance a point is kept: a mask from 0 to 1."), 0.0, 1.0));
                s.inputs.push_back(in("classes", T::String, "", "Class radii, comma separated (\"200, 80, 30\"). Overrides radius."));
                s.inputs.push_back(geoIn("avoid", "Points new points keep clear of."));
                s.inputs.push_back(bounded(in("avoidRadius", T::Float, 0.0, "Clearance around avoid points without a radius attribute, cm."), 0.0));
                s.inputs.push_back(in("seed", T::Int, 0));
                s.outputs = {geoOut("points"), fout("normal", T::Vector), fout("rotation", T::Rotation)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::string normalName = c.anonymous("normal"), rotName = c.anonymous("rotation");
                c.out("normal", attributeField(normalName, T::Vector));
                c.out("rotation", attributeField(rotName, T::Rotation));
                uint32_t seed = uint32_t(c.i("seed"));
                std::vector<double> classes;
                for (double r : parseNumbers(c.s("classes")))
                    if (r > 0.0)
                        classes.push_back(r);
                std::sort(classes.begin(), classes.end(), std::greater<>());
                const size_t kMaxCandidates = 4000000;
                const double kCandidatesPerCell = 8.0;
                std::vector<Candidate> cands;

                auto classOf = [&](uint32_t h) { return int(hash32(h, seed, 0xc1a5) % uint32_t(classes.size())); };

                if (c.prop("mode") == "box")
                {
                    D3 size = glm::max(c.v("size"), D3(0.0)), center = c.v("center");
                    bool flat = size.y <= 1e-9;
                    D3 lo = center - size * 0.5;
                    // The radius field may vary over the box: probe it to size the candidate density.
                    double rmin = classes.empty() ? 0.0 : classes.back();
                    if (classes.empty())
                    {
                        geo::Mesh probe;
                        for (uint32_t k = 0; k < 4096; ++k)
                        {
                            D3 u(hashToUnit(hash32(k, seed, 1)), hashToUnit(hash32(k, seed, 2)), hashToUnit(hash32(k, seed, 3)));
                            probe.addPoint(lo + u * size);
                        }
                        double mn = std::numeric_limits<double>::max();
                        for (double r : evaluateAll<double>(c.field("radius"), FieldContext::forPoints(probe)))
                            if (r > 0.0)
                                mn = std::min(mn, r);
                        rmin = mn == std::numeric_limits<double>::max() ? 0.0 : mn;
                    }
                    if (rmin <= 0.0)
                        throw ExecError("the radius must be above 0");
                    double amount = flat ? size.x * size.z / (rmin * rmin) : size.x * size.y * size.z / (rmin * rmin * rmin) * 2.0;
                    amount *= kCandidatesPerCell;
                    if (amount > double(kMaxCandidates))
                        throw ExecError("too many points to place: raise the radius or shrink the box");
                    size_t n = size_t(amount);
                    cands.resize(n);
                    geo::Mesh cloud;
                    for (size_t k = 0; k < n; ++k)
                    {
                        uint32_t h = hash32(uint32_t(k), seed, 0x9e37);
                        D3 u(hashToUnit(hash32(h, 1)), flat ? 0.5 : hashToUnit(hash32(h, 2)), hashToUnit(hash32(h, 3)));
                        cands[k].pos = lo + u * size;
                        cands[k].normal = kUp;
                        cands[k].hash = h;
                        cloud.addPoint(cands[k].pos);
                    }
                    FieldContext ctx = FieldContext::forPoints(cloud);
                    std::vector<double> radius = evaluateAll<double>(c.field("radius"), ctx), density = evaluateAll<double>(c.field("density"), ctx);
                    for (size_t k = 0; k < n; ++k)
                    {
                        cands[k].radius = radius[k];
                        cands[k].density = density[k];
                    }
                }
                else
                {
                    GeometryPtr g = c.geometry("mesh");
                    if (!g->mesh)
                    {
                        c.out("points", makeGeometry({}));
                        return;
                    }
                    const geo::Mesh& m = *g->mesh;
                    FieldContext pctx = FieldContext::forMesh(m, Domain::Point);
                    std::vector<double> vr = evaluateAll<double>(c.field("radius"), pctx), vd = evaluateAll<double>(c.field("density"), pctx);
                    double rmin = classes.empty() ? std::numeric_limits<double>::max() : classes.back();
                    if (classes.empty())
                        for (double r : vr)
                            if (r > 0.0)
                                rmin = std::min(rmin, r);
                    if (rmin == std::numeric_limits<double>::max())
                        throw ExecError("the radius must be above 0");
                    double perArea = kCandidatesPerCell / (rmin * rmin);
                    geo::Triangulation tris = geo::triangulate(m);
                    size_t nt = tris.faces.size();
                    std::vector<size_t> first(nt + 1, 0);
                    for (size_t t = 0; t < nt; ++t)
                    {
                        D3 a = m.position(m.cornerPoint(tris.corners[t * 3])), b = m.position(m.cornerPoint(tris.corners[t * 3 + 1])),
                           cc = m.position(m.cornerPoint(tris.corners[t * 3 + 2]));
                        double expected = 0.5 * glm::length(glm::cross(b - a, cc - a)) * perArea;
                        double whole = std::floor(expected);
                        first[t + 1] = first[t] + size_t(whole) + (hashToUnit(hash32(uint32_t(t), seed, 0x51)) < expected - whole ? 1 : 0);
                        if (first[t + 1] > kMaxCandidates)
                            throw ExecError("too many points to place: raise the radius");
                    }
                    cands.resize(first[nt]);
                    tbb::parallel_for(size_t(0), nt, [&](size_t t) {
                        Index p[3];
                        D3 v[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            p[k] = m.cornerPoint(tris.corners[t * 3 + k]);
                            v[k] = m.position(p[k]);
                        }
                        D3 nrm = m.faceNormal(tris.faces[t]);
                        for (size_t i = first[t]; i < first[t + 1]; ++i)
                        {
                            uint32_t h = hash32(uint32_t(t), uint32_t(i - first[t]), seed);
                            double s = std::sqrt(hashToUnit(hash32(h, 1))), u = hashToUnit(hash32(h, 2));
                            double w[3] = {1.0 - s, s * (1.0 - u), s * u};
                            Candidate& cd = cands[i];
                            cd.pos = v[0] * w[0] + v[1] * w[1] + v[2] * w[2];
                            cd.normal = nrm;
                            cd.radius = vr[p[0]] * w[0] + vr[p[1]] * w[1] + vr[p[2]] * w[2];
                            cd.density = vd[p[0]] * w[0] + vd[p[1]] * w[1] + vd[p[2]] * w[2];
                            cd.hash = h;
                        }
                    });
                }

                for (Candidate& cd : cands)
                    if (!classes.empty())
                    {
                        cd.cls = classOf(cd.hash);
                        cd.radius = classes[size_t(cd.cls)];
                    }
                std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
                    if (a.cls != b.cls)
                        return a.cls < b.cls;
                    return a.hash < b.hash;
                });

                std::vector<D3> avoid = allPositions(*c.geometry("avoid"));
                std::vector<double> avoidR(avoid.size(), c.f("avoidRadius"));
                {
                    GeometryPtr ag = c.geometry("avoid");
                    size_t at = 0;
                    for (const auto* m : {ag->mesh.get(), ag->points.get()})
                    {
                        if (!m)
                            continue;
                        auto radii = m->pointAttrs().get<float>("radius");
                        size_t k = 0;
                        for (Index p = 0; p < m->pointCapacity(); ++p)
                            if (m->pointValid(p))
                            {
                                if (p < radii.size())
                                    avoidR[at + k] = std::max(avoidR[at + k], double(radii[p]));
                                ++k;
                            }
                        at += k;
                    }
                }

                double cell = 1e-6;
                for (const Candidate& cd : cands)
                    cell = std::max(cell, cd.radius);
                for (double r : avoidR)
                    cell = std::max(cell, r);
                CellGrid grid{cell, {}};
                std::vector<D3> placedPos;
                std::vector<double> placedR;
                for (size_t k = 0; k < avoid.size(); ++k)
                {
                    grid.add(avoid[k], uint32_t(placedPos.size()));
                    placedPos.push_back(avoid[k]);
                    placedR.push_back(avoidR[k]);
                }
                std::vector<size_t> kept;
                for (size_t i = 0; i < cands.size(); ++i)
                {
                    const Candidate& cd = cands[i];
                    if (cd.radius <= 0.0 || hashToUnit(hash32(cd.hash, seed, 0x77)) >= cd.density)
                        continue;
                    bool blocked = false;
                    grid.around(cd.pos, [&](uint32_t j) {
                        double need = j < avoid.size() ? std::max(cd.radius, placedR[j]) : 0.5 * (cd.radius + placedR[j]);
                        if (!blocked && glm::distance(cd.pos, placedPos[j]) < need)
                            blocked = true;
                    });
                    if (blocked)
                        continue;
                    grid.add(cd.pos, uint32_t(placedPos.size()));
                    placedPos.push_back(cd.pos);
                    placedR.push_back(cd.radius);
                    kept.push_back(i);
                }

                geo::Mesh pts;
                std::vector<D3> outN, outR;
                std::vector<double> outRadius;
                std::vector<int64_t> outId, outCls;
                for (size_t i : kept)
                {
                    const Candidate& cd = cands[i];
                    pts.addPoint(cd.pos);
                    outN.push_back(cd.normal);
                    outR.push_back(matrixToEuler(rotationBetween(kUp, cd.normal)));
                    outRadius.push_back(cd.radius);
                    outId.push_back(int64_t(cd.hash & 0x7fffffff));
                    outCls.push_back(cd.cls);
                }
                writeAttribute(pts, "id", Domain::Point, Column::of(std::move(outId)), T::Int);
                writeAttribute(pts, "radius", Domain::Point, Column::of(std::move(outRadius)), T::Float);
                if (!classes.empty())
                    writeAttribute(pts, "class", Domain::Point, Column::of(std::move(outCls)), T::Int);
                writeAttribute(pts, normalName, Domain::Point, Column::of(std::move(outN)), T::Vector);
                writeAttribute(pts, rotName, Domain::Point, Column::of(std::move(outR)), T::Rotation);
                c.out("points", pointGeometry(std::move(pts)));
            },
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Wave function collapse

inline constexpr std::array<D3, 6> kDirs = {D3(1, 0, 0), D3(-1, 0, 0), D3(0, 1, 0), D3(0, -1, 0), D3(0, 0, 1), D3(0, 0, -1)};

int dirIndex(const D3& v)
{
    int best = 0;
    for (int d = 1; d < 6; ++d)
        if (glm::dot(v, kDirs[size_t(d)]) > glm::dot(v, kDirs[size_t(best)]))
            best = d;
    return best;
}

/** Turned by quarter turns about Y, the way composeTransform turns a Y Euler angle. */
D3 rotateY(const D3& v, int quarters)
{
    double a = quarters * kPi * 0.5;
    double cs = std::cos(a), sn = std::sin(a);
    return {v.x * cs + v.z * sn, v.y, -v.x * sn + v.z * cs};
}

int sideIndex(std::string_view s)
{
    static const char* names[6] = {"+x", "-x", "+y", "-y", "+z", "-z"};
    for (int d = 0; d < 6; ++d)
        if (s == names[d])
            return d;
    return -1;
}

/** Sides' labels per tile from "0: +x=a -x=b; 1: ...". */
std::map<int, std::array<std::string, 6>> parseTileLabels(std::string_view text)
{
    std::map<int, std::array<std::string, 6>> out;
    for (const std::string& line : splitText(text, "\n;"))
    {
        size_t colon = line.find(':');
        if (colon == std::string::npos)
            throw ExecError("rule without a tile index: " + line);
        int tile = 0;
        try
        {
            tile = std::stoi(trimText(std::string_view(line).substr(0, colon)));
        }
        catch (const std::exception&)
        {
            throw ExecError("bad tile index in: " + line);
        }
        auto& labels = out[tile];
        for (const std::string& tok : splitText(std::string_view(line).substr(colon + 1), " \t,"))
        {
            size_t eq = tok.find('=');
            int d = eq == std::string::npos ? -1 : sideIndex(trimText(std::string_view(tok).substr(0, eq)));
            if (d < 0)
                throw ExecError("expected a side like +x=label: " + tok);
            labels[size_t(d)] = trimText(std::string_view(tok).substr(eq + 1));
        }
    }
    return out;
}

/** The tile's points lying on each face of its cell, as a comparable key ("" when none). */
std::array<std::string, 6> tileProfiles(const std::vector<D3>& pts, double cellSize)
{
    std::array<std::string, 6> out;
    double tol = cellSize * 1e-3;
    for (int d = 0; d < 6; ++d)
    {
        int axis = d / 2;
        double target = (d % 2 == 0 ? 0.5 : -0.5) * cellSize;
        std::vector<std::pair<int64_t, int64_t>> q;
        for (const D3& p : pts)
            if (std::abs(p[axis] - target) < tol)
                q.push_back({std::llround(p[(axis + 1) % 3] / tol), std::llround(p[(axis + 2) % 3] / tol)});
        std::sort(q.begin(), q.end());
        q.erase(std::unique(q.begin(), q.end()), q.end());
        std::string key;
        for (auto [a, b] : q)
            key += std::to_string(a) + "," + std::to_string(b) + ";";
        out[size_t(d)] = std::move(key);
    }
    return out;
}

struct Variant
{
    int tile = 0;
    int quarters = 0;
    std::array<std::string, 6> keys;
    double weight = 1.0;
};

struct WfcSolver
{
    int sx = 1, sy = 1, sz = 1;
    size_t words = 1;
    std::vector<Variant> variants;
    std::array<std::vector<uint64_t>, 6> compat; // per dir: variants * words
    std::vector<uint64_t> initial;               // cells * words
    std::vector<uint64_t> wave;

    size_t cells() const { return size_t(sx) * sy * sz; }
    size_t cellIndex(int x, int y, int z) const { return (size_t(z) * sy + y) * sx + x; }
    uint64_t* cell(size_t c) { return wave.data() + c * words; }

    long neighbour(size_t c, int d) const
    {
        int x = int(c % sx), y = int((c / sx) % sy), z = int(c / (size_t(sx) * sy));
        x += int(kDirs[size_t(d)].x);
        y += int(kDirs[size_t(d)].y);
        z += int(kDirs[size_t(d)].z);
        if (x < 0 || y < 0 || z < 0 || x >= sx || y >= sy || z >= sz)
            return -1;
        return long(cellIndex(x, y, z));
    }

    bool propagate(std::vector<size_t>& stack)
    {
        std::vector<uint64_t> allowed(words);
        while (!stack.empty())
        {
            size_t c = stack.back();
            stack.pop_back();
            for (int d = 0; d < 6; ++d)
            {
                long n = neighbour(c, d);
                if (n < 0)
                    continue;
                std::fill(allowed.begin(), allowed.end(), 0);
                const uint64_t* w = cell(c);
                for (size_t k = 0; k < words; ++k)
                    for (uint64_t bits = w[k]; bits; bits &= bits - 1)
                    {
                        size_t a = k * 64 + size_t(std::countr_zero(bits));
                        const uint64_t* row = compat[size_t(d)].data() + a * words;
                        for (size_t j = 0; j < words; ++j)
                            allowed[j] |= row[j];
                    }
                uint64_t* nw = cell(size_t(n));
                bool changed = false, any = false;
                for (size_t j = 0; j < words; ++j)
                {
                    uint64_t v = nw[j] & allowed[j];
                    changed = changed || v != nw[j];
                    any = any || v != 0;
                    nw[j] = v;
                }
                if (!any)
                    return false;
                if (changed)
                    stack.push_back(size_t(n));
            }
        }
        return true;
    }

    /** Fills wave with one variant per cell, or returns false on a contradiction. */
    bool run(uint32_t seed)
    {
        wave = initial;
        std::vector<size_t> stack(cells());
        std::iota(stack.begin(), stack.end(), size_t(0));
        if (!propagate(stack))
            return false;
        for (uint32_t step = 0;; ++step)
        {
            long best = -1;
            double bestH = std::numeric_limits<double>::max();
            for (size_t c = 0; c < cells(); ++c)
            {
                const uint64_t* w = cell(c);
                double sum = 0.0, sumLog = 0.0;
                int count = 0;
                for (size_t k = 0; k < words; ++k)
                    for (uint64_t bits = w[k]; bits; bits &= bits - 1)
                    {
                        double wt = variants[k * 64 + size_t(std::countr_zero(bits))].weight;
                        sum += wt;
                        sumLog += wt * std::log(wt);
                        ++count;
                    }
                if (count == 0)
                    return false;
                if (count == 1)
                    continue;
                double h = std::log(sum) - sumLog / sum + hashToUnit(hash32(uint32_t(c), seed, 0xe7)) * 1e-6;
                if (h < bestH)
                {
                    bestH = h;
                    best = long(c);
                }
            }
            if (best < 0)
                return true;
            uint64_t* w = cell(size_t(best));
            double sum = 0.0;
            for (size_t k = 0; k < words; ++k)
                for (uint64_t bits = w[k]; bits; bits &= bits - 1)
                    sum += variants[k * 64 + size_t(std::countr_zero(bits))].weight;
            double r = hashToUnit(hash32(uint32_t(best), step, seed)) * sum;
            size_t chosen = 0;
            bool found = false;
            for (size_t k = 0; k < words && !found; ++k)
                for (uint64_t bits = w[k]; bits; bits &= bits - 1)
                {
                    chosen = k * 64 + size_t(std::countr_zero(bits));
                    r -= variants[chosen].weight;
                    if (r <= 0.0)
                    {
                        found = true;
                        break;
                    }
                }
            std::fill(w, w + words, 0);
            w[chosen / 64] = uint64_t(1) << (chosen % 64);
            stack.assign(1, size_t(best));
            if (!propagate(stack))
                return false;
        }
    }

    int chosen(size_t c) const
    {
        const uint64_t* w = wave.data() + c * words;
        for (size_t k = 0; k < words; ++k)
            if (w[k])
                return int(k * 64 + size_t(std::countr_zero(w[k])));
        return -1;
    }
};

/** Tiles from the links, or the instances of a single instances input (their positions dropped). */
std::vector<GeometryPtr> wfcTiles(const std::vector<GeometryPtr>& parts)
{
    if (parts.size() == 1 && parts[0]->instances && !parts[0]->mesh && !parts[0]->points && !parts[0]->curves)
    {
        const Instances& inst = *parts[0]->instances;
        std::vector<GeometryPtr> out;
        for (size_t k = 0; k < inst.size(); ++k)
        {
            int r = inst.reference[k];
            glm::dmat4 xf = inst.transforms[k];
            xf[3] = glm::dvec4(0, 0, 0, 1);
            if (r >= 0 && size_t(r) < inst.references.size() && inst.references[size_t(r)])
                out.push_back(makeGeometry(transformed(*inst.references[size_t(r)], xf)));
            else
                out.push_back(makeGeometry({}));
        }
        return out;
    }
    return parts;
}

void waveFunctionCollapse()
{
    registerDecl({
        .type = "WaveFunctionCollapse",
        .label = "Wave Function Collapse",
        .category = "Procedural",
        .doc = "Fills a grid of cells with tiles whose touching sides match, as instances with a \"tile\" index. Labels: each side's "
               "label comes from the rules text and sides match when the labels are equal. Profiles: sides match where the tiles' points "
               "on the shared cell face coincide (tiles centred on their cell). Restarts with a new seed on a contradiction; solved is "
               "false when every attempt failed.",
        .props = {prop("adjacency", {"labels", "profiles"}, "labels")},
        .sockets = {.inputs = {multiIn("tiles", "One geometry per tile, or one instances input whose instances are the tiles."),
                               bounded(in("sizeX", T::Int, 8), 1, 512), bounded(in("sizeY", T::Int, 1), 1, 512), bounded(in("sizeZ", T::Int, 8), 1, 512),
                               bounded(in("cellSize", T::Float, 100.0, "cm."), 0.001),
                               in("rules", T::String, "", "Labels per tile side, a tile per line or ';': \"0: +x=road -x=road +z=grass\". Missing sides are \"\"."),
                               in("weights", T::String, "", "Relative frequency per tile, comma separated."),
                               in("boundary", T::String, "",
                                  "Label every outward side on the grid's edge must have; with profiles, any text keeps those sides open. "
                                  "Axes one cell thick are left free."),
                               in("rotations", T::Bool, false, "Also try each tile turned 90, 180 and 270 degrees about Y."), in("seed", T::Int, 0),
                               bounded(in("attempts", T::Int, 10), 1, 1000)},
                    .outputs = {geoOut("instances"), out("solved", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                c.out("solved", constantBool(false));
                std::vector<GeometryPtr> tiles = wfcTiles(c.geometries("tiles"));
                if (tiles.empty())
                {
                    c.out("instances", makeGeometry({}));
                    return;
                }
                bool profiles = c.prop("adjacency") == "profiles";
                double cs = c.f("cellSize");
                std::map<int, std::array<std::string, 6>> labels = profiles ? std::map<int, std::array<std::string, 6>>{} : parseTileLabels(c.s("rules"));
                std::vector<double> weights = parseNumbers(c.s("weights"));
                int turns = c.b("rotations") ? 4 : 1;

                WfcSolver solver;
                for (size_t t = 0; t < tiles.size(); ++t)
                {
                    std::vector<D3> pts = profiles ? allPositions(realize(*tiles[t])) : std::vector<D3>{};
                    size_t firstVariant = solver.variants.size();
                    for (int q = 0; q < turns; ++q)
                    {
                        Variant v;
                        v.tile = int(t);
                        v.quarters = q;
                        v.weight = t < weights.size() ? std::max(1e-6, weights[t]) : 1.0;
                        if (profiles)
                        {
                            std::vector<D3> turned(pts.size());
                            for (size_t k = 0; k < pts.size(); ++k)
                                turned[k] = rotateY(pts[k], q);
                            v.keys = tileProfiles(turned, cs);
                        }
                        else if (auto it = labels.find(int(t)); it != labels.end())
                            for (int d = 0; d < 6; ++d)
                                v.keys[size_t(d)] = it->second[size_t(dirIndex(rotateY(kDirs[size_t(d)], -q)))];
                        bool repeat = false;
                        for (size_t k = firstVariant; k < solver.variants.size(); ++k)
                            repeat = repeat || solver.variants[k].keys == v.keys;
                        if (!repeat)
                            solver.variants.push_back(std::move(v));
                    }
                }

                solver.sx = int(c.i("sizeX"));
                solver.sy = int(c.i("sizeY"));
                solver.sz = int(c.i("sizeZ"));
                if (solver.cells() > 2000000)
                    throw ExecError("the grid has more than 2 million cells");
                size_t nv = solver.variants.size();
                solver.words = (nv + 63) / 64;
                for (int d = 0; d < 6; ++d)
                {
                    auto& rows = solver.compat[size_t(d)];
                    rows.assign(nv * solver.words, 0);
                    for (size_t a = 0; a < nv; ++a)
                        for (size_t b = 0; b < nv; ++b)
                            if (solver.variants[a].keys[size_t(d)] == solver.variants[b].keys[size_t(d ^ 1)])
                                rows[a * solver.words + b / 64] |= uint64_t(1) << (b % 64);
                }
                std::vector<uint64_t> all(solver.words, 0);
                for (size_t v = 0; v < nv; ++v)
                    all[v / 64] |= uint64_t(1) << (v % 64);
                solver.initial.resize(solver.cells() * solver.words);
                std::string boundary = c.s("boundary");
                std::string boundaryKey = profiles ? std::string() : boundary;
                int size[3] = {solver.sx, solver.sy, solver.sz};
                for (int z = 0; z < solver.sz; ++z)
                    for (int y = 0; y < solver.sy; ++y)
                        for (int x = 0; x < solver.sx; ++x)
                        {
                            size_t cell = solver.cellIndex(x, y, z);
                            std::vector<uint64_t> mask = all;
                            int at[3] = {x, y, z};
                            if (!boundary.empty())
                                for (int axis = 0; axis < 3; ++axis)
                                {
                                    if (size[axis] < 2)
                                        continue;
                                    for (int d : {axis * 2, axis * 2 + 1})
                                    {
                                        bool edge = d % 2 == 0 ? at[axis] == size[axis] - 1 : at[axis] == 0;
                                        if (!edge)
                                            continue;
                                        for (size_t v = 0; v < nv; ++v)
                                            if (solver.variants[v].keys[size_t(d)] != boundaryKey)
                                                mask[v / 64] &= ~(uint64_t(1) << (v % 64));
                                    }
                                }
                            std::copy(mask.begin(), mask.end(), solver.initial.begin() + long(cell * solver.words));
                        }

                bool solved = false;
                uint32_t seed = uint32_t(c.i("seed"));
                for (int64_t attempt = 0; attempt < c.i("attempts") && !solved; ++attempt)
                    solved = solver.run(hash32(seed, uint32_t(attempt), 0x3fc));
                c.out("solved", constantBool(solved));
                if (!solved)
                {
                    c.out("instances", makeGeometry({}));
                    return;
                }
                Instances out;
                out.references = tiles;
                std::vector<int64_t> tileIdx, turnIdx;
                for (int z = 0; z < solver.sz; ++z)
                    for (int y = 0; y < solver.sy; ++y)
                        for (int x = 0; x < solver.sx; ++x)
                        {
                            const Variant& v = solver.variants[size_t(solver.chosen(solver.cellIndex(x, y, z)))];
                            out.add(composeTransform(D3(x, y, z) * cs, D3(0.0, v.quarters * kPi * 0.5, 0.0), D3(1.0)), v.tile);
                            tileIdx.push_back(v.tile);
                            turnIdx.push_back(v.quarters);
                        }
                writeAttribute(out, "tile", Column::of(std::move(tileIdx)), T::Int);
                writeAttribute(out, "quarterTurns", Column::of(std::move(turnIdx)), T::Int);
                Geometry g;
                g.instances = std::make_shared<const Instances>(std::move(out));
                c.out("instances", makeGeometry(std::move(g)));
            },
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Split grammar

/** An oriented rectangle (size.z 0) or box: origin at a corner, unit axes, extent along each. */
struct Scope
{
    D3 o{0.0}, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    D3 size{0.0};
    bool box = false;

    D3 axis(int a) const { return a == 0 ? x : a == 1 ? y : z; }
    D3 center() const { return o + x * (size.x * 0.5) + y * (size.y * 0.5) + z * (size.z * 0.5); }
};

struct SplitPart
{
    double size = 0.0;
    bool relative = false;
    std::string side;
    std::string rule;
};

struct SplitRule
{
    enum class Kind
    {
        Alias,
        Extrude,
        Split,
        Repeat,
        Comp,
    } kind = Kind::Alias;
    int axis = 0;
    double amount = 0.0;
    std::vector<SplitPart> parts;
    std::string next;
};

struct GrammarTokens
{
    std::vector<std::string> toks;
    size_t at = 0;
    std::string line;

    explicit GrammarTokens(std::string_view body, std::string l) : line(std::move(l))
    {
        for (size_t k = 0; k < body.size();)
        {
            char ch = body[k];
            if (std::isspace(static_cast<unsigned char>(ch)))
                ++k;
            else if (std::string_view("(){}:|,~").find(ch) != std::string_view::npos)
                toks.emplace_back(1, body[k++]);
            else
            {
                size_t e = k;
                while (e < body.size() && !std::isspace(static_cast<unsigned char>(body[e])) && std::string_view("(){}:|,~").find(body[e]) == std::string_view::npos)
                    ++e;
                toks.emplace_back(body.substr(k, e - k));
                k = e;
            }
        }
    }
    bool done() const { return at >= toks.size(); }
    std::string peek() const { return done() ? std::string() : toks[at]; }
    std::string next()
    {
        if (done())
            throw ExecError("rule ends early: " + line);
        return toks[at++];
    }
    void expect(const char* t)
    {
        if (next() != t)
            throw ExecError(std::string("expected '") + t + "' in: " + line);
    }
    double number()
    {
        std::string t = next();
        try
        {
            return std::stod(t);
        }
        catch (const std::exception&)
        {
            throw ExecError("expected a number, got '" + t + "' in: " + line);
        }
    }
    int axis()
    {
        std::string t = next();
        if (t == "x" || t == "X")
            return 0;
        if (t == "y" || t == "Y")
            return 1;
        if (t == "z" || t == "Z")
            return 2;
        throw ExecError("expected an axis x, y or z in: " + line);
    }
};

std::map<std::string, SplitRule, std::less<>> parseGrammar(std::string_view text)
{
    std::map<std::string, SplitRule, std::less<>> rules;
    for (const std::string& line : splitText(text, "\n;"))
    {
        if (line.starts_with("#") || line.starts_with("//"))
            continue;
        size_t arrow = line.find("->");
        if (arrow == std::string::npos)
            throw ExecError("expected 'Name -> ...': " + line);
        std::string name = trimText(std::string_view(line).substr(0, arrow));
        while (!name.empty() && name.back() == '-')
            name = trimText(std::string_view(name).substr(0, name.size() - 1));
        GrammarTokens tk(std::string_view(line).substr(arrow + 2), line);
        SplitRule r;
        std::string op = tk.next();
        if (op == "extrude")
        {
            r.kind = SplitRule::Kind::Extrude;
            tk.expect("(");
            r.amount = tk.number();
            tk.expect(")");
            r.next = tk.next();
        }
        else if (op == "repeat")
        {
            r.kind = SplitRule::Kind::Repeat;
            tk.expect("(");
            r.axis = tk.axis();
            tk.expect(",");
            r.amount = tk.number();
            tk.expect(")");
            r.next = tk.next();
        }
        else if (op == "split" || op == "comp")
        {
            bool comp = op == "comp";
            r.kind = comp ? SplitRule::Kind::Comp : SplitRule::Kind::Split;
            tk.expect("(");
            if (comp)
                tk.next();
            else
                r.axis = tk.axis();
            tk.expect(")");
            tk.expect("{");
            while (true)
            {
                SplitPart p;
                if (comp)
                    p.side = tk.next();
                else
                {
                    if (tk.peek() == "~")
                    {
                        tk.next();
                        p.relative = true;
                    }
                    p.size = tk.number();
                }
                tk.expect(":");
                p.rule = tk.next();
                r.parts.push_back(std::move(p));
                std::string sep = tk.next();
                if (sep == "}")
                    break;
                if (sep != "|")
                    throw ExecError("expected '|' or '}' in: " + line);
            }
        }
        else
            r.next = op;
        if (!tk.done())
            throw ExecError("unexpected '" + tk.peek() + "' in: " + line);
        rules[name] = std::move(r);
    }
    return rules;
}

/** The six faces of a box scope, each with z pointing out and y up the box's z where it is a side. */
std::map<std::string, Scope> boxFaces(const Scope& b)
{
    auto face = [](D3 o, D3 x, D3 y, double sx, double sy) {
        Scope s;
        s.o = o;
        s.x = x;
        s.y = y;
        s.z = glm::cross(x, y);
        s.size = {sx, sy, 0.0};
        return s;
    };
    const D3 &X = b.x, &Y = b.y, &Z = b.z;
    double sx = b.size.x, sy = b.size.y, sz = b.size.z;
    return {
        {"front", face(b.o, X, Z, sx, sz)},
        {"right", face(b.o + X * sx, Y, Z, sy, sz)},
        {"back", face(b.o + X * sx + Y * sy, -X, Z, sx, sz)},
        {"left", face(b.o + Y * sy, -Y, Z, sy, sz)},
        {"top", face(b.o + Z * sz, X, Y, sx, sy)},
        {"bottom", face(b.o + Y * sy, X, -Y, sx, sy)},
    };
}

struct GrammarRun
{
    const std::map<std::string, SplitRule, std::less<>>& rules;
    int maxDepth = 32;
    std::vector<std::string> names;
    std::vector<std::pair<int, Scope>> out;

    int nameIndex(const std::string& n)
    {
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] == n)
                return int(k);
        names.push_back(n);
        return int(names.size() - 1);
    }

    void apply(const std::string& name, const Scope& s, int depth)
    {
        if (name == "NIL")
            return;
        auto it = rules.find(name);
        if (it == rules.end() || depth >= maxDepth)
        {
            if (out.size() >= 2000000)
                throw ExecError("the grammar made more than 2 million pieces");
            out.push_back({nameIndex(name), s});
            return;
        }
        const SplitRule& r = it->second;
        switch (r.kind)
        {
        case SplitRule::Kind::Alias: apply(r.next, s, depth + 1); break;
        case SplitRule::Kind::Extrude:
        {
            Scope b = s;
            b.box = true;
            b.size.z = r.amount;
            apply(r.next, b, depth + 1);
            break;
        }
        case SplitRule::Kind::Split:
        case SplitRule::Kind::Repeat:
        {
            if (r.axis == 2 && !s.box)
                throw ExecError(name + ": a face scope has no z to split along");
            double length = s.size[r.axis];
            std::vector<std::pair<double, const std::string*>> pieces;
            if (r.kind == SplitRule::Kind::Repeat)
            {
                int n = r.amount > 0.0 ? std::max(1, int(std::floor(length / r.amount + 0.5))) : 1;
                for (int k = 0; k < n; ++k)
                    pieces.push_back({length / n, &r.next});
            }
            else
            {
                double fixed = 0.0, weights = 0.0;
                for (const SplitPart& p : r.parts)
                    (p.relative ? weights : fixed) += std::max(0.0, p.size);
                double left = std::max(0.0, length - fixed);
                for (const SplitPart& p : r.parts)
                {
                    double len = p.relative ? (weights > 0.0 ? left * std::max(0.0, p.size) / weights : 0.0) : std::max(0.0, p.size);
                    pieces.push_back({len, &p.rule});
                }
            }
            double at = 0.0;
            D3 dir = s.axis(r.axis);
            for (auto [len, rule] : pieces)
            {
                if (at >= length - 1e-9)
                    break;
                len = std::min(len, length - at);
                if (len > 1e-9)
                {
                    Scope child = s;
                    child.o = s.o + dir * at;
                    child.size[r.axis] = len;
                    apply(*rule, child, depth + 1);
                }
                at += len;
            }
            break;
        }
        case SplitRule::Kind::Comp:
        {
            if (!s.box)
                throw ExecError(name + ": comp needs a box scope (extrude first)");
            std::map<std::string, Scope> faces = boxFaces(s);
            for (const char* side : {"front", "right", "back", "left", "top", "bottom"})
            {
                const SplitPart* pick = nullptr;
                for (const SplitPart& p : r.parts)
                    if (p.side == side)
                        pick = &p;
                bool isSide = std::string_view(side) != "top" && std::string_view(side) != "bottom";
                if (!pick && isSide)
                    for (const SplitPart& p : r.parts)
                        if (p.side == "side" || p.side == "sides")
                            pick = &p;
                if (!pick)
                    for (const SplitPart& p : r.parts)
                        if (p.side == "all")
                            pick = &p;
                if (pick)
                    apply(pick->rule, faces[side], depth + 1);
            }
            break;
        }
        }
    }
};

/** Each face as a rectangle scope: z its normal, x along its most level edge. */
std::vector<Scope> faceScopes(const geo::Mesh& m)
{
    std::vector<Scope> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        D3 n = m.faceNormal(f);
        std::vector<Index> pts = m.facePoints(f);
        D3 bestDir(0.0);
        double bestLevel = std::numeric_limits<double>::max(), bestLen = 0.0;
        for (size_t k = 0; k < pts.size(); ++k)
        {
            D3 e = m.position(pts[(k + 1) % pts.size()]) - m.position(pts[k]);
            e -= n * glm::dot(e, n);
            double len = glm::length(e);
            if (len < 1e-9)
                continue;
            double level = std::abs(glm::dot(e / len, kUp));
            if (level < bestLevel - 1e-6 || (level < bestLevel + 1e-6 && len > bestLen))
            {
                bestLevel = level;
                bestLen = len;
                bestDir = e / len;
            }
        }
        if (bestLen == 0.0)
            continue;
        Scope s;
        s.z = n;
        s.x = bestDir;
        s.y = glm::cross(n, bestDir);
        D3 p0 = m.position(pts[0]);
        double u0 = std::numeric_limits<double>::max(), u1 = -u0, v0 = u0, v1 = -u0;
        for (Index p : pts)
        {
            D3 d = m.position(p) - p0;
            u0 = std::min(u0, glm::dot(d, s.x));
            u1 = std::max(u1, glm::dot(d, s.x));
            v0 = std::min(v0, glm::dot(d, s.y));
            v1 = std::max(v1, glm::dot(d, s.y));
        }
        s.o = p0 + s.x * u0 + s.y * v0;
        s.size = {u1 - u0, v1 - v0, 0.0};
        out.push_back(s);
    }
    return out;
}

void splitGrammar()
{
    registerDecl({
        .type = "SplitBox",
        .label = "Split Grammar",
        .category = "Procedural",
        .doc = "Runs split rules on the mesh's faces (each as its bounding rectangle) or on the box around it. One rule per line or "
               "';': extrude(h) R, split(x|y|z) { 120 : R | ~1 : R }, repeat(x|y|z, size) R, comp(f) { side : R | top : R | bottom : R "
               "| front/back/left/right/all : R }, or another rule's name. On faces y runs up the wall; NIL drops a piece. Names with no "
               "rule are terminals: the mesh has their pieces with a face \"rule\" index, and points at their centres carry \"rule\", "
               "\"size\" and \"rotation\" for instancing.",
        .props = {prop("scope", {"faces", "box"}, "faces")},
        .sockets = {.inputs = {geoIn("mesh"),
                               in("rules", T::String,
                                  "Lot -> extrude(900) Mass\nMass -> comp(f) { side : Facade | top : Roof }\nFacade -> repeat(y, 300) Floor\n"
                                  "Floor -> repeat(x, 250) Bay\nBay -> split(x) { ~1 : Wall | 120 : Window | ~1 : Wall }"),
                               in("start", T::String, "Lot", "Rule the starting scopes run."), bounded(in("maxDepth", T::Int, 32), 1, 256)},
                    .outputs = {geoOut("mesh"), geoOut("points"), out("names", T::String, "Terminal names in \"rule\" index order, comma separated.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("mesh");
                auto rules = parseGrammar(c.s("rules"));
                GrammarRun run{rules, int(c.i("maxDepth")), {}, {}};
                if (g->mesh)
                {
                    std::string start = c.s("start");
                    if (c.prop("scope") == "box")
                    {
                        geo::Bounds bb = g->mesh->bounds();
                        Scope s;
                        s.box = true;
                        s.x = {1, 0, 0};
                        s.y = {0, 0, -1};
                        s.z = {0, 1, 0};
                        s.o = {bb.min.x, bb.min.y, bb.max.z};
                        D3 sz = bb.size();
                        s.size = {sz.x, sz.z, sz.y};
                        run.apply(start, s, 0);
                    }
                    else
                        for (const Scope& s : faceScopes(*g->mesh))
                            run.apply(start, s, 0);
                }

                geo::Mesh mesh, pts;
                std::vector<int64_t> faceRule, pointRule;
                std::vector<D3> pointSize, pointRot;
                auto addQuad = [&](const Scope& f, int rule) {
                    Index a = mesh.addPoint(f.o), b = mesh.addPoint(f.o + f.x * f.size.x), cc = mesh.addPoint(f.o + f.x * f.size.x + f.y * f.size.y),
                          d = mesh.addPoint(f.o + f.y * f.size.y);
                    mesh.addFace({a, b, cc, d});
                    faceRule.push_back(rule);
                };
                for (const auto& [rule, s] : run.out)
                {
                    if (s.box)
                        for (const auto& [side, f] : boxFaces(s))
                            addQuad(f, rule);
                    else
                        addQuad(s, rule);
                    pts.addPoint(s.center());
                    pointRule.push_back(rule);
                    if (s.box)
                    {
                        pointSize.push_back({s.size.x, s.size.z, s.size.y});
                        pointRot.push_back(matrixToEulerCols(s.x, s.z, -s.y));
                    }
                    else
                    {
                        pointSize.push_back(s.size);
                        pointRot.push_back(matrixToEulerCols(s.x, s.y, s.z));
                    }
                }
                writeAttribute(mesh, "rule", Domain::Face, Column::of(std::move(faceRule)), T::Int);
                writeAttribute(pts, "rule", Domain::Point, Column::of(std::move(pointRule)), T::Int);
                writeAttribute(pts, "size", Domain::Point, Column::of(std::move(pointSize)), T::Vector);
                writeAttribute(pts, "rotation", Domain::Point, Column::of(std::move(pointRot)), T::Rotation);
                std::string names;
                for (size_t k = 0; k < run.names.size(); ++k)
                    names += (k ? "," : "") + run.names[k];
                c.out("mesh", meshGeometry(std::move(mesh)));
                c.out("points", pointGeometry(std::move(pts)));
                c.outString("names", names);
            },
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// L-system

/** Rules "F=FF+[+F]" by symbol; several for one symbol are picked between per use. */
std::map<char, std::vector<std::string>> parseLRules(std::string_view text)
{
    std::map<char, std::vector<std::string>> out;
    for (const std::string& line : splitText(text, "\n;"))
    {
        size_t eq = line.find('=');
        std::string lhs = eq == std::string::npos ? std::string() : trimText(std::string_view(line).substr(0, eq));
        if (lhs.size() != 1)
            throw ExecError("expected 'X=replacement': " + line);
        std::string rhs;
        for (char ch : std::string_view(line).substr(eq + 1))
            if (!std::isspace(static_cast<unsigned char>(ch)))
                rhs += ch;
        out[lhs[0]].push_back(std::move(rhs));
    }
    return out;
}

void lSystem()
{
    registerDecl({
        .type = "LSystem",
        .label = "L-System",
        .category = "Procedural",
        .doc = "Rewrites the axiom by the rules, then draws it with a turtle starting up +Y: F and G draw a step, f moves, + - turn, & ^ "
               "pitch, \\ / roll, | turns around, [ ] branch, ! thins. Each branch is a curve with a \"radius\" that shrinks by taper.",
        .sockets = {.inputs = {in("axiom", T::String, "F"),
                               in("rules", T::String, "F=F[+F]F[-F]F", "One per line or ';'. Several rules for a symbol pick one at random per use."),
                               bounded(in("iterations", T::Int, 4), 0, 12), in("angle", T::Float, 25.0, "Degrees."), bounded(in("step", T::Float, 10.0, "cm."), 0.0),
                               bounded(in("radius", T::Float, 2.0, "cm."), 0.0), bounded(in("taper", T::Float, 0.7, "Radius factor per branch and per !."), 0.0, 1.0),
                               bounded(in("lengthScale", T::Float, 1.0, "Step factor per branch."), 0.0),
                               bounded(in("randomAngle", T::Float, 0.0, "Random turn added to each turn, degrees."), 0.0), in("seed", T::Int, 0)},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                auto rules = parseLRules(c.s("rules"));
                uint32_t seed = uint32_t(c.i("seed"));
                std::string s = c.s("axiom");
                const size_t kMaxLength = size_t(1) << 22;
                for (int64_t it = 0; it < c.i("iterations"); ++it)
                {
                    std::string next;
                    for (size_t k = 0; k < s.size(); ++k)
                    {
                        auto r = rules.find(s[k]);
                        if (r == rules.end())
                            next += s[k];
                        else
                            next += r->second[hash32(uint32_t(k), uint32_t(it), seed) % uint32_t(r->second.size())];
                        if (next.size() > kMaxLength)
                            throw ExecError("the L-system grew past 4 million symbols: lower the iterations");
                    }
                    s = std::move(next);
                }

                struct Turtle
                {
                    D3 pos{0.0}, h{0, 1, 0}, l{-1, 0, 0};
                    double radius = 1.0, step = 1.0;
                };
                struct Branch
                {
                    Turtle t;
                    std::vector<D3> pts;
                    std::vector<double> radii;
                };
                double angle = c.f("angle") * kPi / 180.0, jitter = c.f("randomAngle") * kPi / 180.0, taper = c.f("taper"), lengthScale = c.f("lengthScale");
                Turtle t;
                t.radius = c.f("radius");
                t.step = c.f("step");
                std::vector<D3> pts;
                std::vector<double> radii;
                std::vector<Branch> stack;
                CurveSet cs;
                std::vector<double> outRadius;
                auto flush = [&] {
                    if (pts.size() >= 2)
                    {
                        cs.addCurve(pts, false);
                        outRadius.insert(outRadius.end(), radii.begin(), radii.end());
                    }
                    pts.clear();
                    radii.clear();
                };
                auto turn = [&](const D3& axis, double a, size_t k) {
                    if (jitter > 0.0)
                        a += (hashToUnit(hash32(uint32_t(k), seed, 0x15)) * 2.0 - 1.0) * jitter;
                    glm::dmat3 r = axisAngle(axis, a);
                    t.h = glm::normalize(r * t.h);
                    t.l = glm::normalize(r * t.l);
                };
                for (size_t k = 0; k < s.size(); ++k)
                {
                    D3 u = glm::cross(t.h, t.l);
                    switch (s[k])
                    {
                    case 'F':
                    case 'G':
                        if (pts.empty())
                        {
                            pts.push_back(t.pos);
                            radii.push_back(t.radius);
                        }
                        t.pos += t.h * t.step;
                        pts.push_back(t.pos);
                        radii.push_back(t.radius);
                        break;
                    case 'f':
                        flush();
                        t.pos += t.h * t.step;
                        break;
                    case '+': turn(u, angle, k); break;
                    case '-': turn(u, -angle, k); break;
                    case '&': turn(t.l, angle, k); break;
                    case '^': turn(t.l, -angle, k); break;
                    case '\\': turn(t.h, angle, k); break;
                    case '/': turn(t.h, -angle, k); break;
                    case '|': turn(u, kPi, k); break;
                    case '!': t.radius *= taper; break;
                    case '[':
                        stack.push_back({t, std::move(pts), std::move(radii)});
                        pts.clear();
                        radii.clear();
                        t.radius *= taper;
                        t.step *= lengthScale;
                        break;
                    case ']':
                        flush();
                        if (!stack.empty())
                        {
                            t = stack.back().t;
                            pts = std::move(stack.back().pts);
                            radii = std::move(stack.back().radii);
                            stack.pop_back();
                        }
                        break;
                    default: break;
                    }
                }
                flush();
                writeAttribute(cs, "radius", Domain::Point, Column::of(std::move(outRadius)), T::Float);
                c.out("curve", curveGeometry(std::move(cs)));
            },
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Space colonization

struct TreeNode
{
    D3 pos;
    int parent = -1;
    std::vector<int> children;
    double radius = 0.0;
};

/** Curves through the tree: each runs from a root or branch point along first children; radius per point. */
CurveSet treeCurves(const std::vector<TreeNode>& nodes)
{
    CurveSet cs;
    std::vector<double> radius;
    std::vector<int> starts;
    for (size_t k = 0; k < nodes.size(); ++k)
        if (nodes[k].parent < 0)
            starts.push_back(int(k));
    while (!starts.empty())
    {
        int s = starts.back();
        starts.pop_back();
        std::vector<D3> pts;
        std::vector<double> rs;
        if (nodes[size_t(s)].parent >= 0)
        {
            pts.push_back(nodes[size_t(nodes[size_t(s)].parent)].pos);
            rs.push_back(nodes[size_t(s)].radius);
        }
        for (int n = s; n >= 0;)
        {
            const TreeNode& tn = nodes[size_t(n)];
            pts.push_back(tn.pos);
            rs.push_back(tn.radius);
            for (size_t k = 1; k < tn.children.size(); ++k)
                starts.push_back(tn.children[k]);
            n = tn.children.empty() ? -1 : tn.children[0];
        }
        if (pts.size() >= 2)
        {
            cs.addCurve(pts, false);
            radius.insert(radius.end(), rs.begin(), rs.end());
        }
    }
    writeAttribute(cs, "radius", Domain::Point, Column::of(std::move(radius)), T::Float);
    return cs;
}

void spaceColonization()
{
    registerDecl({
        .type = "SpaceColonization",
        .label = "Space Colonization",
        .category = "Procedural",
        .doc = "Grows branches from the roots toward the attractor points, a step at a time, using up attractors as branches reach "
               "them. For trees, roots, vines and veins. Branch radius follows the pipe model from tipRadius at the tips.",
        .sockets = {.inputs = {geoIn("attractors", "Points the branches grow toward."), geoIn("roots", "Where branches start."),
                               bounded(in("step", T::Float, 10.0, "cm."), 0.001),
                               bounded(in("influence", T::Float, 200.0, "Attractors farther than this from every branch are ignored until one grows near; 0 for no limit."), 0.0),
                               bounded(in("killDistance", T::Float, 20.0, "Attractors this close to a branch are used up."), 0.0),
                               bounded(in("iterations", T::Int, 200), 1, 100000), bounded(in("tipRadius", T::Float, 0.5, "cm."), 0.0),
                               bounded(in("exponent", T::Float, 2.0, "Pipe model: a parent's radius^e is the sum of its children's."), 1.0, 4.0)},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                std::vector<D3> attractors = allPositions(*c.geometry("attractors"));
                std::vector<TreeNode> nodes;
                for (const D3& p : allPositions(*c.geometry("roots")))
                    nodes.push_back({p, -1, {}, 0.0});
                if (nodes.empty())
                {
                    c.out("curve", makeGeometry({}));
                    return;
                }
                double step = c.f("step"), influence = c.f("influence"), kill = c.f("killDistance");
                bool unlimited = influence <= 0.0;
                CellGrid grid{std::max({influence, kill, step}), {}};
                for (size_t k = 0; k < nodes.size(); ++k)
                    grid.add(nodes[k].pos, uint32_t(k));
                std::vector<uint8_t> alive(attractors.size(), 1);
                size_t aliveCount = attractors.size();

                auto nearestNode = [&](const D3& a, double limit) {
                    int best = -1;
                    double bestD = limit;
                    auto test = [&](uint32_t i) {
                        double d = glm::distance(a, nodes[i].pos);
                        if (d < bestD)
                        {
                            bestD = d;
                            best = int(i);
                        }
                    };
                    if (limit == std::numeric_limits<double>::max())
                        for (size_t i = 0; i < nodes.size(); ++i)
                            test(uint32_t(i));
                    else
                        grid.around(a, test);
                    return best;
                };
                auto killNear = [&] {
                    for (size_t a = 0; a < attractors.size(); ++a)
                        if (alive[a] && nearestNode(attractors[a], kill) >= 0)
                        {
                            alive[a] = 0;
                            --aliveCount;
                        }
                };
                auto grow = [&](int n, const D3& dir) {
                    D3 pos = nodes[size_t(n)].pos + dir * step;
                    for (int ch : nodes[size_t(n)].children)
                        if (glm::distance(nodes[size_t(ch)].pos, pos) < step * 0.1)
                            return false;
                    int id = int(nodes.size());
                    nodes.push_back({pos, n, {}, 0.0});
                    nodes[size_t(n)].children.push_back(id);
                    grid.add(pos, uint32_t(id));
                    return true;
                };

                killNear();
                for (int64_t it = 0; it < c.i("iterations") && aliveCount > 0; ++it)
                {
                    std::vector<D3> pull(nodes.size(), D3(0.0));
                    std::vector<uint8_t> pulled(nodes.size(), 0);
                    for (size_t a = 0; a < attractors.size(); ++a)
                    {
                        if (!alive[a])
                            continue;
                        int n = nearestNode(attractors[a], unlimited ? std::numeric_limits<double>::max() : influence);
                        if (n < 0)
                            continue;
                        D3 d = attractors[a] - nodes[size_t(n)].pos;
                        double len = glm::length(d);
                        if (len > 1e-12)
                        {
                            pull[size_t(n)] += d / len;
                            pulled[size_t(n)] = 1;
                        }
                    }
                    bool grew = false;
                    size_t count = pull.size();
                    for (size_t n = 0; n < count; ++n)
                        if (pulled[n] && glm::length(pull[n]) > 1e-9)
                            grew = grow(int(n), glm::normalize(pull[n])) || grew;
                    if (!grew)
                    {
                        // Nothing in reach: the closest branch heads straight for its closest attractor.
                        int bestN = -1;
                        size_t bestA = 0;
                        double bestD = std::numeric_limits<double>::max();
                        for (size_t a = 0; a < attractors.size(); ++a)
                        {
                            if (!alive[a])
                                continue;
                            for (size_t n = 0; n < nodes.size(); ++n)
                            {
                                double d = glm::distance(attractors[a], nodes[n].pos);
                                if (d < bestD)
                                {
                                    bestD = d;
                                    bestN = int(n);
                                    bestA = a;
                                }
                            }
                        }
                        if (bestN < 0 || bestD < 1e-12 || !grow(bestN, (attractors[bestA] - nodes[size_t(bestN)].pos) / bestD))
                            break;
                    }
                    killNear();
                }

                double e = c.f("exponent"), tip = c.f("tipRadius");
                for (size_t k = nodes.size(); k-- > 0;)
                {
                    TreeNode& tn = nodes[k];
                    if (tn.children.empty())
                    {
                        tn.radius = tip;
                        continue;
                    }
                    double sum = 0.0;
                    for (int ch : tn.children)
                        sum += std::pow(nodes[size_t(ch)].radius, e);
                    tn.radius = std::pow(sum, 1.0 / e);
                }
                c.out("curve", curveGeometry(treeCurves(nodes)));
            },
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Spline network

D3 flat(const D3& v) { return {v.x, 0.0, v.z}; }

D3 flatDir(const D3& v)
{
    D3 f = flat(v);
    double l = glm::length(f);
    return l > 1e-12 ? f / l : D3(1, 0, 0);
}

/** Welds by position so strips and junction patches share their corners. */
struct WeldedMesh
{
    geo::Mesh mesh;
    std::map<std::array<int64_t, 3>, Index> lookup;
    std::vector<uint8_t> junction;

    Index point(const D3& p)
    {
        const double q = 1e-4;
        std::array<int64_t, 3> key = {std::llround(p.x / q), std::llround(p.y / q), std::llround(p.z / q)};
        auto it = lookup.find(key);
        if (it != lookup.end())
            return it->second;
        Index i = mesh.addPoint(p);
        lookup[key] = i;
        return i;
    }

    /** A face wound to face up, dropping repeated corners. */
    void face(const std::vector<D3>& pts, bool isJunction)
    {
        std::vector<Index> ids;
        for (const D3& p : pts)
        {
            Index i = point(p);
            if (ids.empty() || ids.back() != i)
                ids.push_back(i);
        }
        while (ids.size() > 1 && ids.front() == ids.back())
            ids.pop_back();
        if (ids.size() < 3)
            return;
        double ny = 0.0;
        for (size_t k = 0; k < ids.size(); ++k)
        {
            const D3& a = mesh.position(ids[k]);
            const D3& b = mesh.position(ids[(k + 1) % ids.size()]);
            ny += (a.z - b.z) * (a.x + b.x);
        }
        if (ny < 0.0)
            std::reverse(ids.begin(), ids.end());
        mesh.addFace(ids);
        junction.push_back(isJunction ? 1 : 0);
    }
};

struct NetChain
{
    int a = -1, b = -1; // end nodes; -1 for a closed loop
    std::vector<D3> pts;
};

/** The polyline with length `cut` removed from its start. */
std::vector<D3> trimStart(const std::vector<D3>& pts, double cut)
{
    if (cut <= 0.0 || pts.size() < 2)
        return pts;
    for (size_t k = 1; k < pts.size(); ++k)
    {
        double seg = glm::distance(pts[k - 1], pts[k]);
        if (cut < seg)
        {
            std::vector<D3> out;
            out.push_back(glm::mix(pts[k - 1], pts[k], cut / seg));
            out.insert(out.end(), pts.begin() + long(k), pts.end());
            return out;
        }
        cut -= seg;
    }
    return {pts.back(), pts.back()};
}

double polylineLength(const std::vector<D3>& pts)
{
    double l = 0.0;
    for (size_t k = 1; k < pts.size(); ++k)
        l += glm::distance(pts[k - 1], pts[k]);
    return l;
}

/** Closest point on segment ab to p in the ground plane: the parameter along ab. */
double closestParam(const D3& a, const D3& b, const D3& p)
{
    D3 ab = flat(b - a);
    double len2 = glm::dot(ab, ab);
    return len2 > 1e-18 ? std::clamp(glm::dot(flat(p - a), ab) / len2, 0.0, 1.0) : 0.0;
}

/** Cuts the curves at crossings and at ends that touch another curve, then merges runs through plain bends. */
std::vector<NetChain> buildNetwork(const CurveSet& cs, double snap, std::vector<D3>& nodePos)
{
    struct Split
    {
        double param;
        int node;
    };
    size_t nc = cs.numCurves();
    std::vector<std::vector<D3>> lines(nc);
    std::vector<uint8_t> closed(nc);
    for (size_t k = 0; k < nc; ++k)
    {
        auto p = cs.curvePoints(k);
        lines[k].assign(p.begin(), p.end());
        closed[k] = cs.cyclic[k] && lines[k].size() > 2;
        if (closed[k])
            lines[k].push_back(lines[k].front());
    }
    std::vector<std::vector<Split>> splits(nc);

    struct Seg
    {
        int curve, index;
        double minX, maxX, minZ, maxZ;
    };
    std::vector<Seg> segs;
    for (size_t k = 0; k < nc; ++k)
        for (size_t i = 0; i + 1 < lines[k].size(); ++i)
        {
            const D3 &a = lines[k][i], &b = lines[k][i + 1];
            segs.push_back({int(k), int(i), std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z)});
        }
    std::sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) { return a.minX < b.minX; });
    for (size_t i = 0; i < segs.size(); ++i)
        for (size_t j = i + 1; j < segs.size() && segs[j].minX <= segs[i].maxX; ++j)
        {
            const Seg &s = segs[i], &t = segs[j];
            if (t.minZ > s.maxZ || t.maxZ < s.minZ)
                continue;
            if (s.curve == t.curve)
            {
                int n = int(lines[size_t(s.curve)].size()) - 1;
                int gap = std::abs(s.index - t.index);
                if (gap <= 1 || (closed[size_t(s.curve)] && gap == n - 1))
                    continue;
            }
            const D3 &a = lines[size_t(s.curve)][size_t(s.index)], &b = lines[size_t(s.curve)][size_t(s.index) + 1];
            const D3 &p = lines[size_t(t.curve)][size_t(t.index)], &q = lines[size_t(t.curve)][size_t(t.index) + 1];
            double rx = b.x - a.x, rz = b.z - a.z, sx = q.x - p.x, sz = q.z - p.z;
            double den = rx * sz - rz * sx;
            if (std::abs(den) < 1e-12)
                continue;
            double u = ((p.x - a.x) * sz - (p.z - a.z) * sx) / den;
            double v = ((p.x - a.x) * rz - (p.z - a.z) * rx) / den;
            if (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0)
                continue;
            D3 at = glm::mix(a, b, u);
            at.y = 0.5 * (at.y + glm::mix(p, q, v).y);
            int node = int(nodePos.size());
            nodePos.push_back(at);
            splits[size_t(s.curve)].push_back({s.index + u, node});
            splits[size_t(t.curve)].push_back({t.index + v, node});
        }

    for (size_t k = 0; k < nc; ++k)
    {
        if (closed[k] || lines[k].size() < 2)
            continue;
        for (int end = 0; end < 2; ++end)
        {
            double param = end == 0 ? 0.0 : double(lines[k].size() - 1);
            D3 p = end == 0 ? lines[k].front() : lines[k].back();
            int node = -1;
            double best = snap;
            for (size_t n = 0; n < nodePos.size(); ++n)
            {
                double d = glm::length(flat(nodePos[n] - p));
                if (d <= best)
                {
                    best = d;
                    node = int(n);
                }
            }
            if (node < 0)
            {
                int bestCurve = -1;
                double bestParam = 0.0;
                best = snap;
                for (size_t o = 0; o < nc; ++o)
                {
                    if (o == k)
                        continue;
                    for (size_t i = 0; i + 1 < lines[o].size(); ++i)
                    {
                        double u = closestParam(lines[o][i], lines[o][i + 1], p);
                        double d = glm::length(flat(glm::mix(lines[o][i], lines[o][i + 1], u) - p));
                        if (d <= best)
                        {
                            best = d;
                            bestCurve = int(o);
                            bestParam = double(i) + u;
                        }
                    }
                }
                node = int(nodePos.size());
                if (bestCurve >= 0)
                {
                    size_t i = std::min(size_t(bestParam), lines[size_t(bestCurve)].size() - 2);
                    nodePos.push_back(glm::mix(lines[size_t(bestCurve)][i], lines[size_t(bestCurve)][i + 1], bestParam - double(i)));
                    splits[size_t(bestCurve)].push_back({bestParam, node});
                }
                else
                    nodePos.push_back(p);
            }
            splits[k].push_back({param, node});
        }
    }

    struct Edge
    {
        int a, b;
        std::vector<D3> pts;
    };
    std::vector<Edge> edges;
    std::vector<NetChain> chains;
    for (size_t k = 0; k < nc; ++k)
    {
        auto& sp = splits[k];
        std::sort(sp.begin(), sp.end(), [](const Split& a, const Split& b) { return a.param < b.param; });
        sp.erase(std::unique(sp.begin(), sp.end(), [](const Split& a, const Split& b) { return a.node == b.node && std::abs(a.param - b.param) < 1e-9; }), sp.end());
        if (lines[k].size() < 2)
            continue;
        if (sp.empty())
        {
            if (closed[k])
                chains.push_back({-1, -1, std::vector<D3>(lines[k].begin(), lines[k].end() - 1)});
            continue;
        }
        double last = double(lines[k].size() - 1);
        size_t count = sp.size();
        size_t pieces = closed[k] ? count : count - 1;
        for (size_t s = 0; s < pieces; ++s)
        {
            const Split& from = sp[s];
            const Split& to = sp[(s + 1) % count];
            double end = s + 1 < count ? to.param : to.param + last;
            Edge e{from.node, to.node, {nodePos[size_t(from.node)]}};
            for (double i = std::floor(from.param) + 1.0; i < end - 1e-9; i += 1.0)
                if (i > from.param + 1e-9)
                    e.pts.push_back(lines[k][size_t(std::fmod(i, last))]);
            e.pts.push_back(nodePos[size_t(to.node)]);
            if (polylineLength(e.pts) > 1e-6)
                edges.push_back(std::move(e));
        }
    }

    std::vector<std::vector<int>> incident(nodePos.size());
    for (size_t e = 0; e < edges.size(); ++e)
    {
        incident[size_t(edges[e].a)].push_back(int(e));
        incident[size_t(edges[e].b)].push_back(int(e));
    }
    std::vector<uint8_t> used(edges.size(), 0);
    auto walk = [&](int startNode, int e) {
        NetChain ch;
        ch.a = startNode;
        int node = startNode;
        while (true)
        {
            used[size_t(e)] = 1;
            std::vector<D3> pts = edges[size_t(e)].pts;
            int other = edges[size_t(e)].b;
            if (edges[size_t(e)].a != node)
            {
                std::reverse(pts.begin(), pts.end());
                other = edges[size_t(e)].a;
            }
            ch.pts.insert(ch.pts.end(), pts.begin() + (ch.pts.empty() ? 0 : 1), pts.end());
            node = other;
            if (incident[size_t(node)].size() != 2)
                break;
            int nextEdge = incident[size_t(node)][0] == e ? incident[size_t(node)][1] : incident[size_t(node)][0];
            if (used[size_t(nextEdge)])
                break;
            e = nextEdge;
        }
        ch.b = node;
        return ch;
    };
    for (size_t n = 0; n < nodePos.size(); ++n)
        if (incident[n].size() != 2)
            for (int e : incident[n])
                if (!used[size_t(e)])
                    chains.push_back(walk(int(n), e));
    for (size_t e = 0; e < edges.size(); ++e)
        if (!used[e])
        {
            NetChain ch = walk(edges[e].a, int(e));
            ch.pts.pop_back();
            ch.a = ch.b = -1;
            chains.push_back(std::move(ch));
        }
    return chains;
}

void splineNetwork()
{
    registerDecl({
        .type = "SplineNetwork",
        .label = "Spline Network",
        .category = "Procedural",
        .doc = "Lays a strip of the given width along each curve, level across and following the curve's height, for roads, paths and "
               "rails. Where curves cross, or an end meets another curve, the strips stop at a junction patch; ends meeting end to end "
               "join into one strip. Faces carry a \"junction\" flag; junctions gives a point per meeting of three or more with its "
               "\"degree\".",
        .sockets = {.inputs = {geoIn("curves"), bounded(in("width", T::Float, 400.0, "cm."), 0.001), in("capEnds", T::Bool, false, "Round off loose ends."),
                               bounded(in("capSegments", T::Int, 6), 1, 64),
                               bounded(in("snap", T::Float, 0.0, "Ends this close to another curve join it, cm. 0 uses half the width."), 0.0)},
                    .outputs = {geoOut("mesh"), geoOut("junctions")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("curves");
                if (!g->curves || g->curves->numCurves() == 0)
                {
                    c.out("mesh", makeGeometry({}));
                    c.out("junctions", makeGeometry({}));
                    return;
                }
                double w = c.f("width") * 0.5;
                double snap = c.f("snap") > 0.0 ? c.f("snap") : w;
                std::vector<D3> nodePos;
                std::vector<NetChain> chains = buildNetwork(g->curves->evaluated(), snap, nodePos);

                std::vector<int> degree(nodePos.size(), 0);
                for (const NetChain& ch : chains)
                    if (ch.a >= 0)
                    {
                        ++degree[size_t(ch.a)];
                        ++degree[size_t(ch.b)];
                    }

                // Trim distance per junction, from the narrowest angle between neighbouring strips.
                std::vector<double> trim(nodePos.size(), 0.0);
                std::vector<std::vector<std::pair<int, bool>>> ends(nodePos.size()); // chain, at its start
                for (size_t k = 0; k < chains.size(); ++k)
                    if (chains[k].a >= 0)
                    {
                        ends[size_t(chains[k].a)].push_back({int(k), true});
                        ends[size_t(chains[k].b)].push_back({int(k), false});
                    }
                auto awayDir = [&](const NetChain& ch, bool atStart) {
                    const auto& p = ch.pts;
                    return atStart ? flatDir(p[1] - p[0]) : flatDir(p[p.size() - 2] - p.back());
                };
                auto angleOf = [](const D3& d) { return std::atan2(-d.z, d.x); };
                for (size_t n = 0; n < nodePos.size(); ++n)
                {
                    if (degree[n] < 3)
                        continue;
                    std::vector<double> ang;
                    for (auto [ch, start] : ends[n])
                        ang.push_back(angleOf(awayDir(chains[size_t(ch)], start)));
                    std::sort(ang.begin(), ang.end());
                    double gap = 2.0 * kPi;
                    for (size_t k = 0; k < ang.size(); ++k)
                        gap = std::min(gap, k + 1 < ang.size() ? ang[k + 1] - ang[k] : ang[0] + 2.0 * kPi - ang[k]);
                    gap = std::min(gap, kPi);
                    double r = gap > 1e-6 ? w / std::tan(gap * 0.5) : 4.0 * w;
                    trim[n] = std::clamp(r, w, 4.0 * w);
                }

                std::vector<std::vector<D3>> trimmed(chains.size());
                for (size_t k = 0; k < chains.size(); ++k)
                {
                    const NetChain& ch = chains[k];
                    std::vector<D3> pts = ch.pts;
                    if (ch.a >= 0)
                    {
                        double len = polylineLength(pts);
                        double ta = std::min(trim[size_t(ch.a)], len * 0.45), tb = std::min(trim[size_t(ch.b)], len * 0.45);
                        pts = trimStart(pts, ta);
                        std::reverse(pts.begin(), pts.end());
                        pts = trimStart(pts, tb);
                        std::reverse(pts.begin(), pts.end());
                    }
                    trimmed[k] = std::move(pts);
                }

                WeldedMesh out;
                for (size_t k = 0; k < chains.size(); ++k)
                {
                    const std::vector<D3>& p = trimmed[k];
                    bool loop = chains[k].a < 0;
                    size_t n = p.size();
                    if (n < 2)
                        continue;
                    std::vector<D3> left(n), right(n);
                    for (size_t i = 0; i < n; ++i)
                    {
                        bool hasPrev = i > 0 || loop, hasNext = i + 1 < n || loop;
                        D3 prev = hasPrev ? flatDir(p[i] - p[(i + n - 1) % n]) : D3(0.0);
                        D3 next = hasNext ? flatDir(p[(i + 1) % n] - p[i]) : D3(0.0);
                        D3 t = hasPrev && hasNext ? flatDir(prev + next) : hasPrev ? prev : next;
                        double miter = hasPrev && hasNext ? 1.0 / std::max(0.25, glm::dot(t, next)) : 1.0;
                        D3 side = glm::cross(t, kUp) * (w * miter);
                        left[i] = p[i] + side;
                        right[i] = p[i] - side;
                    }
                    for (size_t i = 0; i + 1 < n || (loop && i < n); ++i)
                    {
                        size_t j = (i + 1) % n;
                        out.face({right[i], left[i], left[j], right[j]}, false);
                    }
                }

                geo::Mesh junctionPts;
                std::vector<int64_t> junctionDegree;
                bool caps = c.b("capEnds");
                int capSegments = int(c.i("capSegments"));
                for (size_t n = 0; n < nodePos.size(); ++n)
                {
                    if (degree[n] >= 3)
                    {
                        std::vector<std::tuple<double, D3, D3>> sorted;
                        double y = 0.0;
                        for (auto [ch, start] : ends[n])
                        {
                            const std::vector<D3>& p = trimmed[size_t(ch)];
                            D3 e = start ? p.front() : p.back();
                            D3 d = start ? flatDir(p[1] - p[0]) : flatDir(p[p.size() - 2] - p.back());
                            sorted.push_back({angleOf(d), e, d});
                            y += e.y;
                        }
                        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                        std::vector<D3> poly;
                        for (const auto& [a, e, d] : sorted)
                        {
                            D3 s = glm::cross(d, kUp) * w;
                            poly.push_back(e + s);
                            poly.push_back(e - s);
                        }
                        out.face(poly, true);
                        D3 centre = nodePos[n];
                        centre.y = y / double(ends[n].size());
                        junctionPts.addPoint(centre);
                        junctionDegree.push_back(degree[n]);
                    }
                    else if (degree[n] == 1 && caps)
                    {
                        auto [ch, start] = ends[n][0];
                        const std::vector<D3>& p = trimmed[size_t(ch)];
                        D3 e = start ? p.front() : p.back();
                        D3 d = start ? flatDir(p[1] - p[0]) : flatDir(p[p.size() - 2] - p.back());
                        D3 s = glm::cross(d, kUp);
                        std::vector<D3> poly;
                        for (int k = 0; k <= capSegments; ++k)
                        {
                            double a = kPi * k / capSegments;
                            poly.push_back(e + (s * std::cos(a) - d * std::sin(a)) * w);
                        }
                        out.face(poly, false);
                    }
                }

                std::vector<uint8_t> flags = std::move(out.junction);
                writeAttribute(out.mesh, "junction", Domain::Face, Column::of(std::move(flags)), T::Bool);
                writeAttribute(junctionPts, "degree", Domain::Point, Column::of(std::move(junctionDegree)), T::Int);
                c.out("mesh", meshGeometry(std::move(out.mesh)));
                c.out("junctions", pointGeometry(std::move(junctionPts)));
            },
    });
}
} // namespace lila::gn::nodes::procedural

namespace lila::gn::nodes
{
void registerProceduralNodes()
{
    procedural::poissonDisk();
    procedural::waveFunctionCollapse();
    procedural::splitGrammar();
    procedural::lSystem();
    procedural::spaceColonization();
    procedural::splineNetwork();
}
} // namespace lila::gn::nodes
