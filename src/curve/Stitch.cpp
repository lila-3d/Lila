#include "curve/Stitch.h"

#include "core/Profile.h"
#include "curve/Tessellate.h"
#include "geo/Attribute.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lila::curve
{
using D3 = glm::dvec3;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/** One stitch from hole (s0, x0) to hole (s1, x1) in path coordinates, lifted clear of stitches it crosses. */
struct StitchDef
{
    double s0, x0, s1, x1;
    double lift = 0.0;
    double vShift = 0.0;
};

/** Deterministic noise in [-1, 1]. */
struct Rng
{
    uint32_t state;
    double next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return double(state) / double(0xffffffffu) * 2.0 - 1.0;
    }
};

std::vector<double> rowOffsets(const StitchOptions& o)
{
    const int rows = std::clamp(o.rows, 1, 16);
    std::vector<double> out;
    for (int r = 0; r < rows; ++r)
    {
        double x = 0.0;
        switch (o.align)
        {
        case StitchAlign::Center: x = (r - (rows - 1) * 0.5) * o.rowSpacing; break;
        case StitchAlign::Left: x = r * o.rowSpacing; break;
        case StitchAlign::Right: x = -r * o.rowSpacing; break;
        }
        out.push_back(x + o.offset);
    }
    return out;
}

std::vector<StitchDef> layout(double len, bool closed, const StitchOptions& o)
{
    std::vector<StitchDef> out;
    double L = std::max(0.01, o.length);
    double pitch = L + std::max(0.0, o.gap);
    const bool blanket = o.pattern == StitchPattern::Blanket;
    if (len <= 1e-6 || (!closed && len < (blanket ? 0.0 : L * 0.5)))
        return out;
    int n = 0;
    double lead = 0.0;
    if (closed)
    {
        n = std::max(1, int(std::lround(len / pitch)));
        const double k = len / (n * pitch);
        L *= k;
        pitch *= k;
    }
    else if (blanket)
    {
        n = int(std::floor(len / pitch)) + 1;
        lead = (len - (n - 1) * pitch) * 0.5;
    }
    else
    {
        L = std::min(L, len);
        n = int(std::floor((len - L) / pitch)) + 1;
        lead = (len - ((n - 1) * pitch + L)) * 0.5;
    }

    const double w = std::max(0.0, o.width);
    const double crossLift = std::max(0.0, o.diameter) * 0.9;
    Rng rng{o.seed * 2654435761u + 0x9e3779b9u};
    for (double xr : rowOffsets(o))
        for (int k = 0; k < n; ++k)
        {
            const double j = std::clamp(o.jitter, 0.0, 1.0);
            const double ds = j * rng.next() * 0.12 * pitch, dx = j * rng.next() * 0.08 * std::max(L, w);
            const double turn = j * rng.next() * 8.0 * kPi / 180.0, stretch = 1.0 + j * rng.next() * 0.12;
            const double vShift = std::max(0.0, o.uvRandom) * (rng.next() * 0.5 + 0.5);
            const double s = lead + k * pitch + ds, x = xr + dx;
            switch (o.pattern)
            {
            case StitchPattern::Running:
            {
                const double c = s + L * 0.5, half = L * 0.5 * stretch, th = o.rotation * kPi / 180.0 + turn;
                const double hs = half * std::cos(th), hx = half * std::sin(th);
                out.push_back({c - hs, x - hx, c + hs, x + hx, 0.0, vShift});
                break;
            }
            case StitchPattern::Zigzag:
            {
                const double side = k % 2 ? -1.0 : 1.0;
                out.push_back({s, x - side * w * 0.5, s + L * stretch, x + side * w * 0.5, 0.0, vShift});
                break;
            }
            case StitchPattern::Cross:
                out.push_back({s, x - w * 0.5, s + L * stretch, x + w * 0.5, 0.0, vShift});
                out.push_back({s, x + w * 0.5, s + L * stretch, x - w * 0.5, crossLift, vShift});
                break;
            case StitchPattern::Blanket:
                out.push_back({s, x + w * stretch, s, x, 0.0, vShift});
                if (closed || k + 1 < n)
                    out.push_back({s, x, s + pitch, x, 0.0, vShift});
                break;
            }
        }
    return out;
}

/** Ply radius as a fraction of the thread's, for 1, 2 and 3 plies packed in its circle. */
double plyFraction(int plies) { return plies == 1 ? 1.0 : plies == 2 ? 0.5 : 0.464; }
} // namespace

