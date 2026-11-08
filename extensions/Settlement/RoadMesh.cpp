// Settlement: road and sidewalk meshes swept along the road network's cross-sections, junction patches with curb
// returns at the corners, and crosswalk stripes.

#include "Streetscape.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace settlement
{
namespace
{
inline Vec2 flat(const glm::dvec3& p) { return {p.x, p.z}; }
/** The kerb side a driver has on the right, seen from above with Y up. */
inline Vec2 rightSide(Vec2 d) { return {-d.y, d.x}; }

/** Points along a polyline no further apart than seg, the polyline's own corners kept. */
std::vector<glm::dvec3> resample(const std::vector<glm::dvec3>& pts, double seg)
{
    std::vector<glm::dvec3> out;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
    {
        const int n = std::max(1, int(std::ceil(glm::length(flat(pts[k + 1]) - flat(pts[k])) / seg)));
        for (int i = 0; i < n; ++i)
            out.push_back(pts[k] + (pts[k + 1] - pts[k]) * (double(i) / n));
    }
    if (!pts.empty())
        out.push_back(pts.back());
    return out;
}

/** Mitred right-hand offsets per point: the unit side times the mitre length. */
std::vector<Vec2> mitres(const std::vector<glm::dvec3>& pts)
{
    std::vector<Vec2> out(pts.size(), Vec2(0.0));
    auto dirAt = [&](size_t a, size_t b) {
        const Vec2 d = flat(pts[b]) - flat(pts[a]);
        const double len = glm::length(d);
        return len > 1e-9 ? d / len : Vec2(0.0);
    };
    for (size_t k = 0; k < pts.size(); ++k)
    {
        const Vec2 d0 = k > 0 ? dirAt(k - 1, k) : Vec2(0.0), d1 = k + 1 < pts.size() ? dirAt(k, k + 1) : Vec2(0.0);
        Vec2 d = d0 + d1;
        if (glm::length(d) < 1e-9)
            d = glm::length(d1) > 0.0 ? d1 : d0;
        if (glm::length(d) < 1e-9)
            continue;
        d /= glm::length(d);
        const Vec2 n = rightSide(d);
        const Vec2 ref = glm::length(d1) > 0.0 ? rightSide(d1) : rightSide(d0);
        const double c = std::max(glm::dot(n, ref), 0.5); // mitres longer than 2x are clipped
        out[k] = n / c;
    }
    return out;
}

double areaXZ(std::span<const glm::dvec3> pts)
{
    double a = 0.0;
    for (size_t k = 0; k < pts.size(); ++k)
    {
        const glm::dvec3 &p = pts[k], &q = pts[(k + 1) % pts.size()];
        a += p.x * q.z - q.x * p.z;
    }
    return 0.5 * a;
}

/** Builds one mesh with its per-face attributes, written once every face is in. */
struct MeshBuilder
{
    geo::Mesh& mesh;
    std::vector<int32_t> part, surface, line, junction;

    /** A face facing up (clockwise in x, z), or for walls as given. Returns its area from above, m2. */
    double face(std::vector<glm::dvec3> pts, RoadPart p, Surface s, int lineId, int junctionId, bool wall = false)
    {
        const double a = areaXZ(pts);
        if (!wall && a > 0.0)
            std::reverse(pts.begin(), pts.end());
        std::vector<geo::Index> ids;
        for (const glm::dvec3& q : pts)
            ids.push_back(mesh.addPoint(q));
        mesh.addFace(ids);
        part.push_back(int32_t(p));
        surface.push_back(int32_t(s));
        line.push_back(lineId);
        junction.push_back(junctionId);
        return wall ? 0.0 : std::abs(a) / kM2;
    }

    void finish()
    {
        const std::pair<const char*, std::vector<int32_t>*> layers[] = {{"part", &part}, {"surface", &surface}, {"line", &line}, {"junction", &junction}};
        for (const auto& [name, values] : layers)
            mesh.faceAttrs().add(name, geo::AttrType::Int);
        for (const auto& [name, values] : layers)
        {
            std::span<int32_t> out = mesh.faceAttrs().get<int32_t>(name);
            std::copy(values->begin(), values->begin() + std::min(values->size(), out.size()), out.begin());
        }
    }
};

/** A quadratic corner from a to b whose tangents run along da and db (both pointing away from the junction). */
std::vector<Vec2> corner(Vec2 a, Vec2 da, Vec2 b, Vec2 db, int steps)
{
    // The control point is where the two kerb lines meet, behind both ends; otherwise the corner is straight.
    Vec2 c = 0.5 * (a + b);
    const double den = cross(da, db);
    if (std::abs(den) > 1e-6)
    {
        const double t = cross(b - a, db) / den, s = cross(b - a, da) / den;
        if (t < 0.0 && s < 0.0)
            c = a + da * t;
    }
    std::vector<Vec2> out;
    for (int i = 0; i <= steps; ++i)
    {
        const double t = double(i) / steps;
        out.push_back((1 - t) * (1 - t) * a + 2 * (1 - t) * t * c + t * t * b);
    }
    return out;
}
} // namespace

const char* surfaceName(Surface s)
{
    switch (s)
    {
    case Surface::Asphalt: return "asphalt";
    case Surface::Cobble: return "cobble";
    case Surface::Earth: return "earth";
    case Surface::Paving: return "paving";
    }
    return "asphalt";
}

bool parseSurface(std::string_view name, Surface* out)
{
    for (Surface s : {Surface::Asphalt, Surface::Cobble, Surface::Earth, Surface::Paving})
        if (name == surfaceName(s))
        {
            *out = s;
            return true;
        }
    return false;
}

bool medievalStyle(const Layout& l)
{
    const std::string& n = l.profile.name;
    return n == "hamlet" || n == "streetVillage" || n.find("medieval") != std::string::npos || n.find("castrum") != std::string::npos ||
           n.find("medina") != std::string::npos || n.find("fantasy") != std::string::npos;
}

Surface streetSurface(const Layout& l, StreetClass c, const RoadProfile& p)
{
    if (medievalStyle(l))
        return c <= StreetClass::Collector ? Surface::Cobble : Surface::Earth;
    if (p.lanes == 0)
        return Surface::Paving;
    if (p.name == "track")
        return Surface::Earth;
    return Surface::Asphalt;
}

RoadSection roadSection(const Layout& l, const StreetLine& line, bool allowKerbless)
{
    const RoadProfile& p = l.roads[size_t(line.profile)];
    RoadSection s;
    s.surface = streetSurface(l, line.cls, p);
    const double f = p.width() > 1e-6 ? line.width / p.width() : 1.0;
    s.median = p.median * f;
    s.carriage = 0.5 * (p.lanes * p.laneWidth + p.median) * f + p.parking * f;
    s.sidewalk = p.sidewalk * f;
    s.verge = p.verge * f;
    if (p.lanes == 0)
    {
        // Footpaths and stairs: all of it is walked on.
        s.carriage = s.verge = s.median = 0.0;
        s.sidewalk = 0.5 * line.width;
    }
    if (allowKerbless && medievalStyle(l))
    {
        s.kerbless = true;
        s.carriage = 0.5 * line.width;
        s.sidewalk = s.verge = s.median = 0.0;
    }
    return s;
}

RoadMeshes roadMeshes(const Layout& l, const RoadNetwork& net, const Ground& ground, const RoadMeshOptions& o)
{
    LILA_ZONE();

    RoadMeshes out;
    MeshBuilder roads{out.roads}, walks{out.sidewalks}, marks{out.markings};
    const double seg = std::max(std::min(o.segment, ground.isPlane() ? o.segment : std::max(ground.cellSize(), 50.0)), 10.0);
    std::vector<RoadSection> sections;
    for (const StreetLine& line : net.lines)
        {
        RoadSection sec = roadSection(l, line, !o.forceSurface);
        if (o.forceSurface)
            sec.surface = o.surface;
        sections.push_back(sec);
    }

    // Roads: each band of the cross-section swept between consecutive samples.
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& line = net.lines[k];
        const RoadSection& s = sections[k];
        if (line.pts.size() < 2)
            continue;
        const std::vector<glm::dvec3> pts = resample(line.pts, seg);
        const std::vector<Vec2> side = mitres(pts);
        auto at = [&](size_t i, double off, double up) {
            const Vec2 q = flat(pts[i]) + side[i] * off;
            const double h = line.surface ? pts[i].y : ground.surfaceHeight(q);
            return glm::dvec3(q.x, h + o.lift + up, q.y);
        };
        auto band = [&](MeshBuilder& m, double a, double b, double upA, double upB, RoadPart part, Surface surf) {
            double area = 0.0;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
                area += m.face({at(i, a, upA), at(i + 1, a, upA), at(i + 1, b, upB), at(i, b, upB)}, part, surf, int(k), -1);
            return area;
        };
        // A kerb face at offset off, facing the right-hand side when facing is positive.
        auto wall = [&](double off, double up, double facing) {
            for (size_t i = 0; i + 1 < pts.size(); ++i)
            {
                std::vector<glm::dvec3> q = {at(i, off, 0.0), at(i + 1, off, 0.0), at(i + 1, off, up), at(i, off, up)};
                if (facing < 0.0)
                    std::reverse(q.begin(), q.end());
                walks.face(q, RoadPart::Curb, Surface::Paving, int(k), -1, true);
            }
        };
        if (s.kerbless)
        {
            out.roadArea += band(roads, -s.carriage, 0.0, 0.0, -o.gutter, RoadPart::Carriageway, s.surface);
            out.roadArea += band(roads, 0.0, s.carriage, -o.gutter, 0.0, RoadPart::Carriageway, s.surface);
            continue;
        }
        const double m = 0.5 * s.median;
        if (s.carriage > m)
        {
            out.roadArea += band(roads, -s.carriage, -m, 0.0, 0.0, RoadPart::Carriageway, s.surface);
            out.roadArea += band(roads, m, s.carriage, 0.0, 0.0, RoadPart::Carriageway, s.surface);
        }
        if (m > 0.0)
        {
            out.sidewalkArea += band(walks, -m, m, o.curb, o.curb, RoadPart::Median, Surface::Paving);
            wall(-m, o.curb, -1.0);
            wall(m, o.curb, 1.0);
        }
        const bool raised = s.sidewalk > 0.0;
        const double kerbUp = raised ? o.curb : 0.0;
        for (double sign : {-1.0, 1.0})
        {
            const double in = sign * s.carriage, mid = sign * (s.carriage + s.sidewalk), outer = sign * s.half();
            if (raised)
            {
                out.sidewalkArea += band(walks, in, mid, o.curb, o.curb, RoadPart::Sidewalk, Surface::Paving);
                if (s.carriage > 0.0)
                    wall(in, o.curb, -sign);
            }
            if (s.verge > 0.0)
                out.sidewalkArea += band(walks, mid, outer, kerbUp, kerbUp, RoadPart::Verge, Surface::Earth);
        }
    }

    // Junctions: a patch over the mouth of every road, rounded at the corners, and a curb return where both sides have
    // a sidewalk.
    for (size_t ji = 0; ji < net.junctions.size(); ++ji)
    {
        const Junction& j = net.junctions[ji];
        if (j.ends.size() < 3)
            continue;
        std::vector<int> order(j.ends.size());
        std::iota(order.begin(), order.end(), 0);
        auto dirOf = [&](int e) {
            const Vec2 d = flat(j.ends[size_t(e)].out);
            return glm::length(d) > 1e-9 ? d / glm::length(d) : Vec2(1.0, 0.0);
        };
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            const Vec2 da = dirOf(a), db = dirOf(b);
            return std::atan2(da.y, da.x) < std::atan2(db.y, db.x);
        });
        auto h = [&](Vec2 q, double up) { return glm::dvec3(q.x, ground.surfaceHeight(q) + o.lift + up, q.y); };
        bool anyKerbless = false;
        std::vector<Vec2> ring;
        for (size_t i = 0; i < order.size(); ++i)
        {
            const int a = order[i], b = order[(i + 1) % order.size()];
            const RoadEnd &ea = j.ends[size_t(a)], &eb = j.ends[size_t(b)];
            const RoadSection &sa = sections[size_t(ea.line)], &sb = sections[size_t(eb.line)];
            anyKerbless |= sa.kerbless;
            const Vec2 da = dirOf(a), db = dirOf(b), pa = flat(ea.p), pb = flat(eb.p);
            const double ca = sa.carriage > 0.0 ? sa.carriage : sa.half(), cb = sb.carriage > 0.0 ? sb.carriage : sb.half();
            ring.push_back(pa - rightSide(da) * ca);
            const std::vector<Vec2> kerb = corner(pa + rightSide(da) * ca, da, pb - rightSide(db) * cb, db, 6);
            ring.insert(ring.end(), kerb.begin(), kerb.end() - 1);

            const bool returns = !sa.kerbless && !sb.kerbless && sa.sidewalk > 0.0 && sb.sidewalk > 0.0 && sa.carriage > 0.0 && sb.carriage > 0.0;
            if (!returns)
                continue;
            const std::vector<Vec2> back = corner(pa + rightSide(da) * (sa.carriage + sa.sidewalk), da,
                                                  pb - rightSide(db) * (sb.carriage + sb.sidewalk), db, 6);
            double area = 0.0;
            for (size_t t = 0; t + 1 < kerb.size(); ++t)
            {
                area += walks.face({h(kerb[t], o.curb), h(kerb[t + 1], o.curb), h(back[t + 1], o.curb), h(back[t], o.curb)}, RoadPart::CurbReturn,
                                   Surface::Paving, -1, int(ji));
                walks.face({h(kerb[t], 0.0), h(kerb[t + 1], 0.0), h(kerb[t + 1], o.curb), h(kerb[t], o.curb)}, RoadPart::Curb, Surface::Paving, -1,
                           int(ji), true);
            }
            out.curbReturnArea += area;
            ++out.curbReturns;
        }
        const Vec2 c = flat(j.p);
        const Surface surf = anyKerbless ? (o.forceSurface ? o.surface : Surface::Cobble) : (o.forceSurface ? o.surface : Surface::Asphalt);
        for (size_t t = 0; t < ring.size(); ++t)
            out.junctionArea += roads.face({h(c, 0.0), h(ring[t], 0.0), h(ring[(t + 1) % ring.size()], 0.0)}, RoadPart::Junction, surf, -1, int(ji));

        // Zebra bars along the road over the mouth of each controlled approach, in from the stop line.
        for (const Crosswalk& cw : j.crosswalks)
        {
            const RoadEnd& e = j.ends[size_t(cw.end)];
            const RoadSection& s = sections[size_t(e.line)];
            if (s.kerbless || s.carriage <= 0.0)
                continue;
            const Vec2 d = dirOf(cw.end), n = rightSide(d), p0 = flat(e.p);
            const double depth = 300.0, bar = 50.0, from = 100.0;
            out.crosswalks.push_back({h(p0 + d * (from + 0.5 * depth), 1.0), 2.0 * s.carriage, depth,
                                      glm::degrees(std::atan2(d.x, d.y)), int(ji)});
            if (!o.markings)
                continue;
            for (double x = -s.carriage + 0.5 * bar; x + 0.5 * bar <= s.carriage; x += 2.0 * bar)
            {
                const Vec2 a = p0 + d * from + n * (x - 0.5 * bar), b = a + n * bar;
                out.markingArea += marks.face({h(a, 1.0), h(b, 1.0), h(b + d * depth, 1.0), h(a + d * depth, 1.0)}, RoadPart::Marking,
                                              Surface::Asphalt, e.line, int(ji));
            }
        }
    }
    roads.finish();
    walks.finish();
    marks.finish();
    return out;
}
} // namespace settlement
