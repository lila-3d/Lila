#include "uv/FinalMesh.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"
#include "uv/OptimizeMath.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <unordered_map>

namespace lila::uv
{
namespace
{
using namespace detail;

constexpr const char* kProbeFace = "lila:finalFace";
constexpr const char* kProbeCentre = "lila:finalCentre";
constexpr const char* kProbePrefix = "lila:finalProbe";
/** Added to a probed wedge's uv. Irrational-ish so probes never land exactly on another wedge's uv. */
constexpr double kDelta = 0.6180339887;
/** Weights under this are float noise. */
constexpr double kMinWeight = 2e-5;
/** Probe layers per stack run. */
constexpr int kLayersPerRun = 16;

/** Wedge (chart vertex over the whole mesh, from uv agreement) of every corner, and a representative corner per wedge. */
struct Wedges
{
    std::vector<Index> ofCorner;
    std::vector<Index> rep;
    std::vector<Index> point;
    std::vector<std::vector<Index>> atPoint;
};

Wedges wedgesOf(const geo::Mesh& m, std::string_view uvSet)
{
    // Grouped by value at each point, as subdivision's uv islands compare them, so a probe never splits what it joins.
    const std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>(uvSet);
    auto same = [&](Index a, Index b) { return glm::all(glm::lessThanEqual(glm::abs(uv[a] - uv[b]), glm::vec2(1e-6f))); };
    Wedges w;
    w.ofCorner.assign(m.cornerCapacity(), kInvalid);
    w.atPoint.resize(m.pointCapacity());
    std::vector<Index> corners;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        corners.clear();
        m.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
        for (Index c : corners)
        {
            if (w.ofCorner[c] != kInvalid)
                continue;
            const Index id = Index(w.rep.size());
            w.rep.push_back(c);
            w.point.push_back(p);
            w.atPoint[p].push_back(id);
            for (Index o : corners)
                if (w.ofCorner[o] == kInvalid && same(c, o))
                    w.ofCorner[o] = id;
        }
    }
    return w;
}

/** Per face, the wedges at the points within rings edge steps of its points, sorted. */
std::vector<std::vector<Index>> neighbourhoods(const geo::Mesh& m, const Wedges& w, int rings)
{
    std::vector<std::vector<Index>> out(m.faceCapacity());
    tbb::parallel_for(tbb::blocked_range<Index>(0, m.faceCapacity(), 64), [&](const tbb::blocked_range<Index>& r) {
        std::vector<uint32_t> stamp(m.pointCapacity(), 0);
        uint32_t s = 0;
        std::vector<Index> front, next;
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            ++s;
            front.clear();
            m.forEachFaceCorner(f, [&](Index c) {
                const Index p = m.cornerPoint(c);
                if (stamp[p] != s)
                {
                    stamp[p] = s;
                    front.push_back(p);
                }
            });
            std::vector<Index> pts = front;
            for (int k = 0; k < rings; ++k)
            {
                next.clear();
                for (Index p : front)
                    m.forEachPointEdge(p, [&](Index e) {
                        const Index q = m.edgeOtherPoint(e, p);
                        if (stamp[q] != s)
                        {
                            stamp[q] = s;
                            next.push_back(q);
                        }
                    });
                pts.insert(pts.end(), next.begin(), next.end());
                front.swap(next);
            }
            std::vector<Index>& o = out[f];
            for (Index p : pts)
                o.insert(o.end(), w.atPoint[p].begin(), w.atPoint[p].end());
            std::sort(o.begin(), o.end());
        }
    });
    return out;
}

/** Greedy colouring so no two wedges of one face's neighbourhood share a colour. Returns the colour count. */
int colourWedges(const std::vector<std::vector<Index>>& hoods, size_t wedgeCount, std::vector<int>& colour)
{
    std::vector<std::vector<Index>> facesOf(wedgeCount);
    for (Index f = 0; f < hoods.size(); ++f)
        for (Index w : hoods[f])
            facesOf[w].push_back(f);
    colour.assign(wedgeCount, -1);
    std::vector<uint32_t> used;
    uint32_t stamp = 0;
    int count = 0;
    for (size_t w = 0; w < wedgeCount; ++w)
    {
        ++stamp;
        for (Index f : facesOf[w])
            for (Index o : hoods[f])
                if (colour[o] >= 0)
                {
                    if (size_t(colour[o]) >= used.size())
                        used.resize(size_t(colour[o]) + 1, 0);
                    used[size_t(colour[o])] = stamp;
                }
        int c = 0;
        while (size_t(c) < used.size() && used[size_t(c)] == stamp)
            ++c;
        colour[w] = c;
        count = std::max(count, c + 1);
    }
    return count;
}

bool sameTopology(const geo::Mesh& a, const geo::Mesh& b)
{
    return a.cornerCapacity() == b.cornerCapacity() && a.faceCapacity() == b.faceCapacity() && a.numFaces() == b.numFaces() &&
           a.numCorners() == b.numCorners();
}

