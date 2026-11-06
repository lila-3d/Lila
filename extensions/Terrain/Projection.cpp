// Map projections for real-world terrain: transverse Mercator (UTM), ECEF and local east-north-up, and the georeference
// that places latitude and longitude on a terrain's heightfield.

#include "Projection.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

namespace terrain
{
namespace projdetail
{
constexpr double kProjectionPi = 3.14159265358979323846;
constexpr double kDeg = kProjectionPi / 180.0;
constexpr double kA = 6378137.0;
constexpr double kF = 1.0 / 298.257223563;
constexpr double kE2 = kF * (2.0 - kF);

/** Kruger series coefficients for WGS84. */
struct Series
{
    double n = 0.0, A = 0.0, e = 0.0;
    double alpha[7] = {};
    double beta[7] = {};

    Series()
    {
        n = kF / (2.0 - kF);
        const double n2 = n * n, n3 = n2 * n, n4 = n3 * n, n5 = n4 * n, n6 = n5 * n;
        A = kA / (1.0 + n) * (1.0 + n2 / 4.0 + n4 / 64.0 + n6 / 256.0);
        e = std::sqrt(kE2);
        alpha[1] = n / 2 - 2 * n2 / 3 + 5 * n3 / 16 + 41 * n4 / 180 - 127 * n5 / 288 + 7891 * n6 / 37800;
        alpha[2] = 13 * n2 / 48 - 3 * n3 / 5 + 557 * n4 / 1440 + 281 * n5 / 630 - 1983433 * n6 / 1935360;
        alpha[3] = 61 * n3 / 240 - 103 * n4 / 140 + 15061 * n5 / 26880 + 167603 * n6 / 181440;
        alpha[4] = 49561 * n4 / 161280 - 179 * n5 / 168 + 6601661 * n6 / 7257600;
        alpha[5] = 34729 * n5 / 80640 - 3418889 * n6 / 1995840;
        alpha[6] = 212378941 * n6 / 319334400;
        beta[1] = n / 2 - 2 * n2 / 3 + 37 * n3 / 96 - n4 / 360 - 81 * n5 / 512 + 96199 * n6 / 604800;
        beta[2] = n2 / 48 + n3 / 15 - 437 * n4 / 1440 + 46 * n5 / 105 - 1118711 * n6 / 3870720;
        beta[3] = 17 * n3 / 480 - 37 * n4 / 840 - 209 * n5 / 4480 + 5569 * n6 / 90720;
        beta[4] = 4397 * n4 / 161280 - 11 * n5 / 504 - 830251 * n6 / 7257600;
        beta[5] = 4583 * n5 / 161280 - 108847 * n6 / 3991680;
        beta[6] = 20648693 * n6 / 638668800;
    }
};

const Series& series()
{
    static const Series s;
    return s;
}

double metresPerDegLat(double lat)
{
    const double p = lat * kDeg;
    return 111132.92 - 559.82 * std::cos(2 * p) + 1.175 * std::cos(4 * p);
}

double metresPerDegLon(double lat)
{
    const double p = lat * kDeg;
    return 111412.84 * std::cos(p) - 93.5 * std::cos(3 * p);
}

double wrapLon(double lon)
{
    while (lon > 180.0)
        lon -= 360.0;
    while (lon < -180.0)
        lon += 360.0;
    return lon;
}

/** The east, north and up unit vectors at a geodetic point. */
void enuAxes(LatLon o, glm::dvec3& east, glm::dvec3& north, glm::dvec3& up)
{
    const double sl = std::sin(o.lat * kDeg), cl = std::cos(o.lat * kDeg), so = std::sin(o.lon * kDeg), co = std::cos(o.lon * kDeg);
    east = {-so, co, 0.0};
    north = {-sl * co, -sl * so, cl};
    up = {cl * co, cl * so, sl};
}
} // namespace projdetail

using namespace projdetail;

glm::dvec2 transverseMercator(LatLon p, double lon0, double k0, double falseEasting, double falseNorthing)
{
    const Series& s = series();
    const double phi = p.lat * kDeg, lam = wrapLon(p.lon - lon0) * kDeg;
    const double sp = std::sin(phi);
    const double t = std::sinh(std::atanh(sp) - s.e * std::atanh(s.e * sp));
    const double xiP = std::atan2(t, std::cos(lam));
    const double etaP = std::atanh(std::sin(lam) / std::sqrt(1.0 + t * t));
    double xi = xiP, eta = etaP;
    for (int j = 1; j <= 6; ++j)
    {
        xi += s.alpha[j] * std::sin(2 * j * xiP) * std::cosh(2 * j * etaP);
        eta += s.alpha[j] * std::cos(2 * j * xiP) * std::sinh(2 * j * etaP);
    }
    return {falseEasting + k0 * s.A * eta, falseNorthing + k0 * s.A * xi};
}

LatLon inverseTransverseMercator(glm::dvec2 en, double lon0, double k0, double falseEasting, double falseNorthing)
{
    const Series& s = series();
    const double xi = (en.y - falseNorthing) / (k0 * s.A), eta = (en.x - falseEasting) / (k0 * s.A);
    double xiP = xi, etaP = eta;
    for (int j = 1; j <= 6; ++j)
    {
        xiP -= s.beta[j] * std::sin(2 * j * xi) * std::cosh(2 * j * eta);
        etaP -= s.beta[j] * std::cos(2 * j * xi) * std::sinh(2 * j * eta);
    }
    const double chi = std::asin(std::clamp(std::sin(xiP) / std::cosh(etaP), -1.0, 1.0));
    const double lam = std::atan2(std::sinh(etaP), std::cos(xiP));
    // Geodetic from conformal latitude by fixed-point iteration; converges to 1e-15 in under ten steps.
    double phi = chi;
    for (int it = 0; it < 30; ++it)
    {
        const double es = s.e * std::sin(phi);
        const double next = 2.0 * std::atan(std::tan(kProjectionPi / 4 + chi / 2) * std::pow((1 + es) / (1 - es), s.e / 2)) - kProjectionPi / 2;
        if (std::abs(next - phi) < 1e-15)
        {
            phi = next;
            break;
        }
        phi = next;
    }
    return {phi / kDeg, wrapLon(lon0 + lam / kDeg)};
}

int utmZone(LatLon p)
{
    const double lon = wrapLon(p.lon);
    int zone = int(std::floor((lon + 180.0) / 6.0)) + 1;
    zone = std::clamp(zone, 1, 60);
    if (p.lat >= 56.0 && p.lat < 64.0 && lon >= 3.0 && lon < 12.0)
        zone = 32;
    if (p.lat >= 72.0 && p.lat < 84.0 && lon >= 0.0 && lon < 42.0)
        zone = lon < 9.0 ? 31 : lon < 21.0 ? 33 : lon < 33.0 ? 35 : 37;
    return zone;
}

double utmCentralMeridian(int zone) { return double(zone - 1) * 6.0 - 180.0 + 3.0; }

glm::dvec2 latLonToUtm(LatLon p, int zone, bool south)
{
    return transverseMercator(p, utmCentralMeridian(zone), 0.9996, 500000.0, south ? 10000000.0 : 0.0);
}

LatLon utmToLatLon(glm::dvec2 en, int zone, bool south)
{
    return inverseTransverseMercator(en, utmCentralMeridian(zone), 0.9996, 500000.0, south ? 10000000.0 : 0.0);
}

glm::dvec3 geodeticToEcef(LatLon p, double height)
{
    const double sl = std::sin(p.lat * kDeg), cl = std::cos(p.lat * kDeg);
    const double n = kA / std::sqrt(1.0 - kE2 * sl * sl);
    return {(n + height) * cl * std::cos(p.lon * kDeg), (n + height) * cl * std::sin(p.lon * kDeg), (n * (1.0 - kE2) + height) * sl};
}

LatLon ecefToGeodetic(const glm::dvec3& e, double* height)
{
    const double p = std::sqrt(e.x * e.x + e.y * e.y);
    const double lon = std::atan2(e.y, e.x);
    double lat = std::atan2(e.z, p * (1.0 - kE2)), h = 0.0;
    for (int it = 0; it < 10; ++it)
    {
        const double sl = std::sin(lat);
        const double n = kA / std::sqrt(1.0 - kE2 * sl * sl);
        h = p > 1e-9 ? p / std::cos(lat) - n : std::abs(e.z) - n * (1.0 - kE2);
        lat = std::atan2(e.z, p * (1.0 - kE2 * n / (n + h)));
    }
    if (height)
        *height = h;
    return {lat / kDeg, lon / kDeg};
}

glm::dvec3 geodeticToEnu(LatLon p, double height, LatLon origin)
{
    glm::dvec3 east, north, up;
    enuAxes(origin, east, north, up);
    const glm::dvec3 d = geodeticToEcef(p, height) - geodeticToEcef(origin, 0.0);
    return {glm::dot(d, east), glm::dot(d, north), glm::dot(d, up)};
}

LatLon enuToGeodetic(glm::dvec2 en, LatLon origin)
{
    glm::dvec3 east, north, up;
    enuAxes(origin, east, north, up);
    const glm::dvec3 base = geodeticToEcef(origin, 0.0);
    // The ellipsoid point whose tangent-plane east and north are en: solve for the drop below the plane.
    double drop = 0.0;
    LatLon g = origin;
    for (int it = 0; it < 8; ++it)
    {
        g = ecefToGeodetic(base + east * en.x + north * en.y + up * drop);
        const glm::dvec3 d = geodeticToEcef(g, 0.0) - base;
        drop = glm::dot(d, up);
        const glm::dvec2 got{glm::dot(d, east), glm::dot(d, north)};
        if (glm::length(got - en) < 1e-6)
            break;
    }
    return g;
}

const std::vector<std::string>& projectionNames()
{
    static const std::vector<std::string> n = {"local", "utm", "enu"};
    return n;
}

bool parseProjection(std::string_view name, ProjectionKind* out)
{
    const auto& n = projectionNames();
    auto it = std::find(n.begin(), n.end(), name);
    if (it == n.end())
        return false;
    *out = ProjectionKind(it - n.begin());
    return true;
}

int utmZoneOfEpsg(int epsg, bool* south)
{
    if (epsg > 32600 && epsg <= 32660)
    {
        *south = false;
        return epsg - 32600;
    }
    if (epsg > 32700 && epsg <= 32760)
    {
        *south = true;
        return epsg - 32700;
    }
    return 0;
}

glm::dvec2 Georef::project(LatLon p) const
{
    switch (kind)
    {
    case ProjectionKind::Utm: return latLonToUtm(p, zone, south);
    case ProjectionKind::Enu: return glm::dvec2(geodeticToEnu(p, 0.0, enuOrigin));
    default: return {p.lon, p.lat};
    }
}

LatLon Georef::unproject(glm::dvec2 xy) const
{
    switch (kind)
    {
    case ProjectionKind::Utm: return utmToLatLon(xy, zone, south);
    case ProjectionKind::Enu: return enuToGeodetic(xy, enuOrigin);
    default: return {xy.y, xy.x};
    }
}

glm::dvec2 Georef::toLocal(LatLon p) const
{
    const glm::dvec2 xy = project(p);
    return origin + glm::dvec2((xy.x - x0) * mx, (y0 - xy.y) * my) * 100.0;
}

LatLon Georef::toGeographic(glm::dvec2 local) const
{
    const glm::dvec2 d = (local - origin) * 0.01;
    return unproject({x0 + d.x / mx, y0 - d.y / my});
}

nlohmann::json Georef::toJson() const
{
    nlohmann::json j = {{"projection", projectionNames()[size_t(kind)]}, {"x0", x0}, {"y0", y0}, {"mx", mx}, {"my", my},
                        {"origin", {origin.x, origin.y}}};
    if (kind == ProjectionKind::Local)
        j["latMid"] = latMid;
    if (kind == ProjectionKind::Utm)
    {
        j["zone"] = zone;
        j["south"] = south;
        j["epsg"] = (south ? 32700 : 32600) + zone;
    }
    if (kind == ProjectionKind::Enu)
        j["enuOrigin"] = {enuOrigin.lat, enuOrigin.lon};
    if (foreignEpsg)
        j["foreignEpsg"] = foreignEpsg;
    return j;
}

Georef Georef::fromJson(const nlohmann::json& j)
{
    Georef g;
    if (!parseProjection(j.value("projection", std::string("local")), &g.kind))
        throw std::runtime_error("unknown projection " + j.value("projection", std::string()));
    g.x0 = j.value("x0", 0.0);
    g.y0 = j.value("y0", 0.0);
    g.mx = j.value("mx", 1.0);
    g.my = j.value("my", 1.0);
    if (j.contains("origin") && j["origin"].is_array() && j["origin"].size() == 2)
        g.origin = {j["origin"][0].get<double>(), j["origin"][1].get<double>()};
    g.latMid = j.value("latMid", 0.0);
    g.zone = j.value("zone", 0);
    g.south = j.value("south", false);
    if (j.contains("enuOrigin") && j["enuOrigin"].is_array() && j["enuOrigin"].size() == 2)
        g.enuOrigin = {j["enuOrigin"][0].get<double>(), j["enuOrigin"][1].get<double>()};
    g.foreignEpsg = j.value("foreignEpsg", 0);
    return g;
}

Georef georefOf(const Dem& d)
{
    Georef g;
    g.x0 = d.x0;
    g.y0 = d.y0;
    if (d.geographic)
    {
        g.kind = ProjectionKind::Local;
        g.latMid = d.y0 - d.dy * double(d.height - 1) * 0.5;
        g.mx = metresPerDegLon(g.latMid);
        g.my = metresPerDegLat(g.latMid);
        return g;
    }
    g.mx = g.my = 1.0;
    if (int zone = utmZoneOfEpsg(d.epsg, &g.south))
    {
        g.kind = ProjectionKind::Utm;
        g.zone = zone;
    }
    else
        g.foreignEpsg = d.epsg ? d.epsg : -1;
    return g;
}

Dem reprojectDem(const Dem& d, ProjectionKind kind, double spacing, int zone, Georef* georef)
{
    LILA_ZONE();

    if (!d.geographic)
        throw std::runtime_error("the data is projected already (reprojection takes data in degrees)");
    if (!(d.dx > 0.0 && d.dy > 0.0))
        throw std::runtime_error("the data has no georeference");
    const double west = d.x0, north = d.y0, east = d.x0 + d.dx * double(d.width - 1), southLat = d.y0 - d.dy * double(d.height - 1);
    const LatLon centre{(north + southLat) * 0.5, (west + east) * 0.5};
    Georef g;
    g.kind = kind;
    if (kind == ProjectionKind::Utm)
    {
        g.zone = zone > 0 ? std::clamp(zone, 1, 60) : utmZone(centre);
        g.south = centre.lat < 0.0;
    }
    else if (kind == ProjectionKind::Enu)
        g.enuOrigin = centre;
    else
        throw std::runtime_error("reprojection is to utm or enu");

    // The projected box covering every edge sample of the data.
    glm::dvec2 lo(std::numeric_limits<double>::infinity()), hi(-std::numeric_limits<double>::infinity());
    auto grow = [&](double lon, double lat) {
        const glm::dvec2 p = g.project({lat, lon});
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    };
    for (int i = 0; i <= 64; ++i)
    {
        const double t = double(i) / 64.0;
        grow(west + (east - west) * t, north);
        grow(west + (east - west) * t, southLat);
        grow(west, north + (southLat - north) * t);
        grow(east, north + (southLat - north) * t);
    }
    if (spacing <= 0.0)
        spacing = std::min(d.dx * metresPerDegLon(centre.lat), d.dy * metresPerDegLat(centre.lat));
    const double w = (hi.x - lo.x) / spacing, h = (hi.y - lo.y) / spacing;
    if (!(w >= 1.0 && h >= 1.0) || w * h > double(size_t(1) << 28))
        throw std::runtime_error("the reprojected grid would be " + std::to_string(int64_t(w)) + " x " + std::to_string(int64_t(h)) + " samples");

    Dem out;
    out.width = int(std::floor(w)) + 1;
    out.height = int(std::floor(h)) + 1;
    out.geographic = false;
    out.dx = out.dy = spacing;
    // Centred in the projected box, so the edges lose the same sliver on each side.
    out.x0 = (lo.x + hi.x) * 0.5 - spacing * double(out.width - 1) * 0.5;
    out.y0 = (lo.y + hi.y) * 0.5 + spacing * double(out.height - 1) * 0.5;
    out.epsg = kind == ProjectionKind::Utm ? (g.south ? 32700 : 32600) + g.zone : 0;
    out.metres.assign(size_t(out.width) * size_t(out.height), std::numeric_limits<float>::quiet_NaN());
    const bool filled = !d.filled.empty();
    if (filled)
        out.filled.assign(out.metres.size(), 0);
    tbb::parallel_for(0, out.height, [&](int r) {
        for (int c = 0; c < out.width; ++c)
        {
            const LatLon p = g.unproject({out.x0 + spacing * c, out.y0 - spacing * r});
            const double fx = (p.lon - west) / d.dx, fy = (north - p.lat) / d.dy;
            if (fx < -0.5 || fy < -0.5 || fx > double(d.width) - 0.5 || fy > double(d.height) - 0.5)
                continue;
            const double cx = std::clamp(fx, 0.0, double(d.width - 1)), cy = std::clamp(fy, 0.0, double(d.height - 1));
            const int x0 = std::min(int(cx), d.width - 2), y0 = std::min(int(cy), d.height - 2);
            const double tx = cx - x0, ty = cy - y0;
            auto at = [&](int i, int j) { return double(d.metres[size_t(j) * size_t(d.width) + size_t(i)]); };
            double v = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
            const size_t k = size_t(r) * size_t(out.width) + size_t(c);
            if (kind == ProjectionKind::Enu && std::isfinite(v))
                v = geodeticToEnu(p, v, g.enuOrigin).z;
            out.metres[k] = float(v);
            if (filled)
            {
                auto fat = [&](int i, int j) { return double(d.filled[size_t(j) * size_t(d.width) + size_t(i)]); };
                out.filled[k] = uint8_t((fat(x0, y0) + fat(x0 + 1, y0) + fat(x0, y0 + 1) + fat(x0 + 1, y0 + 1)) > 0.0 ? 1 : 0);
            }
        }
    });
    g.x0 = out.x0;
    g.y0 = out.y0;
    g.mx = g.my = 1.0;
    if (georef)
        *georef = g;
    return out;
}

Heightfield demToTerrain(Dem d, const DemOptions& o, ProjectionKind kind, int zone, bool fill, Georef* georef)
{
    LILA_ZONE();

    Georef g;
    if (kind != ProjectionKind::Local && d.geographic)
        d = reprojectDem(d, kind, o.cell > 0.0 ? o.cell * 0.01 : 0.0, zone, &g);
    else
    {
        g = georefOf(d);
        if (kind == ProjectionKind::Enu && !d.geographic)
            throw std::runtime_error("the data is projected already; enu takes data in degrees (use local)");
        if (kind == ProjectionKind::Utm && !d.geographic && (g.kind != ProjectionKind::Utm || (zone > 0 && zone != g.zone)))
            throw std::runtime_error(g.kind == ProjectionKind::Utm ? "the data is in UTM zone " + std::to_string(g.zone) + " already"
                                                                   : "the data is projected (EPSG " + std::to_string(d.epsg) + "), not in degrees; use local");
    }
    if (fill)
        fillVoids(d);
    Heightfield h = demToHeightfield(d, o);
    g.origin = h.origin;
    if (georef)
        *georef = g;
    return h;
}

namespace projdetail
{
std::mutex& georefLock()
{
    static std::mutex m;
    return m;
}

std::map<uint64_t, Georef>& georefs()
{
    static std::map<uint64_t, Georef> m;
    return m;
}
} // namespace projdetail

void publishGeoref(uint64_t owner, const Georef& g)
{
    if (!owner)
        return;
    std::lock_guard lock(georefLock());
    georefs()[owner] = g;
}

bool publishedGeoref(uint64_t owner, Georef* out)
{
    std::lock_guard lock(georefLock());
    auto it = georefs().find(owner);
    if (it == georefs().end())
        return false;
    *out = it->second;
    return true;
}
} // namespace terrain
