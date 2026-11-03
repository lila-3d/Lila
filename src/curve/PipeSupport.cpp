#include "curve/PipeSupport.h"

#include "core/Profile.h"
#include "curve/Ops.h"
#include "geo/Attribute.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace lila::curve
{
namespace
{
using D3 = glm::dvec3;
using geo::Index;

/** Turns a mesh built along +z so it runs along +y (a rotation about x). */
geo::Mesh zToY(geo::Mesh m)
{
    for (D3& p : m.positions())
        p = D3(p.x, p.z, -p.y);
    return m;
}

/** A cylinder along +y from y0 to y1. */
geo::Mesh rod(double radius, double y0, double y1, int sides)
{
    return zToY(lathe({{0.0, y0}, {radius, y0}, {radius, y1}, {0.0, y1}}, false, sides));
}

/** A box between two corners, outward faces, a unit square of uvs per face. */
geo::Mesh box(const D3& lo, const D3& hi)
{
    geo::Mesh m;
    Index p[8];
    for (int i = 0; i < 8; ++i)
        p[i] = m.addPoint(D3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
    const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    auto uvs = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (auto& q : quads)
    {
        Index f = m.addFace(std::vector<Index>{p[q[0]], p[q[1]], p[q[2]], p[q[3]]});
        uvs = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        int k = 0;
        m.forEachFaceCorner(f, [&](Index c) { uvs[c] = uv[k++]; });
    }
    return m;
}

/** A square beam of the given size from a to b. */
geo::Mesh beam(const D3& a, const D3& b, double size)
{
    const double len = glm::distance(a, b);
    if (len < 1e-9)
        return {};
    geo::Mesh m = box(D3(-size * 0.5, -size * 0.5, 0.0), D3(size * 0.5, size * 0.5, len));
    const D3 z = (b - a) / len;
    const D3 x = glm::normalize(glm::cross(std::abs(z.y) < 0.9 ? D3(0, 1, 0) : D3(1, 0, 0), z));
    const D3 y = glm::cross(z, x);
    for (D3& p : m.positions())
        p = a + x * p.x + y * p.y + z * p.z;
    return m;
}

/** A user mesh with +y toward the surface; stretched along y so its top reaches the surface. */
geo::Mesh userSupport(const geo::Mesh& src, double scale, bool stretch, double reach)
{
    geo::Mesh m = src;
    double top = 0.0;
    for (const D3& p : m.positions())
        top = std::max(top, p.y * scale);
    const double sy = stretch && top > 1e-9 ? reach / top : 1.0;
    for (D3& p : m.positions())
        p = D3(p.x * scale, p.y * scale * sy, p.z * scale);
    return m;
}

void tag(geo::Mesh& m, int index)
{
    auto pp = m.faceAttrs().getOrAdd<int32_t>("pipePart");
    auto part = m.faceAttrs().getOrAdd<int32_t>("part");
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            pp[f] = int32_t(PipePart::Support);
            part[f] = index;
        }
}

/** Unit tangent of the polyline at arc length s. */
D3 tangentAt(const Polyline& pl, double s)
{
    double frac = 0.0;
    size_t i = pl.segmentAt(s, &frac);
    size_t j = (i + 1) % pl.size();
    D3 d = pl.points[j] - pl.points[i];
    return glm::length(d) > 1e-12 ? glm::normalize(d) : pl.tangent(i);
}
} // namespace

const char* supportStyleName(SupportStyle s)
{
    switch (s)
    {
    case SupportStyle::Auto: return "auto";
    case SupportStyle::Hanger: return "hanger";
    case SupportStyle::Bracket: return "bracket";
    case SupportStyle::Stand: return "stand";
    case SupportStyle::Clamp: return "clamp";
    case SupportStyle::Mesh: return "mesh";
    }
    return "auto";
}

const char* supportTargetName(SupportTarget t)
{
    switch (t)
    {
    case SupportTarget::Nearest: return "nearest";
    case SupportTarget::Floor: return "floor";
    case SupportTarget::Ceiling: return "ceiling";
    case SupportTarget::Wall: return "wall";
    }
    return "nearest";
}

SupportResult makeSupports(const Spline& input, const SupportOptions& o)
{
    LILA_ZONE();

    SupportResult r;
    if (!isValid(input) || !o.cast)
        return r;
    const double R = std::max(o.pipeRadius, 1e-3);
    FilletOptions fo;
    fo.radius = o.bendRadius > 0.0 ? o.bendRadius : 2.0 * R;
    const Spline path = fillet(input, fo);
    TessOptions tess;
    tess.mode = TessMode::Game;
    const Polyline pl = tessellate(path, tess);
    if (pl.size() < 2 || pl.length < 1e-6)
        return r;
    const double L = pl.length;
    const D3 up = glm::length(o.up) > 1e-9 ? glm::normalize(o.up) : D3(0, 1, 0);
    const double clearance = o.endClearance > 0.0 ? o.endClearance : 2.0 * R;
    const double spacing = std::max(o.spacing, R);
    const double T = o.ringThickness > 0.0 ? o.ringThickness : std::max(R * 0.15, 0.1);
    const double W = o.ringWidth > 0.0 ? o.ringWidth : std::max(R * 0.6, 0.5);
    const double rodR = o.rodSize > 0.0 ? o.rodSize * 0.5 : std::max(R * 0.12, 0.3);
    const int sides = std::clamp(o.sides, 3, 64);

    // A station is usable on a straight stretch: the tangent clearance either side of it matches.
    auto straight = [&](double s) {
        if (!pl.closed && (s < clearance || s > L - clearance))
            return false;
        return glm::dot(tangentAt(pl, s - clearance), tangentAt(pl, s + clearance)) > 0.999;
    };
    std::vector<double> stations;
    for (double s = o.startOffset >= 0.0 ? o.startOffset : spacing * 0.5; s <= L + 1e-6; s += spacing)
    {
        double found = -1.0;
        for (int k = 0; k <= 16 && found < 0.0; ++k)
        {
            double off = (k + 1) / 2 * (spacing * 0.4 / 8.0) * (k % 2 ? 1.0 : -1.0);
            if (straight(s + off))
                found = s + off;
        }
        if (found >= 0.0)
            stations.push_back(found);
    }

    int missing = 0;
    for (double s : stations)
    {
        const D3 at = pl.pointAtLength(s);
        const D3 t = tangentAt(pl, s);
        D3 side = glm::cross(t, up);
        if (glm::length(side) < 1e-6)
            side = glm::cross(t, std::abs(t.x) < 0.9 ? D3(1, 0, 0) : D3(0, 0, 1));
        side = glm::normalize(side);
        std::vector<D3> dirs;
        if (o.target == SupportTarget::Nearest || o.target == SupportTarget::Floor)
            dirs.push_back(-up);
        if (o.target == SupportTarget::Nearest || o.target == SupportTarget::Ceiling)
            dirs.push_back(up);
        if (o.target == SupportTarget::Nearest || o.target == SupportTarget::Wall)
        {
            dirs.push_back(side);
            dirs.push_back(-side);
        }
        std::optional<SurfacePoint> best;
        D3 bestDir(0.0);
        double bestReach = o.maxReach;
        for (const D3& d : dirs)
        {
            if (std::abs(glm::dot(d, t)) > 0.9)
                continue;
            // From just outside the pipe, so the pipe itself (when it is among the surfaces) isn't hit.
            const double start = R * 1.01;
            std::optional<SurfacePoint> hit = o.cast(at + d * start, d, bestReach - start);
            if (!hit)
                continue;
            const double reach = glm::distance(hit->position, at);
            if (reach < bestReach)
            {
                bestReach = reach;
                best = hit;
                bestDir = d;
            }
        }
        if (!best)
        {
            ++missing;
            continue;
        }

        // Local frame: x sideways, y toward the surface, z along the pipe.
        const D3 dy = bestDir;
        const D3 dz = glm::normalize(t - dy * glm::dot(t, dy));
        const D3 dx = glm::cross(dy, dz);
        SupportStyle style = o.style;
        if (style == SupportStyle::Auto)
            style = glm::dot(dy, up) > 0.7 ? SupportStyle::Hanger : glm::dot(dy, up) < -0.7 ? SupportStyle::Stand : SupportStyle::Bracket;
        const double D = bestReach;
        const double ring = R + T;
        const double plateT = std::max(T, 0.2), plateS = std::max(W * 1.6, rodR * 6.0);

        geo::Mesh m;
        if (style == SupportStyle::Mesh)
        {
            if (o.mesh)
                m = userSupport(*o.mesh, o.meshScale, o.stretch, D);
        }
        else
        {
            m = clampPart(R, R + 4.0 * T, 2.0 * W, sides, true);
            switch (style)
            {
            case SupportStyle::Hanger:
                m.append(rod(rodR, ring, D - plateT, 8));
                m.append(box(D3(-plateS * 0.35, D - plateT, -plateS * 0.35), D3(plateS * 0.35, D, plateS * 0.35)));
                break;
            case SupportStyle::Bracket:
            {
                const double a = rodR * 2.0;
                m.append(box(D3(-a * 0.5, ring, -a * 0.5), D3(a * 0.5, D - plateT, a * 0.5)));
                // The brace runs from under the arm's middle down to the wall.
                D3 down = glm::dvec3(glm::dot(-up, dx), 0.0, glm::dot(-up, dz));
                down = glm::length(down) > 1e-6 ? glm::normalize(down) : D3(1, 0, 0);
                const double h = (D - ring) * 0.6;
                m.append(beam(D3(0.0, ring + (D - ring) * 0.35, 0.0) + down * (a * 0.5), D3(0.0, D - plateT, 0.0) + down * h, a * 0.8));
                const D3 plateLo = D3(-plateS * 0.5, D - plateT, -plateS * 0.5), plateHi = D3(plateS * 0.5, D, plateS * 0.5);
                m.append(box(glm::min(plateLo, plateLo + down * h), glm::max(plateHi, plateHi + down * h)));
                break;
            }
            case SupportStyle::Stand:
                m.append(box(D3(-R * 1.1, R * 0.7, -W), D3(R * 1.1, ring + T, W)));
                m.append(rod(rodR * 1.8, ring + T, D - plateT, sides));
                m.append(box(D3(-plateS * 0.6, D - plateT, -plateS * 0.6), D3(plateS * 0.6, D, plateS * 0.6)));
                break;
            default:
                m.append(beam(D3(0.0, ring, 0.0), D3(0.0, D - plateT, 0.0), rodR * 2.0));
                m.append(box(D3(-plateS * 0.4, D - plateT, -plateS * 0.4), D3(plateS * 0.4, D, plateS * 0.4)));
                break;
            }
        }
        if (m.numFaces() == 0)
            continue;
        for (D3& p : m.positions())
            p = at + dx * p.x + dy * p.y + dz * p.z;
        tag(m, int(r.placements.size()));
        r.mesh.append(m);
        r.placements.push_back({s, at, best->position, best->normal, dy, D, style});
    }
    if (missing > 0)
        r.warnings.push_back(std::format("{} support{} found no surface within {:.0f} cm", missing, missing == 1 ? "" : "s", o.maxReach));
    return r;
}
} // namespace lila::curve