/** Cage face centroids on a uniform grid, for the face nearest a point. */
class CentroidGrid
{
public:
    explicit CentroidGrid(const geo::Mesh& cage)
    {
        for (Index f = 0; f < cage.faceCapacity(); ++f)
            if (cage.faceValid(f))
            {
                mFaces.push_back(f);
                mPts.push_back(cage.faceCentroid(f));
            }
        glm::dvec3 lo(1e300), hi(-1e300);
        for (const glm::dvec3& p : mPts)
        {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        mLo = lo;
        const double diag = mPts.empty() ? 1.0 : std::max(glm::length(hi - lo), 1e-9);
        mCell = diag / std::max(1.0, std::cbrt(double(mPts.size())));
        for (size_t i = 0; i < mPts.size(); ++i)
            mCells[key(cellOf(mPts[i]))].push_back(Index(i));
    }

    /** The nearest cage face, kInvalid when there are none. */
    Index nearest(const glm::dvec3& p) const
    {
        if (mPts.empty())
            return kInvalid;
        const glm::ivec3 c = cellOf(p);
        Index best = kInvalid;
        double bestD = 1e300;
        for (int r = 0; r < 64; ++r)
        {
            for (int x = -r; x <= r; ++x)
                for (int y = -r; y <= r; ++y)
                    for (int z = -r; z <= r; ++z)
                    {
                        if (std::max({std::abs(x), std::abs(y), std::abs(z)}) != r)
                            continue;
                        auto it = mCells.find(key(c + glm::ivec3(x, y, z)));
                        if (it == mCells.end())
                            continue;
                        for (Index i : it->second)
                            if (const double d = glm::length(mPts[i] - p); d < bestD)
                            {
                                bestD = d;
                                best = mFaces[i];
                            }
                    }
            // Anything in a farther shell is at least r cells away.
            if (best != kInvalid && bestD <= double(r) * mCell)
                break;
        }
        return best;
    }

private:
    glm::ivec3 cellOf(const glm::dvec3& p) const { return glm::ivec3(glm::floor((p - mLo) / mCell)); }
    static uint64_t key(const glm::ivec3& c)
    {
        return (uint64_t(uint32_t(c.x) & 0x1fffff) << 42) | (uint64_t(uint32_t(c.y) & 0x1fffff) << 21) | uint64_t(uint32_t(c.z) & 0x1fffff);
    }

    std::vector<Index> mFaces;
    std::vector<glm::dvec3> mPts;
    glm::dvec3 mLo{0.0};
    double mCell = 1.0;
    std::unordered_map<uint64_t, std::vector<Index>> mCells;
};

/**
 * Per result face, the cage face it came from. Each face carries its source's id (plus one, so 0 is a face made without
 * a source) and centroid; a modifier that blends face attributes garbles the id but leaves the centroid near where the
 * face came from, so a face whose id disagrees with its centroid takes the cage face nearest that centroid. Faces with
 * no source take a neighbour's.
 */
std::vector<int> sourceFaces(const geo::Mesh& cage, const geo::Mesh& res, std::span<const int> ids, std::span<const glm::vec3> centres)
{
    std::vector<int> src(res.faceCapacity(), -1);
    const glm::dvec3 size = cage.bounds().size();
    const double tol = 1e-5 * std::max(glm::length(size), 1e-9);
    std::unique_ptr<CentroidGrid> grid;
    for (Index f = 0; f < res.faceCapacity(); ++f)
    {
        if (!res.faceValid(f) || ids[f] <= 0)
            continue;
        const Index id = Index(ids[f] - 1);
        if (cage.faceValid(id) && glm::length(cage.faceCentroid(id) - glm::dvec3(centres[f])) <= tol)
        {
            src[f] = int(id);
            continue;
        }
        if (!grid)
            grid = std::make_unique<CentroidGrid>(cage);
        if (const Index n = grid->nearest(glm::dvec3(centres[f])); n != kInvalid)
            src[f] = int(n);
    }
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Index f = 0; f < res.faceCapacity(); ++f)
        {
            if (!res.faceValid(f) || src[f] >= 0)
                continue;
            res.forEachFaceCorner(f, [&](Index c) {
                res.forEachEdgeCorner(res.cornerEdge(c), [&](Index r) {
                    if (src[f] < 0 && src[res.cornerFace(r)] >= 0)
                        src[f] = src[res.cornerFace(r)];
                });
            });
            changed |= src[f] >= 0;
        }
    }
    return src;
}

