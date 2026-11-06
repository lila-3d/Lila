#pragma once

#include "Inputs.h"

#include <glm/glm.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace terrain
{
/** Geodetic latitude and longitude in degrees, height in metres above the WGS84 ellipsoid. */
struct LatLon
{
    double lat = 0.0;
    double lon = 0.0;
};

/** Transverse Mercator on WGS84 (Kruger series to n^6, Karney 2011): sub-millimetre within a few thousand km of the meridian. */
glm::dvec2 transverseMercator(LatLon p, double lon0, double k0, double falseEasting, double falseNorthing);
LatLon inverseTransverseMercator(glm::dvec2 en, double lon0, double k0, double falseEasting, double falseNorthing);

/** The UTM zone (1 to 60) for a point, with the Norway and Svalbard exceptions. */
int utmZone(LatLon p);
double utmCentralMeridian(int zone);
/** Easting and northing in metres in the zone; south uses the 10,000 km false northing. */
glm::dvec2 latLonToUtm(LatLon p, int zone, bool south);
LatLon utmToLatLon(glm::dvec2 en, int zone, bool south);

glm::dvec3 geodeticToEcef(LatLon p, double height);
LatLon ecefToGeodetic(const glm::dvec3& ecef, double* height = nullptr);
/** East, north, up of a point from a tangent plane at origin (height 0 on the ellipsoid there). */
glm::dvec3 geodeticToEnu(LatLon p, double height, LatLon origin);
/** The point on the ellipsoid (height 0) under east, north on the tangent plane at origin. */
LatLon enuToGeodetic(glm::dvec2 en, LatLon origin);

/** The map projection a terrain's horizontal grid is in. */
enum class ProjectionKind : uint8_t
{
    /** Degrees flattened around the data's middle latitude (fine up to about 100 km). */
    Local,
    /** UTM metres in one zone: conformal, the usual choice for real places. */
    Utm,
    /** A tangent plane at the data's centre: east, north, and heights including the earth's curvature. */
    Enu,
};

const std::vector<std::string>& projectionNames();
bool parseProjection(std::string_view name, ProjectionKind* out);

/**
 * Where a terrain is on the earth: how a latitude and longitude land on its heightfield (cm, +X east, -Z north) and
 * back. Data coordinates (degrees for Local, projected metres otherwise) map linearly onto the grid from origin.
 */
struct Georef
{
    ProjectionKind kind = ProjectionKind::Local;
    /** Local: the middle latitude whose metres per degree flatten the data. */
    double latMid = 0.0;
    int zone = 0;
    bool south = false;
    /** Enu: the tangent point. */
    LatLon enuOrigin;
    /** Data coordinates of the heightfield's first sample (west, north). */
    double x0 = 0.0;
    double y0 = 0.0;
    /** Metres per data unit east and north. */
    double mx = 1.0;
    double my = 1.0;
    /** The heightfield's origin, cm. */
    glm::dvec2 origin{0.0};
    /** EPSG code of projected data Lila can't convert (0 when it can). */
    int foreignEpsg = 0;

    /** Data coordinates (lon, lat for Local; easting, northing otherwise) of a geographic point. */
    glm::dvec2 project(LatLon p) const;
    LatLon unproject(glm::dvec2 xy) const;
    /** Heightfield XZ (cm) of a geographic point. */
    glm::dvec2 toLocal(LatLon p) const;
    LatLon toGeographic(glm::dvec2 local) const;
    /** Whether lat/lon can be placed on it. */
    bool geographicOk() const { return foreignEpsg == 0; }

    nlohmann::json toJson() const;
    static Georef fromJson(const nlohmann::json& j);
};

/**
 * Elevation data in geographic degrees resampled onto a UTM or ENU grid in metres. spacing is the new grid's, metres; 0
 * keeps the data's own at its middle. zone 0 picks the data centre's zone. Heights carry over (ENU adds the earth's
 * curvature drop). Voids stay voids. Throws std::runtime_error.
 */
Dem reprojectDem(const Dem& d, ProjectionKind kind, double spacing, int zone, Georef* georef);

/**
 * How a Dem as read (degrees, or UTM metres by its EPSG code) lands on a heightfield demToHeightfield makes from it;
 * set origin to that heightfield's.
 */
Georef georefOf(const Dem& d);

/**
 * Elevation data on a heightfield: data in degrees reprojected when kind is utm or enu (UTM data stays in its zone),
 * voids filled when fill is set, and the georeference of the result. Throws std::runtime_error.
 */
Heightfield demToTerrain(Dem d, const DemOptions& o, ProjectionKind kind, int zone, bool fill, Georef* georef);

/** The georeference of the terrain a modifier (scene node id) made on its last evaluation, from Read Elevation Data. */
void publishGeoref(uint64_t owner, const Georef& g);
bool publishedGeoref(uint64_t owner, Georef* out);

/** The EPSG code's UTM zone (WGS84 326zz north, 327zz south); 0 when it isn't one. */
int utmZoneOfEpsg(int epsg, bool* south);
} // namespace terrain
