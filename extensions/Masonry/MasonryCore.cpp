// Masonry layout: courses of stones laid along a footprint in a chosen bond, corners turned by alternating which wall
// runs through, overlaps cut back, then a little wander so it doesn't look machine laid.

#include "MasonryCore.h"

#include <glm/gtc/matrix_transform.hpp>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
#include <unordered_map>

namespace masonry
{
namespace
{
const glm::dvec3 kUp(0.0, 1.0, 0.0);

glm::dvec3 flat(const glm::dvec3& v) { return {v.x, 0.0, v.z}; }

glm::dvec3 flatDir(const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 d = flat(b - a);
    const double len = glm::length(d);
    return len > 1e-12 ? d / len : glm::dvec3(1, 0, 0);
}

/** Left of a direction in the ground plane. */
glm::dvec3 leftOf(const glm::dvec3& d) { return {-d.z, 0.0, d.x}; }

double crossY(const glm::dvec3& a, const glm::dvec3& b) { return a.z * b.x - a.x * b.z; }

glm::dvec3 miterOffsetImpl(const glm::dvec3& dIn, const glm::dvec3& dOut, double dist)
{
    const glm::dvec3 nIn = leftOf(dIn), nOut = leftOf(dOut);
    glm::dvec3 m = nIn + nOut;
    const double len = glm::length(m);
    if (len < 1e-9)
        return nOut * dist;
    m /= len;
    return m * (dist / std::max(glm::dot(m, nIn), 0.25));
}

uint64_t key(uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0)
{
    uint64_t h = hash64(a);
    h = hash64(h ^ b);
    h = hash64(h ^ c);
    h = hash64(h ^ d);
    return hash64(h ^ e);
}

/** A polyline measured by length, sampled at any distance (wrapping when cyclic). */
struct Line
{
    std::vector<glm::dvec3> pts;
    std::vector<double> at; // cumulative length at each point
    bool cyclic = false;

    void build()
    {
        at.assign(pts.size(), 0.0);
        for (size_t k = 1; k < pts.size(); ++k)
            at[k] = at[k - 1] + glm::length(flat(pts[k] - pts[k - 1]));
        if (cyclic && pts.size() > 1)
        {
            pts.push_back(pts.front());
            at.push_back(at.back() + glm::length(flat(pts.front() - pts[pts.size() - 2])));
        }
    }
    double length() const { return at.empty() ? 0.0 : at.back(); }