/** One attempt at the map with the given ring count. Empty error on success. */
std::string probe(const geo::Mesh& cage, std::string_view uvSet, const Stack& stack, const geo::Mesh& real, int rings, FinalMap& out)
{
    LILA_ZONE();

    const Wedges w = wedgesOf(cage, uvSet);
    const std::vector<std::vector<Index>> hoods = neighbourhoods(cage, w, rings);
    std::vector<int> colour;
    const int colours = colourWedges(hoods, w.rep.size(), colour);
    const int layers = (colours + 1) / 2;

    // The probe base: the cage with only the uv set among its uv layers, and each face's id.
    geo::Mesh base = cage;
    {
        std::vector<std::string> drop;
        for (size_t l = 0; l < base.cornerAttrs().layerCount(); ++l)
            if (const geo::Attribute& a = base.cornerAttrs().layer(l); a.type() == geo::AttrType::Float2 && a.name() != uvSet)
                drop.push_back(a.name());
        for (const std::string& n : drop)
            base.cornerAttrs().remove(n);
        std::span<int> ids = base.faceAttrs().getOrAdd<int>(kProbeFace);
        for (Index f = 0; f < base.faceCapacity(); ++f)
            ids[f] = int(f) + 1;
        std::span<glm::vec3> centres = base.faceAttrs().getOrAdd<glm::vec3>(kProbeCentre);
        for (Index f = 0; f < base.faceCapacity(); ++f)
            if (base.faceValid(f))
                centres[f] = glm::vec3(base.faceCentroid(f));
    }
    const std::span<const glm::vec2> cageUv = cage.cornerAttrs().get<glm::vec2>(uvSet);

    const Index rows = real.cornerCapacity();
    std::vector<std::vector<std::pair<Index, double>>> entries(rows);
    std::vector<double> unattributed(rows, 0.0);
    std::vector<int> sourceFace;
    out.runs = 0;
    for (int first = 0; first < std::max(layers, 1); first += kLayersPerRun)
    {
        const int count = std::min(kLayersPerRun, layers - first);
        geo::Mesh in = base;
        for (int k = 0; k < count; ++k)
        {
            std::span<glm::vec2> p = in.cornerAttrs().getOrAdd<glm::vec2>(std::string(kProbePrefix) + std::to_string(k) + "@" + std::string(uvSet));
            const int cu = 2 * (first + k), cv = cu + 1;
            for (Index c = 0; c < in.cornerCapacity(); ++c)
            {
                const Index wd = w.ofCorner[c];
                if (wd == kInvalid)
                    continue;
                glm::dvec2 v(cageUv[c]);
                if (colour[wd] == cu)
                    v.x += kDelta;
                else if (colour[wd] == cv)
                    v.y += kDelta;
                p[c] = glm::vec2(v);
            }
        }
        geo::Mesh res = runStack(in, stack);
        ++out.runs;
        if (!sameTopology(res, real))
            return "its topology changes with the uvs";
        const std::span<const glm::vec2> baseUv = res.cornerAttrs().get<glm::vec2>(uvSet);
        const std::span<const int> faceIds = res.faceAttrs().get<int>(kProbeFace);
        const std::span<const glm::vec3> faceCentres = res.faceAttrs().get<glm::vec3>(kProbeCentre);
        if (baseUv.empty() || faceIds.empty() || faceCentres.empty())
            return "it drops corner or face attributes";
        if (sourceFace.empty())
            sourceFace = sourceFaces(cage, res, faceIds, faceCentres);
        std::vector<std::span<const glm::vec2>> probes(static_cast<size_t>(count));
        for (int k = 0; k < count; ++k)
        {
            probes[size_t(k)] = res.cornerAttrs().get<glm::vec2>(std::string(kProbePrefix) + std::to_string(k) + "@" + std::string(uvSet));
            if (probes[size_t(k)].empty())
                return "it drops extra uv layers";
        }
        tbb::parallel_for(tbb::blocked_range<Index>(0, res.faceCapacity(), 256), [&](const tbb::blocked_range<Index>& r) {
            for (Index f = r.begin(); f < r.end(); ++f)
            {
                if (!res.faceValid(f))
                    continue;
                const int src = sourceFace[f];
                const std::vector<Index>* hood = src >= 0 && Index(src) < hoods.size() ? &hoods[Index(src)] : nullptr;
                res.forEachFaceCorner(f, [&](Index c) {
                    for (int k = 0; k < count; ++k)
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            const double d = (double(probes[size_t(k)][c][ch]) - double(baseUv[c][ch])) / kDelta;
                            if (std::abs(d) < kMinWeight)
                                continue;
                            const int col = 2 * (first + k) + ch;
                            Index found = kInvalid;
                            if (hood)
                                for (Index wd : *hood)
                                    if (colour[wd] == col)
                                    {
                                        found = wd;
                                        break;
                                    }
                            if (found == kInvalid)
                                unattributed[c] += std::abs(d);
                            else
                                entries[c].push_back({found, d});
                        }
                });
            }
        });
    }

    // Check: every response found its wedge, and the map reproduces the exported uvs.
    const std::span<const glm::vec2> realUv = real.cornerAttrs().get<glm::vec2>(uvSet);
    glm::dvec2 lo(1e300), hi(-1e300);
    for (Index c = 0; c < cage.cornerCapacity(); ++c)
        if (w.ofCorner[c] != kInvalid)
        {
            lo = glm::min(lo, glm::dvec2(cageUv[c]));
            hi = glm::max(hi, glm::dvec2(cageUv[c]));
        }
    const double tol = 1e-3 * std::max(1.0, glm::length(hi - lo));
    double worstMiss = 0.0, worstFit = 0.0;
    for (Index f = 0; f < real.faceCapacity(); ++f)
        if (real.faceValid(f))
            real.forEachFaceCorner(f, [&](Index c) {
                worstMiss = std::max(worstMiss, unattributed[c]);
                glm::dvec2 s(0.0);
                for (auto [wd, wt] : entries[c])
                    s += wt * glm::dvec2(cageUv[w.rep[wd]]);
                worstFit = std::max(worstFit, glm::length(s - glm::dvec2(realUv[c])));
            });
    if (worstMiss > 1e-3)
        return "reach";
    if (worstFit > tol)
        return "its uvs are not a blend of the cage's";

    out.rowStart.assign(size_t(rows) + 1, 0);
    for (Index c = 0; c < rows; ++c)
    {
        std::vector<std::pair<Index, double>>& e = entries[c];
        std::sort(e.begin(), e.end());
        out.rowStart[c + 1] = out.rowStart[c] + Index(e.size());
    }
    out.cols.resize(out.rowStart.back());
    out.weights.resize(out.rowStart.back());
    for (Index c = 0; c < rows; ++c)
        for (size_t k = 0; k < entries[c].size(); ++k)
        {
            out.cols[out.rowStart[c] + k] = w.rep[entries[c][k].first];
            out.weights[out.rowStart[c] + k] = entries[c][k].second;
        }
    out.sourceFace.assign(sourceFace.size(), kInvalid);
    for (size_t f = 0; f < sourceFace.size(); ++f)
        if (sourceFace[f] >= 0)
            out.sourceFace[f] = Index(sourceFace[f]);
    out.rings = rings;
    return {};
}
/** Moves an island that grew over its UDIM tile's border back inside, shrinking it only when it is bigger than the tile. */
void keepInTile(Chart& ch, const glm::ivec2& tile)
{
    // Just inside, so the island's uvs never round onto the next tile.
    const glm::dvec2 lo = glm::dvec2(tile) + glm::dvec2(1e-5), hi = glm::dvec2(tile) + glm::dvec2(1.0 - 1e-5);
    glm::dvec2 mn = ch.uvMin(), mx = ch.uvMax();
    const glm::dvec2 size = mx - mn;
    const double fit = std::min(1.0, (hi.x - lo.x) / std::max(size.x, size.y));
    if (fit < 1.0)
    {
        const glm::dvec2 c = (mn + mx) * 0.5;
        for (glm::dvec2& p : ch.uv)
            p = c + (p - c) * fit;
        mn = ch.uvMin();
        mx = ch.uvMax();
    }
    glm::dvec2 shift(0.0);
    for (int k = 0; k < 2; ++k)
        if (mn[k] < lo[k])
            shift[k] = lo[k] - mn[k];
        else if (mx[k] > hi[k])
            shift[k] = hi[k] - mx[k];
    if (shift != glm::dvec2(0.0))
        for (glm::dvec2& p : ch.uv)
            p += shift;
}
} // namespace

