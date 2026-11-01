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

/**
 * cornerNormals' topology work done once and reused while only points move: which smooth fan each corner is in, in
 * face-vertex order (valid faces in id order, each from faceCorner). compute() is then a few linear passes.
 */
class LILA_SCENE_API CornerNormalPlan
{
public:
    explicit CornerNormalPlan(const Mesh& mesh, const CornerNormalOptions& opts = {});

    /** True while mesh has the topology and hard edges this plan was made from. */
    bool fits(const Mesh& mesh) const;
    /** Face-vertex rows, numCorners(). */
    size_t rows() const { return mRowPoint.size(); }
    /** Where each valid face's rows start, plus the total. */
    std::span<const uint32_t> faceOffsets() const { return mFaceOffsets; }

    /** cornerNormals(mesh) in row order into out (rows() long). Parallel; one call at a time, as it reuses scratch. */
    void compute(const Mesh& mesh, std::span<glm::vec3> out);

private:
    CornerNormalOptions mOpts;
    uint64_t mTopology = 0;
    std::vector<uint8_t> mHard;
    UninitVector<uint32_t> mFaceOffsets;
    UninitVector<uint32_t> mRowPoint;
    UninitVector<uint32_t> mRowFace; // index into mFaceOffsets
    UninitVector<uint32_t> mRowFan;
    UninitVector<uint32_t> mFanOffsets;
    UninitVector<uint32_t> mFanRows;
    UninitVector<glm::dvec3> mArea;
    UninitVector<double> mRowAngle;
    UninitVector<glm::vec3> mFanNormal;
};

enum class WeightedNormalMode : uint8_t
{
    FaceArea,
    CornerAngle,
    AreaAngle,
};

struct WeightedNormalOptions
{
    WeightedNormalMode mode = WeightedNormalMode::FaceArea;
    /**
     * Within each smooth fan, faces weighing less than this fraction of the fan's heaviest face are left out, so
     * large flat faces keep their own normal across a chamfer or bevel beside them. 0 keeps every face.
     */
    double dominance = 0.5;
    /** Split along "hard" edges; off smooths across them too. */
    bool keepSharp = true;
    /** Only the faces with the highest face int "faceStrength" (0 weak, 1 medium, 2 strong) in a fan count, when present. */
    bool useFaceStrength = true;
};

/**
 * Face-weighted split normals per corner capacity (3ds Max/Blender Weighted Normal): each soft fan takes the normal of
 * its dominant faces, so a low poly with chamfered edges shades its big faces flat. Parallel.
 */
LILA_SCENE_API std::vector<glm::vec3> weightedNormals(const Mesh& mesh, const WeightedNormalOptions& opts = {});

/** Writes weightedNormals() into the corner "normal" attribute, for the given faces' corners only when faces is given. */
LILA_SCENE_API void applyWeightedNormals(Mesh& mesh, const WeightedNormalOptions& opts = {}, std::span<const Index> faces = {});

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
 * computed on the given triangulation (the one used for export). Empty without the uv set. Runs in
 * parallel chunks, each with the triangles around it; the result is the same as one call.
 */
LILA_SCENE_API std::vector<glm::vec4> cornerTangents(const Mesh& mesh, const Triangulation& tris, std::span<const glm::vec3> normals,
                                      std::string_view uvSet = "uv0", bool parallel = true);

/** Triangulates, then writes the corner "tangent" attribute. False without the uv set. */
LILA_SCENE_API bool updateTangents(Mesh& mesh, const TangentOptions& opts = {});

/** Angle at corner c between its two edges, radians. */
LILA_SCENE_API double cornerAngle(const Mesh& mesh, Index c);
} // namespace lila::geo