    glm::dvec3 sample(double u) const
    {
        const double total = length();
        if (pts.size() < 2 || total <= 0.0)
            return pts.empty() ? glm::dvec3(0.0) : pts.front();
        if (cyclic)
            u = std::fmod(std::fmod(u, total) + total, total);
        if (u <= 0.0)
            return pts.front() + flatDir(pts[0], pts[1]) * u;
        if (u >= total)
            return pts.back() + flatDir(pts[pts.size() - 2], pts.back()) * (u - total);
        const size_t k = size_t(std::upper_bound(at.begin(), at.end(), u) - at.begin()) - 1;
        const double seg = at[k + 1] - at[k];
        const double t = seg > 0.0 ? (u - at[k]) / seg : 0.0;
        return glm::mix(pts[k], pts[k + 1], t);
    }
};

/** A stretch of wall between corners or ends; a closed footprint with no corners is one cyclic run. */
struct Run
{
    std::vector<glm::dvec3> pts;
    bool cyclic = false;
    int startCorner = -1, endCorner = -1; // footprint vertex, or -1 for an open end
    glm::dvec3 before{0.0}, after{0.0};   // directions of the neighbouring runs at the corners
};

struct Clean
{
    std::vector<glm::dvec3> pts;
    bool closed = false;
    double base = 0.0;
};

Clean clean(const Footprint& f)
{
    Clean c;
    c.closed = f.closed;
    c.base = 1e300;
    for (const glm::dvec3& p : f.points)
        c.base = std::min(c.base, p.y);
    for (const glm::dvec3& p : f.points)
    {
        const glm::dvec3 q(p.x, c.base, p.z);
        if (c.pts.empty() || glm::length(flat(q - c.pts.back())) > 1e-3)
            c.pts.push_back(q);
    }
    if (c.closed && c.pts.size() > 2 && glm::length(flat(c.pts.front() - c.pts.back())) <= 1e-3)
        c.pts.pop_back();
    if (c.pts.size() < 3)
        c.closed = false;
    return c;
}

std::vector<Run> splitRuns(const Clean& c, double cornerAngleDeg)
{
    const size_t n = c.pts.size();
    std::vector<Run> runs;
    if (n < 2)
        return runs;
    const double cosLimit = std::cos(glm::radians(std::clamp(cornerAngleDeg, 1.0, 179.0)));
    auto dir = [&](size_t i) { return flatDir(c.pts[i % n], c.pts[(i + 1) % n]); };
    std::vector<size_t> corners;
    const size_t first = c.closed ? 0 : 1, last = c.closed ? n : n - 1;
    for (size_t v = first; v < last; ++v)
        if (glm::dot(dir((v + n - 1) % n), dir(v)) < cosLimit)
            corners.push_back(v);

    if (c.closed && corners.empty())
    {
        Run r;
        r.pts = c.pts;
        r.cyclic = true;
        runs.push_back(std::move(r));
        return runs;
    }
    // Run ends: corners, plus the two ends of an open footprint.
    std::vector<size_t> stops = corners;
    if (!c.closed)
    {
        stops.insert(stops.begin(), 0);
        stops.push_back(n - 1);
    }
    const size_t count = c.closed ? stops.size() : stops.size() - 1;
    for (size_t k = 0; k < count; ++k)
    {
        const size_t a = stops[k], b = c.closed ? stops[(k + 1) % stops.size()] : stops[k + 1];
        Run r;
        for (size_t v = a;; v = (v + 1) % n)
        {
            r.pts.push_back(c.pts[v]);
            if (v == b && r.pts.size() > 1)
                break;
        }
        const bool startIsCorner = c.closed || k > 0;
        const bool endIsCorner = c.closed || k + 1 < count;
        if (startIsCorner)
        {
            r.startCorner = int(a);
            r.before = dir((a + n - 1) % n);
        }
        if (endIsCorner)
        {
            r.endCorner = int(b);
            r.after = dir(b);
        }
        runs.push_back(std::move(r));
    }
    return runs;
}

/** The run's line offset sideways by dist (left positive), mitred at bends and at corners with the next run. */
Line offsetLine(const Run& r, double dist)
{
    Line l;
    l.cyclic = r.cyclic;
    const size_t n = r.pts.size();
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec3 dIn, dOut;
        if (r.cyclic)
        {
            dIn = flatDir(r.pts[(i + n - 1) % n], r.pts[i]);
            dOut = flatDir(r.pts[i], r.pts[(i + 1) % n]);
        }
        else
        {
            dOut = i + 1 < n ? flatDir(r.pts[i], r.pts[i + 1]) : (r.endCorner >= 0 ? r.after : flatDir(r.pts[i - 1], r.pts[i]));
            dIn = i > 0 ? flatDir(r.pts[i - 1], r.pts[i]) : (r.startCorner >= 0 ? r.before : dOut);
        }
        l.pts.push_back(r.pts[i] + miterOffsetImpl(dIn, dOut, dist));
    }
    l.build();
    return l;
}

struct Unit
{
    double width;
    Role role;
    double depth;
};

struct Pattern
{
    std::vector<Unit> lead;
    std::vector<Unit> cycle;
    const Unit& operator[](size_t k) const { return k < lead.size() ? lead[k] : cycle[(k - lead.size()) % cycle.size()]; }
};

Pattern pattern(Bond bond, int course, const Units& u, double stretcherDepth, double headerDepth)
{
    const double j = u.joint;
    const Unit S{u.length, Role::Stretcher, stretcherDepth};
    const Unit H{u.depth, Role::Header, headerDepth};
    const Unit B{(u.length - j) * 0.5, Role::Bat, stretcherDepth};
    const Unit Q{(u.depth - j) * 0.5, Role::Closer, headerDepth};
    const bool odd = (course & 1) != 0;
    switch (bond)
    {
    case Bond::Stack: return {{}, {S}};
    case Bond::English: return odd ? Pattern{{H, Q}, {H}} : Pattern{{}, {S}};
    case Bond::Flemish: return odd ? Pattern{{H, Q}, {S, H}} : Pattern{{}, {S, H}};
    case Bond::Header: return odd ? Pattern{{Q}, {H}} : Pattern{{}, {H}};
    case Bond::GardenWall:
        if (course % 4 == 3)
            return {{H, Q}, {H}};
        return odd ? Pattern{{B}, {S}} : Pattern{{}, {S}};
    case Bond::Running:
    default: return odd ? Pattern{{B}, {S}} : Pattern{{}, {S}};
    }
}

struct Interval
{
    double a, b;
    Unit unit;
};

/**
 * One course on one face: units laid in from both ends toward a point near the middle, the gap left there filled by
 * one cut stone, so cuts land mid wall instead of at a corner.
 */
std::vector<Interval> layCourse(const Pattern& pat, double t0, double t1, double mid, double joint, double minLen)
{
    std::vector<Interval> fromStart, fromEnd, out;
    if (t1 - t0 < minLen)
        return out;
    double pos = t0;
    for (size_t k = 0; k < (size_t(1) << 20); ++k)
    {
        const Unit& u = pat[k];
        if (pos + u.width > mid)
            break;
        fromStart.push_back({pos, pos + u.width, u});
        pos += u.width + joint;
    }
    pos = t1;
    for (size_t k = 0; k < (size_t(1) << 20); ++k)
    {
        const Unit& u = pat[k];
        if (pos - u.width < mid)
            break;
        fromEnd.push_back({pos - u.width, pos, u});
        pos -= u.width + joint;
    }
    const double a = fromStart.empty() ? t0 - joint : fromStart.back().b;
    const double b = fromEnd.empty() ? t1 + joint : fromEnd.back().a;
    const double fill = b - a - 2.0 * joint;
    const Unit& neighbour = !fromStart.empty() ? fromStart.back().unit : (!fromEnd.empty() ? fromEnd.back().unit : pat[0]);
    std::vector<Interval> fillers;
    if (fill >= minLen)
    {
        const Unit filler{0.0, Role::Filler, neighbour.depth};
        const double widest = std::max(pat[0].width, pat.cycle[0].width);
        if (fill > widest + minLen + joint)
        {
            const double w = (fill - joint) * 0.5;
            fillers.push_back({a + joint, a + joint + w, filler});
            fillers.push_back({b - joint - w, b - joint, filler});
        }
        else
            fillers.push_back({a + joint, b - joint, filler});
    }
    else if (fill > -joint)
    {
        // Too little room for a stone: the stones either side take it up instead.
        const double extra = fill + joint;
        if (!fromStart.empty() && !fromEnd.empty())
        {
            fromStart.back().b += extra * 0.5;
            fromEnd.back().a -= extra * 0.5;
        }
        else if (!fromStart.empty())
            fromStart.back().b += extra;
        else if (!fromEnd.empty())
            fromEnd.back().a -= extra;
    }
    out = std::move(fromStart);
    out.insert(out.end(), fillers.begin(), fillers.end());
    out.insert(out.end(), fromEnd.rbegin(), fromEnd.rend());
    return out;
}

// --- Oriented boxes ---

glm::dvec3 halfExtentAabb(const Stone& s)
{
    const glm::dvec3 h = s.size * 0.5;
    glm::dvec3 e(0.0);
    for (int k = 0; k < 3; ++k)
        e += glm::abs(s.frame[k]) * h[k];
    return e;
}

std::array<glm::dvec3, 8> boxCorners(const Stone& s)
{
    std::array<glm::dvec3, 8> c;
    const glm::dvec3 h = s.size * 0.5;
    for (int i = 0; i < 8; ++i)
    {
        const glm::dvec3 sign((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0);
        c[size_t(i)] = s.center + s.frame * (sign * h);
    }
    return c;
}

/** Where along one of s's axes another box lies, relative to s's centre. */
std::pair<double, double> projectOnto(const Stone& s, int axis, const Stone& other)
{
    double lo = 1e300, hi = -1e300;
    for (const glm::dvec3& p : boxCorners(other))
    {
        const double t = glm::dot(p - s.center, s.frame[axis]);
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    return {lo, hi};
}

/**
 * Cuts s back along its length or depth so it clears `other` by a joint, keeping the larger side. False when what would
 * remain is too small to be a stone.
 */
bool trimAgainst(Stone& s, const Stone& other, double joint, double minLength, double minDepth)
{
    int bestAxis = -1;
    double bestLo = 0.0, bestHi = 0.0, bestKeep = -1.0;
    for (int axis : {0, 2})
    {
        const double e = s.size[axis] * 0.5;
        const auto [lo, hi] = projectOnto(s, axis, other);
        const double cutLo = lo - joint, cutHi = hi + joint;
        if (cutHi <= -e || cutLo >= e)
            continue;
        const double leftHi = std::min(e, cutLo), rightLo = std::max(-e, cutHi);
        const double left = leftHi + e, right = e - rightLo;
        const double keep = std::max(left, right);
        const double minimum = axis == 0 ? minLength : minDepth;
        if (keep < minimum || keep >= 2.0 * e - 1e-9)
            continue;
        const double frac = keep / (2.0 * e);
        if (frac > bestKeep)
        {
            bestKeep = frac;
            bestAxis = axis;
            if (left >= right)
            {
                bestLo = -e;
                bestHi = leftHi;
            }
            else
            {
                bestLo = rightLo;
                bestHi = e;
            }
        }
    }
    if (bestAxis < 0)
        return false;
    s.center += s.frame[bestAxis] * ((bestLo + bestHi) * 0.5);
    s.size[bestAxis] = bestHi - bestLo;
    if (bestAxis == 0)
        s.role = Role::Closer;
    return true;
}

/** Stones in a coarse grid, for finding the ones a new stone might touch. */
struct Grid
{
    double cell;
    std::unordered_map<uint64_t, std::vector<int>> cells;

    static uint64_t pack(int64_t x, int64_t y, int64_t z)
    {
        constexpr int64_t off = 1 << 20;
        return (uint64_t(x + off) & 0x1FFFFF) | ((uint64_t(y + off) & 0x1FFFFF) << 21) | ((uint64_t(z + off) & 0x1FFFFF) << 42);
    }
    template <class Fn> void forCells(const Stone& s, double pad, Fn&& fn) const
    {
        const glm::dvec3 e = halfExtentAabb(s) + pad;
        const glm::dvec3 lo = glm::floor((s.center - e) / cell), hi = glm::floor((s.center + e) / cell);
        for (int64_t x = int64_t(lo.x); x <= int64_t(hi.x); ++x)
            for (int64_t y = int64_t(lo.y); y <= int64_t(hi.y); ++y)
                for (int64_t z = int64_t(lo.z); z <= int64_t(hi.z); ++z)
                    fn(pack(x, y, z));
    }
    void insert(const Stone& s, int index)
    {
        forCells(s, 0.0, [&](uint64_t k) { cells[k].push_back(index); });
    }
    std::vector<int> around(const Stone& s, double pad) const
    {
        std::vector<int> out;
        forCells(s, pad, [&](uint64_t k) {
            auto it = cells.find(k);
            if (it != cells.end())
                out.insert(out.end(), it->second.begin(), it->second.end());
        });
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }
};

glm::dmat3 rotationXyz(double ax, double ay, double az)
{
    const glm::dmat4 r = glm::rotate(glm::rotate(glm::rotate(glm::dmat4(1.0), az, glm::dvec3(0, 0, 1)), ay, glm::dvec3(0, 1, 0)), ax,
                                     glm::dvec3(1, 0, 0));
    return glm::dmat3(r);
}

int faceRank(Face f) { return f == Face::Through ? 0 : (f == Face::Outer ? 1 : 2); }
} // namespace

glm::dvec3 miterOffset(const glm::dvec3& dIn, const glm::dvec3& dOut, double dist) { return miterOffsetImpl(dIn, dOut, dist); }

uint64_t hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

double unitRandom(uint64_t k) { return double(hash64(k) >> 11) * (1.0 / 9007199254740992.0); }

const char* bondName(Bond b)
{
    switch (b)
    {
    case Bond::Running: return "running";
    case Bond::Stack: return "stack";
    case Bond::English: return "english";
    case Bond::Flemish: return "flemish";
    case Bond::Header: return "header";
    case Bond::GardenWall: return "gardenWall";
    }
    return "running";
}

bool parseBond(std::string_view name, Bond* out)
{
    for (Bond b : {Bond::Running, Bond::Stack, Bond::English, Bond::Flemish, Bond::Header, Bond::GardenWall})
        if (name == bondName(b))
        {
            *out = b;
            return true;
        }
    return false;
}

std::vector<std::string> bondNames()
{
    std::vector<std::string> out;
    for (Bond b : {Bond::Running, Bond::Stack, Bond::English, Bond::Flemish, Bond::Header, Bond::GardenWall})
        out.emplace_back(bondName(b));
    return out;
}

glm::dmat4 stoneMatrix(const Stone& s)
{
    glm::dmat4 m(s.frame);
    m[3] = glm::dvec4(s.center, 1.0);
    return m;
}

bool overlaps(const Stone& a, const Stone& b, double tolerance)
{
    const glm::dvec3 ha = glm::max(a.size * 0.5 - tolerance * 0.5, glm::dvec3(0.0));
    const glm::dvec3 hb = glm::max(b.size * 0.5 - tolerance * 0.5, glm::dvec3(0.0));
    const glm::dmat3 r = glm::transpose(a.frame) * b.frame; // b's axes in a's frame
    const glm::dvec3 t = glm::transpose(a.frame) * (b.center - a.center);
    glm::dmat3 absR;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            absR[j][i] = std::abs(r[j][i]) + 1e-12;
    auto row = [&](const glm::dmat3& m, int i) { return glm::dvec3(m[0][i], m[1][i], m[2][i]); };
    for (int i = 0; i < 3; ++i)
        if (std::abs(t[i]) > ha[i] + glm::dot(row(absR, i), hb))
            return false;
    for (int j = 0; j < 3; ++j)
        if (std::abs(glm::dot(t, r[j])) > glm::dot(ha, absR[j]) + hb[j])
            return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
            const double ra = ha[i1] * absR[j][i2] + ha[i2] * absR[j][i1];
            const double rb = hb[j1] * absR[j2][i] + hb[j2] * absR[j1][i];
            const double d = t[i2] * r[j][i1] - t[i1] * r[j][i2];
            if (std::abs(d) > ra + rb)
                return false;
        }
    return true;
}

Layout layoutWall(const Footprint& footprint, const WallParams& p, int wall)
{
    Layout out;
    const Clean c = clean(footprint);
    if (c.pts.size() < 2 || p.height <= 0.0)
        return out;
    const Units& u = p.stone;
    const double j = std::max(0.0, u.joint);
    const double T = std::max(p.thickness, 1.0);
    const bool single = T < u.depth * 1.5;
    const double stretcherDepth = single ? T : std::min(u.depth, (T - j) * 0.5);
    const double headerDepth = single ? T : std::min(u.length, T);
    const double minLen = std::max(2.0, u.length * 0.2);

    out.courses = std::max(1, int(std::lround(p.height / (u.height + j))));
    out.pitch = p.height / out.courses;
    const double h = std::max(0.5, out.pitch - j);

    // Which side of a closed footprint faces out.
    bool leftIsOuter = true;
    if (c.closed)
    {
        double area = 0.0;
        for (size_t i = 0; i < c.pts.size(); ++i)
        {
            const glm::dvec3& a = c.pts[i];
            const glm::dvec3& b = c.pts[(i + 1) % c.pts.size()];
            area += a.x * b.z - b.x * a.z;
        }
        leftIsOuter = area < 0.0;
    }

    const std::vector<Run> runs = splitRuns(c, p.cornerAngle);
    std::vector<Stone> laid;
    for (size_t ri = 0; ri < runs.size(); ++ri)
    {
        const Run& r = runs[ri];
        const std::vector<double> sides = single ? std::vector<double>{0.0} : std::vector<double>{1.0, -1.0};
        for (double side : sides)
        {
            const Line line = offsetLine(r, side * T * 0.5);
            const double total = line.length();
            // At a corner, the side that goes round the outside of the turn either runs to the corner (this run
            // passes through the corner on this course) or stops short of the other run's stones.
            auto cornerTrim = [&](int corner, const glm::dvec3& dIn, const glm::dvec3& dOut, bool atEnd, int course, double ownerDepth) {
                const double s = std::max(std::abs(crossY(dIn, dOut)), 0.35);
                const bool owner = ((course + corner) % 2 == 0) == atEnd;
                const glm::dvec3 vertex = atEnd ? r.pts.back() : r.pts.front();
                const glm::dvec3 miter = vertex + miterOffset(dIn, dOut, side * T * 0.5);
                const bool convex = glm::dot(flat(miter - vertex), atEnd ? dIn : -dOut) > 1e-9 || single;
                // Inside a turn the faces meet at the mitre, and stones pushed out of one face move into the other.
                if (!convex)
                    return owner ? 0.0 : j + std::max(0.0, p.protrude);
                if (single)
                    return owner ? -(T * 0.5) / s : (T * 0.5) / s + j;
                return owner ? 0.0 : (ownerDepth + j) / s;
            };
            const Face face = single ? Face::Through : ((side > 0.0) == leftIsOuter ? Face::Outer : Face::Inner);
            for (int course = 0; course < out.courses; ++course)
            {
                const Pattern pat = pattern(p.bond, course, u, stretcherDepth, headerDepth);
                double t0 = 0.0, t1 = total - (r.cyclic ? j : 0.0);
                Pattern used = pat;
                if (r.cyclic)
                {
                    // No ends to start from: the course starts where the bond says, round the loop.
                    used.lead.clear();
                    const double shift = (course & 1) ? (used.cycle[0].width + j) * 0.5 : 0.0;
                    t0 += shift;
                    t1 += shift;
                }
                else
                {
                    if (r.startCorner >= 0)
                        t0 += cornerTrim(r.startCorner, r.before, flatDir(r.pts[0], r.pts[1]), false, course, pat[0].depth);
                    if (r.endCorner >= 0)
                        t1 -= cornerTrim(r.endCorner, flatDir(r.pts[r.pts.size() - 2], r.pts.back()), r.after, true, course, pat[0].depth);
                }
                const double drift = (unitRandom(key(p.seed, uint64_t(wall), ri, uint64_t(course), 17)) - 0.5) * u.length * 0.8;
                double mid = (t0 + t1) * 0.5 + drift;
                if (t1 - t0 > 2.0 * u.length)
                    mid = std::clamp(mid, t0 + u.length, t1 - u.length);
                for (const Interval& iv : layCourse(used, t0, t1, mid, j, minLen))
                {
                    const glm::dvec3 A = line.sample(iv.a), B = line.sample(iv.b);
                    const double width = glm::length(flat(B - A));
                    if (width < 1e-3)
                        continue;
                    const glm::dvec3 along = flatDir(A, B);
                    // Into the wall: away from the face this side lays.
                    glm::dvec3 into = side > 0.0 ? -leftOf(along) : leftOf(along);
                    glm::dvec3 x = along;
                    if (glm::dot(glm::cross(x, kUp), into) < 0.0)
                        x = -x;
                    Stone s;
                    s.frame = glm::dmat3(x, kUp, into);
                    s.size = {width, h, iv.unit.depth};
                    const glm::dvec3 faceMid = (A + B) * 0.5;
                    s.center = faceMid + (side == 0.0 ? glm::dvec3(0.0) : into * (iv.unit.depth * 0.5));
                    s.center.y = c.base + course * out.pitch + h * 0.5;
                    s.wall = wall;
                    s.course = course;
                    s.run = int32_t(ri);
                    s.role = iv.unit.role;
                    s.face = iv.unit.depth >= T - j - 1e-6 ? Face::Through : face;
                    laid.push_back(s);
                }
            }
        }
    }

    // Through stones and the outer face win, then the inner face fits round them.
    std::vector<size_t> order(laid.size());
    for (size_t k = 0; k < order.size(); ++k)
        order[k] = k;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (laid[a].course != laid[b].course)
            return laid[a].course < laid[b].course;
        return faceRank(laid[a].face) < faceRank(laid[b].face);
    });
    Grid grid{std::max(u.length, 20.0), {}};
    const double minDepth = std::max(1.0, stretcherDepth * 0.3);
    // Most of a joint between any two stones, so the wander below can't close it.
    const double clearance = -0.75 * j;
    for (size_t k : order)
    {
        Stone s = laid[k];
        bool alive = true, trimmed = false;
        for (int pass = 0; pass < 6 && alive; ++pass)
        {
            const Stone* hit = nullptr;
            for (int other : grid.around(s, j))
                if (overlaps(s, out.stones[size_t(other)], clearance))
                {
                    hit = &out.stones[size_t(other)];
                    break;
                }
            if (!hit)
                break;
            alive = trimAgainst(s, *hit, j, minLen, minDepth);
            trimmed = true;
            if (pass == 5 && alive)
                for (int other : grid.around(s, j))
                    if (overlaps(s, out.stones[size_t(other)], clearance))
                        alive = false;
        }
        if (!alive)
        {
            ++out.dropped;
            continue;
        }
        out.trimmed += trimmed ? 1 : 0;
        grid.insert(s, int(out.stones.size()));
        out.stones.push_back(s);
    }

    // Wander: half the joint at most per stone, split between moving and tilting, so neighbours never meet.
    const double budget = 0.25 * j * std::clamp(p.jitter, 0.0, 1.0);
    for (size_t k = 0; k < out.stones.size(); ++k)
    {
        Stone& s = out.stones[k];
        auto rnd = [&](uint64_t n) { return unitRandom(key(p.seed, uint64_t(wall), k, n, 91)); };
        s.random = float(rnd(0));
        const double step = budget / std::sqrt(3.0);
        const glm::dvec3 shift((rnd(1) * 2 - 1) * step, (rnd(2) * 2 - 1) * step, -rnd(3) * std::max(0.0, p.protrude));
        const double halfDiag = glm::length(s.size) * 0.5;
        const double maxAngle = halfDiag > 0.0 ? (budget / 3.0) / halfDiag : 0.0;
        const glm::dmat3 tilt = rotationXyz((rnd(4) * 2 - 1) * maxAngle, (rnd(5) * 2 - 1) * maxAngle, (rnd(6) * 2 - 1) * maxAngle);
        s.center += s.frame * shift;
        s.frame = s.frame * tilt;
    }
    return out;
}