FinalMap traceFaces(const geo::Mesh& cage, const Stack& stack)
{
    LILA_ZONE();

    FinalMap map;
    if (stack.empty())
        return map;
    geo::Mesh in = cage;
    std::span<int> ids = in.faceAttrs().getOrAdd<int>(kProbeFace);
    std::span<glm::vec3> centres = in.faceAttrs().getOrAdd<glm::vec3>(kProbeCentre);
    for (Index f = 0; f < in.faceCapacity(); ++f)
        if (in.faceValid(f))
        {
            ids[f] = int(f) + 1;
            centres[f] = glm::vec3(in.faceCentroid(f));
        }
    geo::Mesh out;
    try
    {
        out = runStack(in, stack);
    }
    catch (const std::exception& e)
    {
        map.warning = std::string("the modifier stack failed (") + e.what() + "); measured on the cage";
        return map;
    }
    if (!changesSurface(cage, out))
        return map;
    const std::span<const int> outIds = out.faceAttrs().get<int>(kProbeFace);
    const std::span<const glm::vec3> outCentres = out.faceAttrs().get<glm::vec3>(kProbeCentre);
    if (outIds.empty() || outCentres.empty())
    {
        map.warning = "the modifier stack drops face attributes; measured on the cage";
        return map;
    }
    const std::vector<int> src = sourceFaces(cage, out, outIds, outCentres);
    map.sourceFace.assign(src.size(), kInvalid);
    for (size_t f = 0; f < src.size(); ++f)
        if (src[f] >= 0)
            map.sourceFace[f] = Index(src[f]);
    out.faceAttrs().remove(kProbeFace);
    out.faceAttrs().remove(kProbeCentre);
    map.exported = std::make_shared<geo::Mesh>(std::move(out));
    return map;
}

void keepInUdimTile(Chart& chart, const glm::ivec2& tile)
{
    keepInTile(chart, tile);
}

std::vector<Index> exportedFaces(const FinalMap& map, std::span<const Index> cageFaces)
{
    std::vector<Index> out;
    if (!map.exported)
        return out;
    const geo::Mesh& ex = *map.exported;
    std::vector<uint8_t> want;
    if (!cageFaces.empty())
    {
        Index cap = 0;
        for (Index f : cageFaces)
            cap = std::max(cap, f + 1);
        want.assign(cap, 0);
        for (Index f : cageFaces)
            want[f] = 1;
    }
    for (Index f = 0; f < ex.faceCapacity(); ++f)
    {
        if (!ex.faceValid(f))
            continue;
        const Index src = f < map.sourceFace.size() ? map.sourceFace[f] : kInvalid;
        if (want.empty() || (src < want.size() && want[src]))
            out.push_back(f);
    }
    return out;
}

bool changesSurface(const geo::Mesh& cage, const geo::Mesh& exported)
{
    if (cage.pointCapacity() != exported.pointCapacity() || cage.numPoints() != exported.numPoints() || !sameTopology(cage, exported))
        return true;
    for (Index f = 0; f < cage.faceCapacity(); ++f)
        if (cage.faceValid(f) != exported.faceValid(f) || (cage.faceValid(f) && cage.faceSize(f) != exported.faceSize(f)))
            return true;
    for (Index p = 0; p < cage.pointCapacity(); ++p)
        if (cage.pointValid(p) && glm::length(cage.position(p) - exported.position(p)) > 1e-9 * std::max(1.0, glm::length(cage.position(p))))
            return true;
    return false;
}

geo::Mesh runStack(const geo::Mesh& mesh, const Stack& stack)
{
    geo::Mesh cur = mesh;
    for (const StackStep& s : stack)
        cur = s.apply(cur);
    return cur;
}