const char* stitchPatternName(StitchPattern p)
{
    switch (p)
    {
    case StitchPattern::Running: return "running";
    case StitchPattern::Zigzag: return "zigzag";
    case StitchPattern::Cross: return "cross";
    case StitchPattern::Blanket: return "blanket";
    }
    return "running";
}

StitchResult buildStitches(double pathLength, bool closed, const StitchSurface& surface, const StitchOptions& o)
{
    LILA_ZONE();

    StitchResult res;
    if (!surface)
        return res;
    const std::vector<StitchDef> defs = layout(pathLength, closed, o);
    const int segs = std::clamp(o.segments, 2, 64), sides = std::clamp(o.sides, 3, 32), plies = std::clamp(o.plies, 1, 3);
    const double r = std::max(1e-4, o.diameter * 0.5), flat = 1.0 - std::clamp(o.flatten, 0.0, 0.95);
    const double rp = r * plyFraction(plies), plyOff = r - rp;
    const double top = o.restHeight + r * flat, inset = std::max(0.0, o.inset);
    const double vPerCm = std::max(1e-6, o.uvScale) / (kPi * 2.0 * r);

    geo::Mesh& m = res.mesh;
    std::vector<std::array<glm::vec2, 4>> faceUv;
    std::vector<D3> P(segs + 1), N(segs + 1), C(segs + 1);
    std::vector<double> arc(segs + 1);
    std::vector<geo::Index> ring((segs + 1) * sides);
    for (const StitchDef& d : defs)
    {
        bool ok = true;
        for (int i = 0; i <= segs && ok; ++i)
        {
            const double u = double(i) / segs;
            const std::optional<SurfacePoint> sp = surface(d.s0 + (d.s1 - d.s0) * u, d.x0 + (d.x1 - d.x0) * u);
            ok = sp.has_value();
            if (!ok)
                break;
            // A flat-topped arch: steep out of each hole, level along the cloth.
            const double rise = std::pow(std::sin(kPi * u), 0.3);
            P[i] = sp->position;
            N[i] = sp->normal;
            C[i] = P[i] + N[i] * (-inset + (top + inset + d.lift) * rise);
        }
        if (!ok)
            continue;
        arc[0] = 0.0;
        for (int i = 1; i <= segs; ++i)
            arc[i] = arc[i - 1] + glm::distance(C[i], C[i - 1]);
        const D3 chord = P[segs] - P[0];
        const double vBase = o.continuousUv ? std::min(d.s0, d.s1) : 0.0;

        for (int ply = 0; ply < plies; ++ply)
        {
            for (int i = 0; i <= segs; ++i)
            {
                D3 t = C[std::min(i + 1, segs)] - C[std::max(i - 1, 0)];
                t = glm::length(t) > 1e-12 ? glm::normalize(t) : D3(0, 0, 1);
                D3 along = P[std::min(i + 1, segs)] - P[std::max(i - 1, 0)];
                if (glm::length(along) < 1e-9)
                    along = chord;
                D3 side = glm::cross(N[i], along);
                side -= t * glm::dot(side, t);
                if (glm::length(side) < 1e-9)
                    side = glm::cross(t, std::abs(t.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0));
                side = glm::normalize(side);
                const D3 up = glm::cross(t, side);
                const double phase = 2.0 * kPi * (double(ply) / plies + o.twist * arc[i]);
                const D3 centre = plies > 1 ? C[i] + plyOff * (std::cos(phase) * side + flat * std::sin(phase) * up) : C[i];
                for (int k = 0; k < sides; ++k)
                {
                    const double a = 2.0 * kPi * k / sides;
                    ring[i * sides + k] = m.addPoint(centre + rp * (std::cos(a) * side + flat * std::sin(a) * up));
                }
            }
            for (int i = 0; i < segs; ++i)
            {
                const float v0 = float((vBase + arc[i]) * vPerCm + d.vShift), v1 = float((vBase + arc[i + 1]) * vPerCm + d.vShift);
                for (int k = 0; k < sides; ++k)
                {
                    const int k1 = (k + 1) % sides;
                    const geo::Index f = m.addFace({ring[i * sides + k], ring[i * sides + k1], ring[(i + 1) * sides + k1], ring[(i + 1) * sides + k]});
                    if (f == geo::kInvalid)
                        continue;
                    if (faceUv.size() <= size_t(f))
                        faceUv.resize(size_t(f) + 1);
                    const float u0 = float(k) / sides, u1 = float(k + 1) / sides;
                    faceUv[f] = {glm::vec2(u0, v0), glm::vec2(u1, v0), glm::vec2(u1, v1), glm::vec2(u0, v1)};
                }
            }
        }
        ++res.stitches;
    }

    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity() && size_t(f) < faceUv.size(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        size_t c = 0;
        m.forEachFaceCorner(f, [&](geo::Index cc) {
            if (c < 4)
                uv[cc] = faceUv[f][c++];
        });
    }
    return res;
}

