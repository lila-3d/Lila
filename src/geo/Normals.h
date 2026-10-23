#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace lila::geo
{
struct Triangulation;

enum class NormalWeight : uint8_t
{
    Uniform,
    Area,
    Angle,
    AreaAngle,
};

/** Unit face normals per face capacity. Parallel. */
LILA_SCENE_API std::vector<glm::dvec3> faceNormals(const Mesh& mesh);

/** Smooth point normals per point capacity, ignoring hard edges. */
LILA_SCENE_API std::vector<glm::vec3> pointNormals(const Mesh& mesh, NormalWeight weight = NormalWeight::AreaAngle);

struct CornerNormalOptions
{
    NormalWeight weight = NormalWeight::AreaAngle;
    /** Split along edges whose "hard" attribute is set. Boundary and non-manifold edges always split. */
    bool useHardEdges = true;
};

/** Split normals per corner capacity: one smooth fan per run of soft edges around each point. Parallel. */
LILA_SCENE_API std::vector<glm::vec3> cornerNormals(const Mesh& mesh, const CornerNormalOptions& opts = {});

/** Writes cornerNormals() into the corner "normal" attribute. */
LILA_SCENE_API void updateCornerNormals(Mesh& mesh, const CornerNormalOptions& opts = {});

/** Marks edges hard where neighbouring faces meet at more than the angle (Maya's Set Normal Angle). */
LILA_SCENE_API void setHardEdgesByAngle(Mesh& mesh, double degrees, std::span<const Index> edges = {});

/** Clears "hard" on the given edges, or all. */
LILA_SCENE_API void setSoftEdges(Mesh& mesh, std::span<const Index> edges = {});

struct TangentOptions
{
    std::string_view uvSet = "uv0";
    /** Use the corner "normal" attribute if present instead of recomputing. */
    bool useStoredNormals = true;
};

/**
 * MikkTSpace tangents per corner capacity (xyz tangent, w bitangent sign), the same as Unreal's,
 * computed on the given triangulation (the one used for export). Empty without the uv set. Parts of
 * the mesh that share no point run in parallel; the result is the same as one call.
 */
LILA_SCENE_API std::vector<glm::vec4> cornerTangents(const Mesh& mesh, const Triangulation& tris, std::span<const glm::vec3> normals,
                                      std::string_view uvSet = "uv0", bool parallel = true);

/** Triangulates, then writes the corner "tangent" attribute. False without the uv set. */
LILA_SCENE_API bool updateTangents(Mesh& mesh, const TangentOptions& opts = {});

/** Angle at corner c between its two edges, radians. */
LILA_SCENE_API double cornerAngle(const Mesh& mesh, Index c);
} // namespace lila::geo