FinalMap buildFinalMap(const geo::Mesh& cage, std::string_view uvSet, const Stack& stack)
{
    LILA_ZONE();

    FinalMap map;
    if (stack.empty())
        return map;
    geo::Mesh real;
    try
    {
        real = runStack(cage, stack);
    }
    catch (const std::exception& e)
    {
        map.warning = std::string("the modifier stack failed (") + e.what() + "); solved on the cage";
        return map;
    }
    auto exported = std::make_shared<geo::Mesh>(std::move(real));
    map.exported = exported;
    if (!changesSurface(cage, *exported) || cage.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return map;
    if (exported->cornerAttrs().get<glm::vec2>(uvSet).empty())
    {
        map.warning = "the modifier stack drops " + std::string(uvSet) + "; solved on the cage";
        return map;
    }
    std::string why;
    for (int rings : {3, 6, 12})
    {
        try
        {
            why = probe(cage, uvSet, stack, *exported, rings, map);
        }
        catch (const std::exception& e)
        {
            why = e.what();
        }
        if (why.empty())
        {
            map.method = FinalMethod::Probed;
            return map;
        }
        if (why != "reach")
            break;
    }
    if (why == "reach")
        why = "its uvs depend on cage uvs too far away";
    // Name the first modifier the map breaks at.
    std::string culprit;
    for (size_t n = 1; n <= stack.size() && culprit.empty() && stack.size() > 1; ++n)
    {
        Stack prefix(stack.begin(), stack.begin() + std::ptrdiff_t(n));
        FinalMap trial;
        try
        {
            geo::Mesh r = runStack(cage, prefix);
            if (!changesSurface(cage, r))
                continue;
            if (r.cornerAttrs().get<glm::vec2>(uvSet).empty() || !probe(cage, uvSet, prefix, r, 12, trial).empty())
                culprit = stack[n - 1].name;
        }
        catch (const std::exception&)
        {
            culprit = stack[n - 1].name;
        }
    }
    if (culprit.empty())
        culprit = stack.size() == 1 ? stack.front().name : "the modifier stack";
    map.warning = culprit + " changes uvs in a way uv tools can't follow (" + why + "); solved on the cage";
    map.rowStart.clear();
    map.sourceFace.clear();
    map.cols.clear();
    map.weights.clear();
    return map;
}

double FinalIsland::area3d() const
{
    double a = 0.0;
    for (const auto& t : tris)
        a += 0.5 * glm::length(glm::cross(pos[t[1]] - pos[t[0]], pos[t[2]] - pos[t[0]]));
    return a;
}

std::vector<glm::dvec2> FinalIsland::uvs(const Chart& chart) const
{
    std::vector<glm::dvec2> out(pos.size());
    for (size_t c = 0; c < pos.size(); ++c)
    {
        glm::dvec2 s = offset[c];
        for (Index k = rowStart[c]; k < rowStart[c + 1]; ++k)
            s += weights[k] * chart.uv[cols[k]];
        out[c] = s;
    }
    return out;
}

double FinalIsland::areaUv(const Chart& chart) const
{
    const std::vector<glm::dvec2> y = uvs(chart);
    double a = 0.0;
    for (const auto& t : tris)
        a += signedArea2(y[t[0]], y[t[1]], y[t[2]]);
    return a;
}

std::vector<FinalIsland> finalIslands(const FinalMap& map, const geo::Mesh& cage, std::span<const Chart> charts, std::string_view uvSet)
{
    LILA_ZONE();

    std::vector<FinalIsland> out(charts.size());
    if (!map.active() || !map.exported)
        return out;
    const geo::Mesh& ex = *map.exported;
    std::vector<Index> cornerChart(cage.cornerCapacity(), kInvalid), cornerVertex(cage.cornerCapacity(), kInvalid);
    for (size_t i = 0; i < charts.size(); ++i)
        for (size_t k = 0; k < charts[i].corners.size(); ++k)
        {
            cornerChart[charts[i].corners[k]] = Index(i);
            cornerVertex[charts[i].corners[k]] = charts[i].cornerVertex[k];
        }
    const std::span<const glm::vec2> cageUv = cage.cornerAttrs().get<glm::vec2>(uvSet);
    // Each exported face goes to the chart with most of its weight.
    std::vector<Index> faceChart(ex.faceCapacity(), kInvalid);
    tbb::parallel_for(tbb::blocked_range<Index>(0, ex.faceCapacity(), 512), [&](const tbb::blocked_range<Index>& r) {
        std::vector<std::pair<Index, double>> tally;
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (!ex.faceValid(f))
                continue;
            tally.clear();
            ex.forEachFaceCorner(f, [&](Index c) {
                for (Index k = map.rowStart[c]; k < map.rowStart[c + 1]; ++k)
                {
                    const Index ch = cornerChart[map.cols[k]];
                    if (ch == kInvalid)
                        continue;
                    auto it = std::find_if(tally.begin(), tally.end(), [&](const auto& t) { return t.first == ch; });
                    if (it == tally.end())
                        tally.push_back({ch, std::abs(map.weights[k])});
                    else
                        it->second += std::abs(map.weights[k]);
                }
            });
            if (tally.empty())
                continue;
            faceChart[f] = std::max_element(tally.begin(), tally.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
        }
    });
    std::vector<Index> local(ex.cornerCapacity(), kInvalid), tri;
    std::vector<std::pair<Index, double>> row;
    for (Index f = 0; f < ex.faceCapacity(); ++f)
    {
        const Index ci = faceChart[f];
        if (ci == kInvalid)
            continue;
        FinalIsland& isl = out[ci];
        if (isl.rowStart.empty())
            isl.rowStart.push_back(0);
        ex.forEachFaceCorner(f, [&](Index c) {
            local[c] = Index(isl.pos.size());
            isl.pos.push_back(ex.position(ex.cornerPoint(c)));
            glm::dvec2 off(0.0);
            row.clear();
            for (Index k = map.rowStart[c]; k < map.rowStart[c + 1]; ++k)
            {
                const Index col = map.cols[k];
                if (cornerChart[col] == ci)
                    row.push_back({cornerVertex[col], map.weights[k]});
                else if (col < cageUv.size())
                    off += map.weights[k] * glm::dvec2(cageUv[col]);
            }
            std::sort(row.begin(), row.end());
            for (size_t k = 0; k < row.size(); ++k)
                if (!isl.cols.empty() && isl.cols.size() > isl.rowStart.back() && isl.cols.back() == row[k].first)
                    isl.weights.back() += row[k].second;
                else
                {
                    isl.cols.push_back(row[k].first);
                    isl.weights.push_back(row[k].second);
                }
            isl.rowStart.push_back(Index(isl.cols.size()));
            isl.offset.push_back(off);
        });
        tri.clear();
        geo::triangulateFace(ex, f, tri);
        for (size_t i = 0; i + 2 < tri.size(); i += 3)
        {
            isl.tris.push_back({local[tri[i]], local[tri[i + 1]], local[tri[i + 2]]});
            isl.triFace.push_back(f);
        }
    }
    return out;
}

ChartStats finalStats(const FinalIsland& isl, const Chart& chart)
{
    ChartStats s;
    const std::vector<glm::dvec2> y = isl.uvs(chart);
    std::vector<TriFrame> fr(isl.tris.size());
    double a3 = 0.0, au = 0.0;
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        const auto& t = isl.tris[i];
        fr[i] = triFrame(isl.pos[t[0]], isl.pos[t[1]], isl.pos[t[2]]);
        a3 += fr[i].area;
        au += signedArea2(y[t[0]], y[t[1]], y[t[2]]);
    }
    if (a3 <= 0.0 || au == 0.0)
        return s;
    const double sign = au < 0.0 ? -1.0 : 1.0;
    const double ratio = std::abs(au) / a3;
    double wsum = 0.0;
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        if (fr[i].area <= 0.0)
            continue;
        const auto& t = isl.tris[i];
        const double a = sign * signedArea2(y[t[0]], y[t[1]], y[t[2]]);
        if (a <= 0.0)
        {
            ++s.folded;
            continue;
        }
        const double la = std::abs(std::log2(a / fr[i].area / ratio));
        const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
        const double an = d.s1 / std::max(std::abs(d.s2), 1e-12) - 1.0;
        s.area += fr[i].area * la;
        s.angle += fr[i].area * an;
        s.maxArea = std::max(s.maxArea, la);
        s.maxAngle = std::max(s.maxAngle, an);
        wsum += fr[i].area;
    }
    if (wsum > 0.0)
    {
        s.area /= wsum;
        s.angle /= wsum;
    }
    return s;
}

