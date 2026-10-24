#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "geo/Bvh.h"
#include "geo/Symmetry.h"
#include "scene/Node.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Bake cages and the point attributes that shape them.
 *
 * The auto cage pushes each low point along its averaged smooth normal by frontal x the group's cageOffset x the point's
 * "bakeOffset" (1 where unpainted); "bakeSkew" (0 where unpainted, added to the group's skew) tilts rays toward each low
 * face's geometric normal. An editable cage is a mesh node with the low's topology: bakeCage(create=True) makes one from
 * the auto cage, positions in world space at the scene root, and the group uses it until reset to auto.
 *
 * PointPaint is the brush the bakePaint viewport tool, the UV editor and the bakePaint command share: a float point
 * attribute painted with add, subtract, smooth and set dabs, flood and mirror symmetry, on the low's base mesh. Dabs land
 * on the surface (walked along edges from a hit) or in UV space (the corners under the brush in the low's uv0, folded to
 * their points, so a point split across islands takes one value however many of its corners the dab reaches).
 */
namespace lila::bake
{
inline constexpr const char* kOffsetAttr = "bakeOffset";
inline constexpr const char* kSkewAttr = "bakeSkew";

/** What a painted attribute is called and its range: offset (bakeOffset, 1 unpainted, 0-10) or skew (bakeSkew, 0, -1-1). */
struct PaintAttr
{
    const char* name = kOffsetAttr;
    float unpainted = 1.0f;
    float min = 0.0f;
    float max = 10.0f;
};
/** "offset" / "bakeOffset" or "skew" / "bakeSkew"; nullopt for anything else. */
LILA_SCENE_API std::optional<PaintAttr> paintAttr(std::string_view name);

/** The attribute's values per point (capacity sized), the unpainted value where the mesh lacks it. */
LILA_SCENE_API std::vector<float> pointValues(const geo::Mesh& mesh, const PaintAttr& attr);

/**
 * Writes values into the mesh node's base mesh (its edit plug) as the attribute, one undo step. Values are per point of
 * that base mesh. Returns false when the node is not a mesh or the sizes differ.
 */
LILA_SCENE_API bool setPointValues(scene::Graph& g, scene::NodeId mesh, const PaintAttr& attr, std::span<const float> values, const std::string& label);

enum class PaintOp : uint8_t
{
    Add,
    Subtract,
    Smooth,
    Set,
};
LILA_SCENE_API std::optional<PaintOp> paintOpFromName(std::string_view name);
LILA_SCENE_API const char* paintOpName(PaintOp op);

/** How a dab's weight falls from its centre to its rim: smooth (1 - t^2)^2, linear, or constant. */
enum class PaintFalloff : uint8_t
{
    Smooth,
    Linear,
    Constant,
};
LILA_SCENE_API std::optional<PaintFalloff> paintFalloffFromName(std::string_view name);
LILA_SCENE_API const char* paintFalloffName(PaintFalloff f);
/** t is distance / radius; 0 at and past the rim. */
LILA_SCENE_API float paintFalloff(PaintFalloff f, double t);

/** A low's uv0 layout for dabs in UV space: corners and triangles bucketed on a grid. */
class LILA_SCENE_API UvLayout
{
public:
    explicit UvLayout(const geo::Mesh& mesh);

    bool empty() const { return mUv.empty(); }
    /** Per corner (capacity sized); zero for corners without one. */
    std::span<const glm::dvec2> uvs() const { return mUv; }
    const geo::Triangulation& triangles() const { return mTris; }
    /** The triangle under a UV position and its barycentric weights. */
    bool triangleAt(const glm::dvec2& uv, geo::Index& triangle, glm::dvec3& bary) const;
    /**
     * Corners within radius of centre (UV units) with their falloff, plus every corner of the face under the centre (so a
     * dab inside a face larger than the brush still reaches it), each corner once, ascending.
     */
    std::vector<std::pair<geo::Index, float>> corners(const glm::dvec2& center, double radius, PaintFalloff falloff) const;

private:
    int cellOf(const glm::dvec2& uv, int& cx, int& cy) const;

    std::vector<glm::dvec2> mUv;
    geo::Triangulation mTris;
    glm::dvec2 mMin{0.0};
    glm::dvec2 mCell{1.0};
    int mCols = 1;
    int mRows = 1;
    std::vector<std::vector<geo::Index>> mCornerCells;
    std::vector<std::vector<geo::Index>> mTriCells;
};

/** A working copy of one painted attribute on a mesh, with what brush queries need. Object space. */
class LILA_SCENE_API PointPaint
{
public:
    PointPaint(std::shared_ptr<const geo::Mesh> mesh, const PaintAttr& attr);

    const geo::Mesh& mesh() const { return *mMesh; }
    const PaintAttr& attr() const { return mAttr; }
    std::span<const float> values() const { return mValues; }
    std::span<float> values() { return mValues; }
    const geo::MeshBvh& bvh() const { return mBvh; }

    /** Mirror dabs across an axis plane (0 x, 1 y, 2 z); -1 turns it off. */
    void setSymmetry(int axis);
    int symmetryAxis() const { return mAxis; }
    void setFalloff(PaintFalloff f) { mFalloff = f; }
    PaintFalloff falloff() const { return mFalloff; }

    bool raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const;

    /** Points within radius of the hit, walked along edges (so it never jumps a gap), with a smooth falloff weight. */
    std::vector<std::pair<geo::Index, float>> brushPoints(const geo::BvhHit& hit, double radius) const;
    /** Points within radius of center (straight distance), with the same falloff. */
    std::vector<std::pair<geo::Index, float>> pointsNear(const glm::dvec3& center, double radius) const;
    /**
     * A dab in UV space (uv0, UV units): the corners under it folded onto their points, each point at its largest corner
     * weight. Empty when the mesh has no uv0.
     */
    std::vector<std::pair<geo::Index, float>> uvPoints(const glm::dvec2& center, double radius) const;
    /** The uv0 layout, built on first use. */
    const UvLayout& uvLayout() const;

    /** Applies op at each point by weight x strength, and at the mirror points (not already in points) when symmetry is on. */
    void apply(std::span<const std::pair<geo::Index, float>> points, PaintOp op, float value, float strength);
    /** op over every valid point, or over mask's points when it is not empty. */
    void flood(PaintOp op, float value, float strength, std::span<const geo::Index> mask = {});

private:
    void applyOnce(std::span<const std::pair<geo::Index, float>> points, PaintOp op, float value, float strength);

    std::shared_ptr<const geo::Mesh> mMesh;
    PaintAttr mAttr;
    std::vector<float> mValues;
    geo::MeshBvh mBvh;
    int mAxis = -1;
    PaintFalloff mFalloff = PaintFalloff::Smooth;
    geo::SymmetryMap mSymmetry;
    mutable std::unique_ptr<UvLayout> mUv;
};

/** The auto cage of a mesh in world space: every point pushed by frontal x offset x its bakeOffset along its smooth normal. */
LILA_SCENE_API std::vector<glm::dvec3> autoCagePoints(const geo::Mesh& mesh, const glm::dmat4& world, double frontal, double offset);
/** The same with per point offsets given (a stroke in progress) instead of the mesh's bakeOffset. */
LILA_SCENE_API std::vector<glm::dvec3> autoCagePoints(const geo::Mesh& mesh, const glm::dmat4& world, double frontal, double offset,
                                                     std::span<const float> pointOffsets);

/** The resolved frontal distance of a group: its setting, else 3% of its lows' world bounds diagonal (as bakes use). */
LILA_SCENE_API double groupFrontal(scene::Graph& g, const GroupSettings& s);

/** Why cage can't stand in for low ("" when it can): point count, face count or a face's corners differing. */
LILA_SCENE_API std::string cageMismatch(const geo::Mesh& low, const geo::Mesh& cage);

/** The group's only low, or a std::runtime_error naming the problem. */
LILA_SCENE_API scene::NodeId cageLow(scene::Graph& g, const scene::Node& group);

/**
 * Creates an editable cage for a one-low group from its auto cage (paint included), names it in the group and returns
 * it. Undoable. An existing cage is replaced in the group but left in the scene. Throws std::runtime_error.
 */
LILA_SCENE_API scene::Node* createCage(scene::Graph& g, scene::NodeId group, const std::string& name = {});

/** Moves the group's cage points to the auto cage as painted now ("bake paint into cage"). Undoable. Throws. */
LILA_SCENE_API void cageFromPaint(scene::Graph& g, scene::NodeId group);

/** Back to the auto cage; deleteNode also removes the cage mesh. Undoable. Returns the cage that was used, or empty. */
LILA_SCENE_API std::string resetCage(scene::Graph& g, scene::NodeId group, bool deleteNode);
} // namespace lila::bake
