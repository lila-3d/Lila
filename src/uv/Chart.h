#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace lila::uv
{
using geo::Index;
using geo::kInvalid;

/**
 * One UV island prepared for solving. Vertices are UV wedges: a mesh point splits into several where
 * seams (or boundaries) separate its corners, so a cylinder cut along one seam has two vertices per seam
 * point. Triangles come from the faces' deterministic triangulation.
 */
struct LILA_SCENE_API Chart
{
    std::vector<Index> faces;              // mesh faces, ascending
    std::vector<Index> corners;            // mesh corners of those faces, face by face
    std::vector<Index> faceStart;          // per face, its first entry in corners; one extra at the end
    std::vector<Index> cornerVertex;       // chart vertex per entry of corners
    std::vector<Index> vertexPoint;        // mesh point per vertex
    std::vector<glm::dvec3> pos;           // 3D position per vertex
    std::vector<glm::dvec2> uv;            // per vertex
    std::vector<uint8_t> pinned;           // per vertex, 1 keeps uv fixed in solves
    std::vector<std::array<Index, 3>> tris; // chart vertices, wound like the faces
    std::vector<Index> triFace;            // mesh face per triangle

    Index vertexCount() const { return Index(pos.size()); }
    Index triangleCount() const { return Index(tris.size()); }

    double area3d() const;
    /** Signed; positive when the island is not mirrored. */
    double areaUv() const;
    /** Ordered boundary loops of chart vertices, longest (by 3D length) first. */
    std::vector<std::vector<Index>> boundaryLoops() const;
    /** Vertices minus edges plus faces of the triangulated chart: 1 for a disk. */
    int euler() const;
    glm::dvec2 uvMin() const;
    glm::dvec2 uvMax() const;
    glm::dvec2 uvCentroid() const;
};

/** How islands are found. */
enum class IslandSource : uint8_t
{
    Seams, // split along edge "seam", mesh boundaries and non-manifold edges
    Uvs,   // split wherever the corners of the uv set disagree across an edge
};

struct ChartOptions
{
    IslandSource source = IslandSource::Seams;
    std::string_view uvSet = "uv0";
    /** Extra edges to treat as seams, per edge capacity (optional). */
    std::span<const uint8_t> extraSeams;
    /** With IslandSource::Seams, whether the mesh's own "seam" edges split. */
    bool useSeamAttr = true;
    /** Corner bool attribute marking pinned uvs; any pinned corner pins its wedge. */
    std::string_view pinAttr = "uvPin";
};

/**
 * Islands of the given faces (all live faces when empty). Charts and their vertices come out in a
 * stable order, so the same mesh always gives the same charts. Current uvs are loaded when the set
 * exists.
 */
LILA_SCENE_API std::vector<Chart> buildCharts(const geo::Mesh& mesh, std::span<const Index> faces = {}, const ChartOptions& opts = {});

/** Face id lists of the islands, without building chart geometry. */
LILA_SCENE_API std::vector<std::vector<Index>> islandFaces(const geo::Mesh& mesh, std::span<const Index> faces = {}, const ChartOptions& opts = {});

/** Writes each chart's vertex uvs into its corners, adding the uv set if needed. */
LILA_SCENE_API void writeCharts(geo::Mesh& mesh, std::span<const Chart> charts, std::string_view uvSet = "uv0");

/** Corner uvs of a set, added (zeroed) when missing. */
LILA_SCENE_API std::span<glm::vec2> uvLayer(geo::Mesh& mesh, std::string_view uvSet);

/** Live faces as a list, or the given ones filtered to live faces. */
LILA_SCENE_API std::vector<Index> liveFaces(const geo::Mesh& mesh, std::span<const Index> faces = {});

/**
 * Sets "seam" on the edges of the faces wherever the uv set splits (or the face set ends) and clears it
 * where both sides agree. Edges away from the faces are left alone. Returns the seam count among them.
 */
LILA_SCENE_API Index markSeamsFromUvs(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet = "uv0");

/** Unsigned angle between the two faces of a manifold edge, degrees; 0 when flat, 180 when folded. */
LILA_SCENE_API double dihedralAngle(const geo::Mesh& mesh, Index e);
} // namespace lila::uv