size_t collapsedTriangles(const FinalIsland& isl, const Chart& chart)
{
    const std::vector<glm::dvec2> y = isl.uvs(chart);
    double a3 = 0.0, au = 0.0;
    std::vector<double> tri3(isl.tris.size()), triUv(isl.tris.size());
    for (size_t i = 0; i < isl.tris.size(); ++i)
    {
        const auto& t = isl.tris[i];
        tri3[i] = 0.5 * glm::length(glm::cross(isl.pos[t[1]] - isl.pos[t[0]], isl.pos[t[2]] - isl.pos[t[0]]));
        triUv[i] = std::abs(signedArea2(y[t[0]], y[t[1]], y[t[2]]));
        a3 += tri3[i];
        au += triUv[i];
    }
    if (a3 <= 0.0)
        return 0;
    const double density = au / a3;
    size_t n = 0;
    for (size_t i = 0; i < isl.tris.size(); ++i)
        if (tri3[i] > 1e-12 * a3 && triUv[i] < tri3[i] * density * kCollapsedFraction)
            ++n;
    return n;
}

void setDensityArea(Chart& chart, const FinalIsland& isl)
{
    chart.densityArea = 0.0;
    if (isl.tris.empty())
        return;
    const double cageUv = std::abs(chart.areaUv()), exUv = std::abs(isl.areaUv(chart)), ex3 = isl.area3d();
    if (cageUv <= 0.0 || exUv <= 0.0 || ex3 <= 0.0)
        return;
    // A collapsed island would read as nearly no density; bound how far the export can move it from the cage's.
    const double a3 = chart.area3d();
    chart.densityArea = std::clamp(ex3 * cageUv / exUv, a3 / 16.0, a3 * 16.0);
}