size_t countOverlaps(std::span<const Stone> stones, double tolerance)
{
    double cell = 20.0;
    for (const Stone& s : stones)
        cell = std::max(cell, std::max({s.size.x, s.size.y, s.size.z}));
    Grid grid{cell, {}};
    size_t count = 0;
    for (size_t k = 0; k < stones.size(); ++k)
    {
        for (int other : grid.around(stones[k], 0.0))
            if (overlaps(stones[k], stones[size_t(other)], tolerance))
                ++count;
        grid.insert(stones[k], int(k));
    }
    return count;
}

geo::Mesh coreMesh(const Footprint& footprint, const WallParams& p)
{
    const Clean c = clean(footprint);
    const size_t n = c.pts.size();
    if (n < 2 || p.height <= 0.0)
        return {};
    const double T = std::max(p.thickness, 1.0);
    double half = T * 0.5 - std::max(0.0, p.recess);
    if (half < T * 0.25)
        half = T * 0.25;
    const double top = c.base + std::max(1.0, p.height - p.stone.joint - std::max(0.0, p.recess));
    const size_t segs = c.closed ? n : n - 1;
    auto dir = [&](size_t i) { return flatDir(c.pts[i % n], c.pts[(i + 1) % n]); };

    std::vector<glm::dvec3> pts;
    for (size_t i = 0; i < n; ++i)
    {
        glm::dvec3 dIn, dOut;
        if (c.closed)
        {
            dIn = dir((i + n - 1) % n);
            dOut = dir(i);
        }
        else
        {
            dOut = i + 1 < n ? dir(i) : dir(n - 2);
            dIn = i > 0 ? dir(i - 1) : dOut;
        }
        const glm::dvec3 off = miterOffset(dIn, dOut, half);
        for (const glm::dvec3& q : {c.pts[i] + off, c.pts[i] - off})
        {
            pts.push_back({q.x, c.base, q.z});
            pts.push_back({q.x, top, q.z});
        }
    }
    // Point i: left bottom 4i, left top 4i+1, right bottom 4i+2, right top 4i+3.
    auto lb = [](size_t i) { return geo::Index(4 * i); };
    auto lt = [](size_t i) { return geo::Index(4 * i + 1); };
    auto rb = [](size_t i) { return geo::Index(4 * i + 2); };
    auto rt = [](size_t i) { return geo::Index(4 * i + 3); };

    std::vector<geo::Index> sizes, idx;
    auto quad = [&](std::array<geo::Index, 4> q, const glm::dvec3& outward) {
        glm::dvec3 nrm(0.0);
        for (size_t k = 0; k < 4; ++k)
            nrm += glm::cross(pts[q[k]], pts[q[(k + 1) % 4]]);
        if (glm::dot(nrm, outward) < 0.0)
            std::reverse(q.begin(), q.end());
        sizes.push_back(4);
        idx.insert(idx.end(), q.begin(), q.end());
    };
    for (size_t s = 0; s < segs; ++s)
    {
        const size_t a = s, b = (s + 1) % n;
        const glm::dvec3 nrm = leftOf(dir(s));
        quad({lb(a), lb(b), lt(b), lt(a)}, nrm);
        quad({rb(a), rb(b), rt(b), rt(a)}, -nrm);
        quad({lt(a), lt(b), rt(b), rt(a)}, kUp);
        quad({lb(a), lb(b), rb(b), rb(a)}, -kUp);
    }
    if (!c.closed)
    {
        quad({lb(0), lt(0), rt(0), rb(0)}, -dir(0));
        quad({lb(n - 1), lt(n - 1), rt(n - 1), rb(n - 1)}, dir(n - 2));
    }
    geo::Mesh core = geo::Mesh::fromPolygons(pts, sizes, idx);
    lila::scene::materials::assign(core, kMortarSlot, {});
    return core;
}

