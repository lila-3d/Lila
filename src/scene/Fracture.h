#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Deterministic Voronoi fracture: scatter, cut into closed pieces that keep the source's attributes outside and get
 * tagged, box-mapped faces inside, plus the contacts between pieces. Same inputs give byte-identical pieces whatever
 * the thread count.
 */
namespace lila::scene::fracture
{
/** Face int on pieces: 1 on faces made by the cut. */
inline constexpr std::string_view kInsideAttr = "inside";
/** Face int on inside faces: the cell of the piece across the face, -1 when none. */
inline constexpr std::string_view kNeighborAttr = "neighbor";

struct ScatterOptions
{
    int count = 50;
    uint64_t seed = 1;
    /** False scatters inside the volume (the mesh should be closed), true on the surface. */
    bool surface = false;
    /** Points that concentrate the scatter around them. */
    std::vector<glm::dvec3> impacts;
    /** Spread of the points drawn near an impact, cm. */
    double impactRadius = 50.0;
    /** 0..1: the fraction of points drawn near the impacts, with a gaussian falloff. */
    double impactBias = 0.0;
    /** Float point attribute on the mesh weighting where points land; empty is uniform. */
    std::string densityAttr;
};

/** Seeded points in or on the mesh. Fewer than count only when the mesh has no volume (or area) to put them in. */
LILA_SCENE_API std::vector<glm::dvec3> scatterPoints(const geo::Mesh& mesh, const ScatterOptions& o);

/**
 * Glass: the impact plus rings x spokes around it in the plane through it with the given normal, rings packed toward
 * the impact, jittered (0..1 of the spacing) so the shards read as radial and concentric.
 */
LILA_SCENE_API std::vector<glm::dvec3> radialPoints(const glm::dvec3& impact, const glm::dvec3& normal, double radius, int rings,
                                                    int spokes, double jitter, uint64_t seed);

struct FractureOptions
{
    uint64_t seed = 1;
    /**
     * Scales distances per axis when deciding which point is closest, so cells are elongated along axes below 1 (wood
     * grain along x: {0.25, 1, 1}). Components are clamped to at least 0.001.
     */
    glm::dvec3 stretch{1.0};
    /**
     * Roughness of the cutting surfaces, cm; 0 cuts flat. The surfaces are displaced by a smooth seeded noise of
     * position, so neighbouring pieces still fit; the amplitude is capped at a fifth of noiseScale to keep that true.
     */
    double noiseAmplitude = 0.0;
    /** Feature size of the noise, cm. */
    double noiseScale = 10.0;
    /** Edge length the cutting surfaces are subdivided to before displacement, cm; 0 is a third of noiseScale. */
    double detailSize = 0.0;
    /** Material slot of the inside faces; empty is the source's first slot plus "_Interior". */
    std::string interiorMaterial;
    /** Cm per uv unit of the box-projected uvs on the inside faces. */
    double interiorUvScale = 100.0;
    /** Detail is coarsened so a piece's cutting surfaces stay around this many triangles. */
    int maxInteriorFacesPerPiece = 4000;
};

struct Piece
{
    /** In the source mesh's space. */
    geo::Mesh mesh;
    /** Volume centroid. */
    glm::dvec3 centroid{0.0};
    double volume = 0.0;
    /** The scatter point it came from (voronoiFracture), its smallest member's cell (clusterPieces), or its index (refinePieces). */
    int cell = -1;
};

/** Pieces a and b (indices, a < b) touch across inside faces of this total area, with this area-weighted centroid. */
struct Contact
{
    int a = -1;
    int b = -1;
    double area = 0.0;
    glm::dvec3 centroid{0.0};
};

struct FractureResult
{
    std::vector<Piece> pieces;
    /** Sorted by (a, b). */
    std::vector<Contact> contacts;
    std::string error;
    bool ok() const { return error.empty(); }
};

/**
 * Cuts a closed mesh into the Voronoi cells of the points, in cell order; empty cells are dropped. Every piece is a
 * closed manifold. Outside faces keep the source's uvs, colours, normals, hard edges and materials; inside faces get
 * kInsideAttr, kNeighborAttr, the interior slot and box-projected uvs.
 */
LILA_SCENE_API FractureResult voronoiFracture(const geo::Mesh& source, std::span<const glm::dvec3> points, const FractureOptions& o);

/**
 * Merges touching pieces into clusters of about clusterSize pieces each, grown from seeded starting pieces through
 * the contacts. Contacts are rebuilt.
 */
LILA_SCENE_API FractureResult clusterPieces(const FractureResult& in, double clusterSize, uint64_t seed);

/**
 * Fractures again every piece above minVolume (cm^3) into count more cells. Cells are renumbered to piece indices;
 * a face between two refined pieces goes to the neighbour piece under its centroid.
 */
LILA_SCENE_API FractureResult refinePieces(const FractureResult& in, int count, double minVolume, const FractureOptions& o);
} // namespace lila::scene::fracture