StitchSurface polylineSurface(std::vector<D3> points, std::vector<D3> normals, bool closed, ClosestSurface snap)
{
    if (points.size() < 2 || normals.size() != points.size())
        return {};
    auto pl = std::make_shared<Polyline>(polylineFrom(points, closed));
    auto nrm = std::make_shared<std::vector<D3>>(std::move(normals));
    return [pl, nrm, closed, snap](double s, double x) -> std::optional<SurfacePoint> {
        const size_t n = pl->size();
        if (closed)
            s = std::fmod(std::fmod(s, pl->length) + pl->length, pl->length);
        else
            s = std::clamp(s, 0.0, pl->length);
        double frac = 0.0;
        const size_t i0 = std::min(pl->segmentAt(s, &frac), n - 1), i1 = closed ? (i0 + 1) % n : std::min(i0 + 1, n - 1);
        const D3 p = glm::mix(pl->points[i0], pl->points[i1], frac);
        D3 nn = glm::mix((*nrm)[i0], (*nrm)[i1], frac);
        nn = glm::length(nn) > 1e-12 ? glm::normalize(nn) : D3(0, 1, 0);
        D3 t = glm::mix(pl->tangent(i0), pl->tangent(i1), frac);
        D3 side = glm::cross(nn, t);
        side = glm::length(side) > 1e-12 ? glm::normalize(side) : D3(0.0);
        const D3 q = p + side * x;
        if (snap)
            if (std::optional<SurfacePoint> hit = snap(q))
                return hit;
        return SurfacePoint{q, nn};
    };
}

const std::vector<std::string>& stitchPresetNames()
{
    static const std::vector<std::string> names{"running", "topstitch", "saddle", "denim", "heavyLeather", "zigzag", "cross", "blanket"};
    return names;
}

std::optional<StitchOptions> stitchPreset(std::string_view name)
{
    StitchOptions o;
    if (name == "running")
        return o;
    if (name == "topstitch")
    {
        o.rows = 2;
        o.rowSpacing = 0.6;
        o.length = 0.35;
        o.gap = 0.15;
        o.diameter = 0.07;
        o.flatten = 0.3;
        return o;
    }
    if (name == "saddle")
    {
        o.length = 0.5;
        o.gap = 0.2;
        o.rotation = 25.0;
        o.diameter = 0.12;
        o.plies = 2;
        o.flatten = 0.15;
        return o;
    }
    if (name == "denim")
    {
        o.rows = 2;
        o.rowSpacing = 0.65;
        o.length = 0.35;
        o.gap = 0.12;
        o.diameter = 0.1;
        o.plies = 3;
        o.twist = 3.0;
        return o;
    }
    if (name == "heavyLeather")
    {
        o.length = 0.6;
        o.gap = 0.3;
        o.rotation = 20.0;
        o.diameter = 0.16;
        o.plies = 3;
        o.inset = 0.12;
        o.jitter = 0.15;
        return o;
    }
    if (name == "zigzag")
    {
        o.pattern = StitchPattern::Zigzag;
        o.length = 0.3;
        o.gap = 0.0;
        o.width = 0.5;
        o.diameter = 0.07;
        return o;
    }
    if (name == "cross")
    {
        o.pattern = StitchPattern::Cross;
        o.length = 0.5;
        o.gap = 0.15;
        o.width = 0.5;
        o.diameter = 0.1;
        o.plies = 2;
        return o;
    }
    if (name == "blanket")
    {
        o.pattern = StitchPattern::Blanket;
        o.length = 0.3;
        o.gap = 0.3;
        o.width = 0.6;
        o.diameter = 0.1;
        o.plies = 2;
        return o;
    }
    return std::nullopt;
}
} // namespace lila::curve