geo::Mesh stoneMesh(const StoneShape& s)
{
    const glm::dvec3 e = glm::max(s.size * 0.5, glm::dvec3(0.05));
    const double minE = std::min({e.x, e.y, e.z});
    const double b = s.detail == Detail::Box ? 0.0 : std::clamp(s.bevel, 0.0, 0.45 * minE);
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes, idx;
    auto face = [&](std::vector<geo::Index> f, const glm::dvec3& outward) {
        glm::dvec3 nrm(0.0);
        for (size_t k = 0; k < f.size(); ++k)
            nrm += glm::cross(pts[f[k]], pts[f[(k + 1) % f.size()]]);
        if (glm::dot(nrm, outward) < 0.0)
            std::reverse(f.begin(), f.end());
        sizes.push_back(geo::Index(f.size()));
        idx.insert(idx.end(), f.begin(), f.end());
    };
    auto sign = [](int corner, int axis) { return (corner >> axis) & 1 ? 1.0 : -1.0; };

    if (b <= 1e-6)
    {
        for (int c = 0; c < 8; ++c)
            pts.push_back(glm::dvec3(sign(c, 0), sign(c, 1), sign(c, 2)) * e);
        for (int axis = 0; axis < 3; ++axis)
            for (int side = 0; side < 2; ++side)
            {
                const int i = (axis + 1) % 3, j = (axis + 2) % 3;
                std::vector<geo::Index> f;
                for (int q : {0, 1, 3, 2})
                    f.push_back(geo::Index((side << axis) | ((q & 1) << i) | (((q >> 1) & 1) << j)));
                glm::dvec3 outward(0.0);
                outward[axis] = side ? 1.0 : -1.0;
                face(f, outward);
            }
    }
    else
    {
        // Each box corner splits into three points, one on each face meeting there.
        for (int c = 0; c < 8; ++c)
        {
            const glm::dvec3 sg(sign(c, 0), sign(c, 1), sign(c, 2));
            double knock = 0.0;
            if (s.detail == Detail::Worn)
            {
                const double r = unitRandom(uint64_t(s.seed) * 8 + uint64_t(c));
                knock = std::min(std::clamp(s.wear, 0.0, 1.0) * b * (0.3 + 1.7 * r * r), 0.4 * minE - b);
                knock = std::max(knock, 0.0);
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                glm::dvec3 q;
                for (int k = 0; k < 3; ++k)
                    q[k] = sg[k] * (k == axis ? e[k] : e[k] - b);
                q -= sg * knock;
                pts.push_back(q);
            }
        }
        auto at = [](int corner, int axis) { return geo::Index(corner * 3 + axis); };
        for (int axis = 0; axis < 3; ++axis)
        {
            const int i = (axis + 1) % 3, j = (axis + 2) % 3;
            for (int side = 0; side < 2; ++side)
            {
                std::vector<geo::Index> f;
                for (int q : {0, 1, 3, 2})
                    f.push_back(at((side << axis) | ((q & 1) << i) | (((q >> 1) & 1) << j), axis));
                glm::dvec3 outward(0.0);
                outward[axis] = side ? 1.0 : -1.0;
                face(f, outward);
            }
            // Chamfers along this axis, between the faces of the other two.
            for (int si = 0; si < 2; ++si)
                for (int sj = 0; sj < 2; ++sj)
                {
                    const int lo = (si << i) | (sj << j), hi = lo | (1 << axis);
                    glm::dvec3 outward(0.0);
                    outward[i] = si ? 1.0 : -1.0;
                    outward[j] = sj ? 1.0 : -1.0;
                    face({at(lo, i), at(hi, i), at(hi, j), at(lo, j)}, outward);
                }
        }
        for (int c = 0; c < 8; ++c)
            face({at(c, 0), at(c, 1), at(c, 2)}, glm::dvec3(sign(c, 0), sign(c, 1), sign(c, 2)));
    }

    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    // Box-projected UVs at the given scale, offset per seed so stones read different parts of the texture.
    const glm::dvec2 offset(unitRandom(uint64_t(s.seed) * 2 + 101), unitRandom(uint64_t(s.seed) * 2 + 102));
    const double tile = std::max(s.tile, 1e-3);
    std::span<glm::vec2> uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 nrm = glm::abs(m.faceNormal(f));
        const int axis = nrm.x >= nrm.y && nrm.x >= nrm.z ? 0 : (nrm.y >= nrm.z ? 1 : 2);
        const int ua = axis == 0 ? 2 : 0, va = axis == 1 ? 2 : 1;
        for (geo::Index c : m.faceCorners(f))
        {
            const glm::dvec3& q = m.position(m.cornerPoint(c));
            uv[c] = glm::vec2(float((q[ua] + e[ua]) / tile + offset.x), float((q[va] + e[va]) / tile + offset.y));
        }
    }
    std::span<bool> hard = m.edgeAttrs().getOrAdd<bool>(geo::attr::kHard);
    std::fill(hard.begin(), hard.end(), true);
    lila::scene::materials::assign(m, kStoneSlot, {});
    return m;
}