OptimizeResult solveFinal(Chart& ch, const FinalIsland& isl, const FinalSolveOptions& o)
{
    LILA_ZONE();

    OptimizeResult r;
    const size_t V = ch.pos.size(), C = isl.pos.size(), T = isl.tris.size();
    if (V < 3 || T == 0)
        return r;
    r.before = finalStats(isl, ch);
    size_t pins = 0;
    if (o.usePins)
        for (uint8_t p : ch.pinned)
            pins += p ? 1 : 0;

    const double cageUv = ch.areaUv(), cage3 = ch.area3d();
    if (cage3 <= 0.0 || std::abs(cageUv) <= 1e-300)
        return r;
    const bool mirror = cageUv < 0.0 && pins < 2;
    const double sx = mirror ? -1.0 : 1.0;
    std::vector<glm::dvec2> x(ch.uv.begin(), ch.uv.end()), off(isl.offset);
    for (glm::dvec2& p : x)
        p.x *= sx;
    for (glm::dvec2& p : off)
        p.x *= sx;

    // Exported triangles at the island's density on the cage, so 1 means "as dense as the island is now".
    const double scale = std::sqrt(std::abs(cageUv) / cage3);
    std::vector<TriFrame> fr(T);
    for (size_t i = 0; i < T; ++i)
    {
        const auto& t = isl.tris[i];
        fr[i] = triFrame(isl.pos[t[0]] * scale, isl.pos[t[1]] * scale, isl.pos[t[2]] * scale);
    }

    // y = R x + off: exported corner uvs from chart vertex uvs.
    SpMat R(static_cast<Eigen::Index>(C), static_cast<Eigen::Index>(V));
    {
        std::vector<Triplet> trips;
        trips.reserve(isl.cols.size());
        for (size_t c = 0; c < C; ++c)
            for (Index k = isl.rowStart[c]; k < isl.rowStart[c + 1]; ++k)
                trips.push_back(Triplet(int(c), int(isl.cols[k]), isl.weights[k]));
        R.setFromTriplets(trips.begin(), trips.end());
    }
    const SpMat Rt = R.transpose();

    std::vector<int> freeIdx(V, -1);
    int nFree = 0;
    std::vector<uint8_t> fixed(V, 0);
    if (pins > 0)
        for (size_t v = 0; v < V; ++v)
            fixed[v] = ch.pinned[v];
    else
    {
        // Pin the vertex with the most weight so the system has a fixed point.
        Eigen::VectorXd colWeight = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(V));
        for (int k = 0; k < R.outerSize(); ++k)
            for (SpMat::InnerIterator it(R, k); it; ++it)
                colWeight[it.col()] += std::abs(it.value());
        Eigen::Index best = 0;
        colWeight.maxCoeff(&best);
        fixed[size_t(best)] = 1;
    }
    for (size_t v = 0; v < V; ++v)
        if (!fixed[v])
            freeIdx[v] = nFree++;
    if (nFree == 0)
        return r;

    // The weighted stiffness of the exported triangles and its factorisation over the free chart vertices.
    struct Coupling
    {
        int row, col;
        double w;
    };
    SpMat S(static_cast<Eigen::Index>(C), static_cast<Eigen::Index>(C));
    std::vector<Coupling> kfc;
    auto solver = std::make_unique<Eigen::SimplicialLDLT<SpMat>>();
    auto factor = [&](const std::vector<double>& wt) {
        std::vector<Triplet> trips;
        trips.reserve(T * 9);
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const double a = fr[i].area * wt[i];
            for (int p = 0; p < 3; ++p)
                for (int q = 0; q < 3; ++q)
                    trips.push_back(Triplet(int(isl.tris[i][p]), int(isl.tris[i][q]), a * glm::dot(fr[i].g[p], fr[i].g[q])));
        }
        S.setFromTriplets(trips.begin(), trips.end());
        const SpMat K = (Rt * (S * R)).pruned();
        std::vector<Triplet> kff;
        kfc.clear();
        double diag = 0.0;
        for (int k = 0; k < K.outerSize(); ++k)
            for (SpMat::InnerIterator it(K, k); it; ++it)
            {
                const int fr0 = freeIdx[size_t(it.row())], fc0 = freeIdx[size_t(it.col())];
                if (fr0 < 0)
                    continue;
                if (fc0 >= 0)
                {
                    kff.push_back(Triplet(fr0, fc0, it.value()));
                    if (fr0 == fc0)
                        diag += it.value();
                }
                else
                    kfc.push_back({fr0, int(it.col()), it.value()});
            }
        // Chart vertices no exported triangle reaches keep a tiny diagonal so the system stays solvable.
        const double eps = 1e-9 * std::max(diag / nFree, 1e-30);
        for (int i = 0; i < nFree; ++i)
            kff.push_back(Triplet(i, i, eps));
        SpMat Kff(nFree, nFree);
        Kff.setFromTriplets(kff.begin(), kff.end());
        solver = std::make_unique<Eigen::SimplicialLDLT<SpMat>>(Kff);
        return solver->info() == Eigen::Success;
    };

    auto exportUv = [&](const std::vector<glm::dvec2>& xs, std::vector<glm::dvec2>& y) {
        y.assign(C, glm::dvec2(0.0));
        for (size_t c = 0; c < C; ++c)
        {
            glm::dvec2 s = off[c];
            for (Index k = isl.rowStart[c]; k < isl.rowStart[c + 1]; ++k)
                s += isl.weights[k] * xs[isl.cols[k]];
            y[c] = s;
        }
    };
    // The reweighted rounds pull towards each triangle's own shape whatever the mode: the other modes let slivers stay small.
    bool isometric = o.mode == OptimizeMode::Isometric;
    auto targetOf = [&](const Svd2& d) {
        if (isometric)
            return std::pair<double, double>(1.0, 1.0);
        return targetStretch(o.mode, o.mix, d.s1, d.s2);
    };
    auto energy = [&](const std::vector<glm::dvec2>& y, const std::vector<double>& wt) {
        double e = 0.0;
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = isl.tris[i];
            const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
            auto [t1, t2] = targetOf(d);
            e += wt[i] * fr[i].area * ((d.s1 - t1) * (d.s1 - t1) + (d.s2 - t2) * (d.s2 - t2));
        }
        return e;
    };
    auto exportArea = [&](const std::vector<glm::dvec2>& y) {
        double a = 0.0;
        for (const auto& t : isl.tris)
            a += signedArea2(y[t[0]], y[t[1]], y[t[2]]);
        return a;
    };
    auto centroid = [&](const std::vector<glm::dvec2>& xs) {
        glm::dvec2 s(0.0);
        for (const glm::dvec2& p : xs)
            s += p;
        return s / double(xs.size());
    };
    // Isometric targets carry their own scale; the others would drift without holding the exported area.
    const bool keepArea = pins < 2 && o.mode != OptimizeMode::Isometric && o.mode != OptimizeMode::Authalic;
    double keptArea = 0.0;
    auto restoreArea = [&](std::vector<glm::dvec2>& xs) {
        std::vector<glm::dvec2> y;
        exportUv(xs, y);
        const double a = std::abs(exportArea(y));
        if (a <= 0.0)
            return;
        const glm::dvec2 c = centroid(xs);
        const double k = std::sqrt(keptArea / a);
        for (glm::dvec2& p : xs)
            p = c + (p - c) * k;
    };
    auto folded = [&](const std::vector<glm::dvec2>& y) {
        size_t n = 0;
        for (size_t i = 0; i < T; ++i)
            if (fr[i].area > 0.0 && signedArea2(y[isl.tris[i][0]], y[isl.tris[i][1]], y[isl.tris[i][2]]) <= 0.0)
                ++n;
        return n;
    };
    // Triangles under kCollapsedFraction of the uv area the island's density gives them, as bake lint counts them.
    auto collapsed = [&](const std::vector<glm::dvec2>& y) {
        double a3 = 0.0, au = 0.0;
        for (size_t i = 0; i < T; ++i)
        {
            a3 += fr[i].area;
            au += std::abs(signedArea2(y[isl.tris[i][0]], y[isl.tris[i][1]], y[isl.tris[i][2]]));
        }
        size_t n = 0;
        if (a3 <= 0.0)
            return n;
        const double density = au / a3;
        for (size_t i = 0; i < T; ++i)
            if (fr[i].area > 1e-12 * a3 &&
                std::abs(signedArea2(y[isl.tris[i][0]], y[isl.tris[i][1]], y[isl.tris[i][2]])) < fr[i].area * density * kCollapsedFraction)
                ++n;
        return n;
    };

    const glm::dvec2 centre0 = ch.uvCentroid();
    std::vector<glm::dvec2> y;
    exportUv(x, y);
    keptArea = std::abs(exportArea(y));
    const size_t folded0 = folded(y);
    std::vector<double> wt(T, 1.0);
    if (!factor(wt))
        return r;
    // Steps are free to fold on the way (a flip guard stalls on the slivers this solve exists to open). The result is the
    // iterate with the fewest collapsed triangles, then the lowest plain energy, that folds no more than the start.
    const std::vector<double> plain(T, 1.0);
    std::vector<glm::dvec2> best = x;
    size_t bestCollapsed = collapsed(y);
    double bestE = energy(y, plain);
    auto consider = [&]() {
        if (folded(y) > folded0)
            return;
        const size_t c = collapsed(y);
        const double e = energy(y, plain);
        if (c < bestCollapsed || (c == bestCollapsed && e < bestE))
        {
            best = x;
            bestCollapsed = c;
            bestE = e;
        }
    };

    Eigen::VectorXd bu(nFree), bv(nFree);
    std::vector<glm::dvec2> be(C), trial(V), dir(V), ty;
    std::vector<Jac2> target(T);
    // Local/global iterations with the current weights; true when they ran to a settled energy.
    auto iterate = [&](int maxIt, int& ran) {
        double e0 = energy(y, wt);
        for (int it = 0; it < maxIt; ++it)
        {
            ++ran;
            tbb::parallel_for(size_t(0), T, [&](size_t i) {
                if (fr[i].area <= 0.0)
                    return;
                const auto& t = isl.tris[i];
                const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
                auto [t1, t2] = targetOf(d);
                target[i] = compose(d, t1, t2);
            });
            std::fill(be.begin(), be.end(), glm::dvec2(0.0));
            for (size_t i = 0; i < T; ++i)
            {
                if (fr[i].area <= 0.0)
                    continue;
                const auto& t = isl.tris[i];
                const double a = fr[i].area * wt[i];
                for (int p = 0; p < 3; ++p)
                    be[t[p]] += a * glm::dvec2(fr[i].g[p].x * target[i].a + fr[i].g[p].y * target[i].b,
                                               fr[i].g[p].x * target[i].c + fr[i].g[p].y * target[i].d);
            }
            // rhs = R^T (be - S off), then moved over the fixed vertices.
            const auto nC = static_cast<Eigen::Index>(C);
            Eigen::VectorXd eu(nC), ev(nC), ou(nC), ov(nC);
            for (size_t c = 0; c < C; ++c)
            {
                eu[Eigen::Index(c)] = be[c].x;
                ev[Eigen::Index(c)] = be[c].y;
                ou[Eigen::Index(c)] = off[c].x;
                ov[Eigen::Index(c)] = off[c].y;
            }
            const Eigen::VectorXd fu = Rt * (eu - S * ou), fv = Rt * (ev - S * ov);
            for (size_t v = 0; v < V; ++v)
                if (freeIdx[v] >= 0)
                {
                    bu[freeIdx[v]] = fu[Eigen::Index(v)];
                    bv[freeIdx[v]] = fv[Eigen::Index(v)];
                }
            for (const Coupling& cp : kfc)
            {
                bu[cp.row] -= cp.w * x[size_t(cp.col)].x;
                bv[cp.row] -= cp.w * x[size_t(cp.col)].y;
            }
            const Eigen::VectorXd su = solver->solve(bu), sv = solver->solve(bv);
            if (solver->info() != Eigen::Success || !su.allFinite() || !sv.allFinite())
                return false;
            for (size_t v = 0; v < V; ++v)
                dir[v] = freeIdx[v] >= 0 ? glm::dvec2(su[freeIdx[v]], sv[freeIdx[v]]) - x[v] : glm::dvec2(0.0);
            double step = 1.0, e1 = e0;
            bool accepted = false;
            for (int back = 0; back < 12; ++back)
            {
                for (size_t v = 0; v < V; ++v)
                    trial[v] = x[v] + step * dir[v];
                if (keepArea)
                    restoreArea(trial);
                exportUv(trial, ty);
                e1 = energy(ty, wt);
                if (e1 <= e0)
                {
                    accepted = true;
                    break;
                }
                step *= 0.5;
            }
            if (!accepted)
                return true;
            x.swap(trial);
            y.swap(ty);
            consider();
            const bool settled = e0 - e1 <= 1e-7 * std::max(e0, 1e-30);
            e0 = e1;
            if (settled)
                return true;
        }
        return false;
    };

    int it = 0;
    iterate(o.iterations, it);
    // Then reweighted rounds that weigh squeezed triangles up (towards symmetric Dirichlet's barrier), so slivers open
    // even where they are a small share of the island's area.
    for (int round = 0; round < 6 && collapsed(y) > 0; ++round)
    {
        for (size_t i = 0; i < T; ++i)
        {
            if (fr[i].area <= 0.0)
                continue;
            const auto& t = isl.tris[i];
            const Svd2 d = svd2(jacobian(fr[i], y[t[0]], y[t[1]], y[t[2]]));
            const double s2 = std::max(std::abs(d.s2), 1e-3);
            wt[i] = std::min(1.0 + 0.05 / (s2 * s2), 1e4);
        }
        if (!factor(wt))
            break;
        isometric = true;
        iterate(std::max(10, o.iterations / 4), it);
    }
    for (size_t v = 0; v < V; ++v)
        ch.uv[v] = glm::dvec2(best[v].x * sx, best[v].y);
    if (pins == 0)
    {
        const glm::dvec2 shift = centre0 - ch.uvCentroid();
        for (glm::dvec2& p : ch.uv)
            p += shift;
    }
    r.ok = true;
    r.iterations = it;
    r.after = finalStats(isl, ch);
    return r;
}