std::array<geo::Mesh, 3> stoneLods(StoneShape s)
{
    std::array<geo::Mesh, 3> out;
    const Detail levels[] = {Detail::Worn, Detail::Clean, Detail::Box};
    for (size_t k = 0; k < out.size(); ++k)
    {
        s.detail = levels[k];
        out[k] = stoneMesh(s);
    }
    return out;
}

geo::Mesh prismMesh(const std::array<glm::dvec3, 4>& ground, double y0, double y1)
{
    std::vector<glm::dvec3> pts;
    for (const glm::dvec3& g : ground)
        pts.push_back({g.x, y0, g.z});
    for (const glm::dvec3& g : ground)
        pts.push_back({g.x, y1, g.z});
    glm::dvec3 mid(0.0);
    for (const glm::dvec3& q : pts)
        mid += q / 8.0;
    std::vector<geo::Index> sizes, idx;
    auto quad = [&](std::array<geo::Index, 4> q) {
        glm::dvec3 nrm(0.0), centre(0.0);
        for (size_t k = 0; k < 4; ++k)
        {
            nrm += glm::cross(pts[q[k]], pts[q[(k + 1) % 4]]);
            centre += pts[q[k]] / 4.0;
        }
        if (glm::dot(nrm, centre - mid) < 0.0)
            std::reverse(q.begin(), q.end());
        sizes.push_back(4);
        idx.insert(idx.end(), q.begin(), q.end());
    };
    quad({0, 1, 2, 3});
    quad({4, 5, 6, 7});
    for (geo::Index k = 0; k < 4; ++k)
        quad({k, geo::Index((k + 1) % 4), geo::Index((k + 1) % 4 + 4), geo::Index(k + 4)});
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

std::vector<geo::Mesh> collisionHulls(const Footprint& footprint, const WallParams& p, int bands)
{
    std::vector<geo::Mesh> out;
    const Clean c = clean(footprint);
    const size_t n = c.pts.size();
    if (n < 2 || p.height <= 0.0)
        return out;
    const double half = std::max(p.thickness, 1.0) * 0.5;
    const int count = std::clamp(bands, 1, 16);
    auto dir = [&](size_t i) { return flatDir(c.pts[i % n], c.pts[(i + 1) % n]); };
    auto offsetAt = [&](size_t i) {
        if (c.closed)
            return miterOffset(dir((i + n - 1) % n), dir(i), half);
        const glm::dvec3 dOut = i + 1 < n ? dir(i) : dir(n - 2);
        return miterOffset(i > 0 ? dir(i - 1) : dOut, dOut, half);
    };
    const size_t segs = c.closed ? n : n - 1;
    for (size_t s = 0; s < segs; ++s)
    {
        const size_t a = s, b = (s + 1) % n;
        const glm::dvec3 oa = offsetAt(a), ob = offsetAt(b);
        const std::array<glm::dvec3, 4> ground = {c.pts[a] + oa, c.pts[b] + ob, c.pts[b] - ob, c.pts[a] - oa};
        for (int band = 0; band < count; ++band)
        {
            const double y0 = c.base + p.height * band / count, y1 = c.base + p.height * (band + 1) / count;
            out.push_back(prismMesh(ground, y0, y1));
        }
    }
    return out;
}

geo::Mesh mergeStones(const Built& b, std::span<const int> stones, int lod, const glm::dvec3& origin)
{
    geo::Mesh out;
    const int level = std::clamp(lod, 0, 2);
    for (int k : stones)
    {
        const size_t r = size_t(b.reference[size_t(k)]);
        geo::Mesh m = b.lods.empty() ? b.meshes[r] : b.lods[r][size_t(level)];
        Stone s = b.stones[size_t(k)];
        s.center -= origin;
        const glm::dmat4 xf = stoneMatrix(s);
        for (glm::dvec3& q : m.positions())
            q = glm::dvec3(xf * glm::dvec4(q, 1.0));
        out.append(m);
    }
    return out;
}

Chunks chunkStones(std::span<const Stone> stones, double size)
{
    Chunks c;
    const double cell = std::max(size, 1.0);
    std::vector<glm::ivec2> at(stones.size());
    for (size_t k = 0; k < stones.size(); ++k)
        at[k] = glm::ivec2(int(std::floor(stones[k].center.x / cell)), int(std::floor(stones[k].center.z / cell)));
    auto less = [](const glm::ivec2& a, const glm::ivec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; };
    c.cells = at;
    std::sort(c.cells.begin(), c.cells.end(), less);
    c.cells.erase(std::unique(c.cells.begin(), c.cells.end()), c.cells.end());
    c.of.resize(stones.size());
    for (size_t k = 0; k < stones.size(); ++k)
        c.of[k] = int(std::lower_bound(c.cells.begin(), c.cells.end(), at[k], less) - c.cells.begin());
    return c;
}

std::vector<Footprint> footprintsFromEdges(const geo::Mesh& m)
{
    std::vector<Footprint> out;
    std::vector<geo::Index> loose;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) == 0)
            loose.push_back(e);
    if (loose.empty())
        return out;
    std::map<geo::Index, std::vector<geo::Index>> at; // point -> loose edges
    for (geo::Index e : loose)
        for (int k = 0; k < 2; ++k)
            at[m.edgePoint(e, k)].push_back(e);
    std::map<geo::Index, bool> used;
    auto walk = [&](geo::Index start, geo::Index firstEdge) {
        Footprint f;
        f.points.push_back(m.position(start));
        geo::Index p = start, e = firstEdge;
        while (e != geo::kInvalid && !used[e])
        {
            used[e] = true;
            p = m.edgeOtherPoint(e, p);
            if (p == start)
            {
                f.closed = true;
                break;
            }
            f.points.push_back(m.position(p));
            e = geo::kInvalid;
            const auto& next = at[p];
            if (next.size() == 2)
                for (geo::Index n : next)
                    if (!used[n])
                        e = n;
        }
        if (f.points.size() >= 2)
            out.push_back(std::move(f));
    };
    // Lines first, from their ends (and junctions), then whatever is left is loops.
    for (const auto& [p, edges] : at)
        if (edges.size() != 2)
            for (geo::Index e : edges)
                if (!used[e])
                    walk(p, e);
    for (geo::Index e : loose)
        if (!used[e])
            walk(m.edgePoint(e, 0), e);
    return out;
}