FinalRefineResult refineFinal(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack, const FinalSolveOptions& opts)
{
    LILA_ZONE();

    FinalRefineResult out;
    if (stack.empty() || cage.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return out;
    const FinalMap map = buildFinalMap(cage, uvSet, stack);
    out.method = map.method;
    out.warning = map.warning;
    if (!map.active())
        return out;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    const std::vector<Index> live = faces.empty() ? std::vector<Index>{} : islandsOfFaces(cage, liveFaces(cage, faces), co);
    std::vector<Chart> charts = buildCharts(cage, live, co);
    std::vector<FinalIsland> islands = finalIslands(map, cage, charts, uvSet);
    std::vector<OptimizeResult> res(charts.size());
    std::vector<size_t> before(charts.size(), 0), after(charts.size(), 0);
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        if (islands[i].tris.empty())
            return;
        before[i] = collapsedTriangles(islands[i], charts[i]);
        Chart trial = charts[i];
        const glm::ivec2 tile(glm::floor(trial.uvCentroid()));
        res[i] = solveFinal(trial, islands[i], opts);
        if (res[i].ok)
        {
            if (!opts.usePins || std::ranges::none_of(trial.pinned, [](uint8_t p) { return p != 0; }))
                keepInTile(trial, tile);
            charts[i] = std::move(trial);
        }
        after[i] = collapsedTriangles(islands[i], charts[i]);
    });
    double w = 0.0;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (islands[i].tris.empty())
            continue;
        ++out.islands;
        out.collapsedBefore += before[i];
        out.collapsedAfter += after[i];
        if (!res[i].ok)
        {
            ++out.failed;
            continue;
        }
        const double a = islands[i].area3d();
        out.distortionBefore += a * res[i].before.angle;
        out.distortionAfter += a * res[i].after.angle;
        w += a;
    }
    if (w > 0.0)
    {
        out.distortionBefore /= w;
        out.distortionAfter /= w;
    }
    writeCharts(cage, charts, uvSet);
    return out;
}

FinalMap applyDensityAreas(const geo::Mesh& cage, std::span<Chart> charts, std::string_view uvSet, const Stack& stack)
{
    FinalMap map = buildFinalMap(cage, uvSet, stack);
    if (!map.active())
        return map;
    std::vector<FinalIsland> islands = finalIslands(map, cage, charts, uvSet);
    for (size_t i = 0; i < charts.size(); ++i)
        setDensityArea(charts[i], islands[i]);
    return map;
}
} // namespace lila::uv