geo::Mesh edgesFromFootprints(std::span<const Footprint> footprints)
{
    geo::Mesh m;
    for (const Footprint& f : footprints)
    {
        std::vector<geo::Index> ids;
        for (const glm::dvec3& p : f.points)
            ids.push_back(m.addPoint(p));
        for (size_t k = 0; k + 1 < ids.size(); ++k)
            m.addEdge(ids[k], ids[k + 1]);
        if (f.closed && ids.size() > 2)
            m.addEdge(ids.back(), ids.front());
    }
    return m;
}

Built buildWalls(std::span<const Footprint> footprints, const BuildOptions& o)
{
    Built b;
    for (size_t w = 0; w < footprints.size(); ++w)
    {
        Layout l = layoutWall(footprints[w], o.wall, int(w));
        b.courses = std::max(b.courses, l.courses);
        b.trimmed += l.trimmed;
        b.dropped += l.dropped;
        b.stones.insert(b.stones.end(), l.stones.begin(), l.stones.end());
        b.core.append(coreMesh(footprints[w], o.wall));
    }
    const int variants = std::max(1, o.variants);
    using Key = std::tuple<int64_t, int64_t, int64_t, int>;
    std::map<Key, int> refs;
    std::vector<std::pair<glm::dvec3, int>> shapes; // size and variant per reference
    b.reference.resize(b.stones.size());
    b.variant.resize(b.stones.size());
    for (size_t k = 0; k < b.stones.size(); ++k)
    {
        const Stone& s = b.stones[k];
        const int v = std::min(variants - 1, int(double(s.random) * variants));
        // Sizes rounded down to a quarter centimetre (never grown into a neighbour), so the common stones share a handful of meshes.
        const glm::dvec3 q = glm::floor(s.size * 4.0);
        const Key key{int64_t(q.x), int64_t(q.y), int64_t(q.z), v};
        auto [it, added] = refs.emplace(key, int(shapes.size()));
        if (added)
            shapes.push_back({q / 4.0, v});
        b.reference[k] = it->second;
        b.variant[k] = v;
    }
    b.meshes.resize(shapes.size());
    if (o.lods)
        b.lods.resize(shapes.size());
    tbb::parallel_for(size_t(0), shapes.size(), [&](size_t r) {
        StoneShape shape;
        shape.size = shapes[r].first;
        shape.bevel = o.bevel;
        shape.wear = o.wear;
        shape.seed = uint32_t(hash64(uint64_t(o.wall.seed) * 1000003ull + uint64_t(shapes[r].second)));
        shape.detail = o.detail;
        shape.tile = o.tile;
        b.meshes[r] = stoneMesh(shape);
        if (o.lods)
            b.lods[r] = stoneLods(shape);
    });
    return b;
}
} // namespace masonry
