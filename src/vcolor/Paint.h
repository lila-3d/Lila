#pragma once

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"
#include "scene/SoftSelect.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Vertex color painting and editing on one color set of a mesh, independent of the UI: blend math, brush strokes with
 * symmetry and masking, fills, gradients, adjustments and bakes. Everything is in the mesh's object space and runs in
 * parallel.
 */
namespace lila::vcolor
{
using geo::Index;

enum class Blend : uint8_t
{
    Replace,
    Add,
    Subtract,
    Multiply,
    Lighten,
    Darken,
    Blur,
    Sharpen,
    Count
};

std::string_view blendName(Blend b);
std::optional<Blend> blendFromName(std::string_view name);
/** Blur and Sharpen work from the neighbourhood instead of a brush color. */
inline bool isFilter(Blend b) { return b == Blend::Blur || b == Blend::Sharpen; }

/** Channel mask bits. */
enum Channels : uint8_t
{
    kRed = 1,
    kGreen = 2,
    kBlue = 4,
    kAlpha = 8,
    kRgb = 7,
    kRgba = 15,
};

/** "rgba", "rgb", "r", "ga"... to bits; 0 when the string has no channel letter. */
uint8_t channelsFromString(std::string_view s);
std::string channelsToString(uint8_t channels);

/**
 * base blended toward brush by weight w (0..1), only in the masked channels. For Blur and Sharpen, brush is the
 * neighbourhood average: Blur moves toward it, Sharpen away from it. Results are not clamped (linear colors may exceed
 * 1), except that Subtract and Sharpen stop at 0.
 */
glm::vec4 blend(Blend mode, const glm::vec4& base, const glm::vec4& brush, float w, uint8_t channels);

/** sRGB (what color pickers show) to linear and back, per channel; alpha passes through. */
glm::vec4 srgbToLinear(const glm::vec4& c);
glm::vec4 linearToSrgb(const glm::vec4& c);

struct BrushSettings
{
    Blend blend = Blend::Replace;
    glm::vec4 color{1.0f}; // linear
    /** Most a stroke adds, 0..1. Strokes don't build up past it, however often they pass. */
    float strength = 1.0f;
    uint8_t channels = kRgba;
    scene::softSelect::Curve falloff;
    /** Skip points facing away from the view. */
    bool frontFacesOnly = true;
    /** Paint points hidden behind other parts of the mesh too. */
    bool paintThrough = false;
    /** Mirror across the symmetry map given to the PaintMesh. */
    bool mirror = false;
};

/** One brush stamp, object space. */
struct Dab
{
    glm::dvec3 center{0.0};
    double radius = 1.0;
    /** Multiplies the weight: pen pressure mapped to opacity. */
    double opacity = 1.0;
    /** Into the screen. Zero disables the facing test. */
    glm::dvec3 viewDir{0.0};
    /** Camera position, for the occlusion test. */
    glm::dvec3 eye{0.0};
};

/** Corner colors a stroke or edit changed, enough to undo and redo it. */
struct ColorDelta
{
    std::vector<Index> corners;
    std::vector<glm::vec4> before, after;

    bool empty() const { return corners.empty(); }
    size_t bytes() const { return corners.size() * (sizeof(Index) + 2 * sizeof(glm::vec4)); }
};

/**
 * A mesh being painted: a working copy of one color set plus what brush queries need (BVH, point corners, neighbours,
 * normals). Topology and positions never change while it lives.
 */
class PaintMesh
{
public:
    /** set must exist on mesh. */
    PaintMesh(std::shared_ptr<const geo::Mesh> mesh, std::string set);

    const geo::Mesh& mesh() const { return *mMesh; }
    const std::shared_ptr<const geo::Mesh>& meshPtr() const { return mMesh; }
    const std::string& set() const { return mSet; }
    const geo::MeshBvh& bvh() const { return mBvh; }
    std::span<const glm::vec4> colors() const { return mColors; }
    std::span<const glm::vec3> normals() const { return mNormals; }
    std::span<const Index> pointCorners(Index p) const { return {mCorners.data() + mCornerOffsets[p], mCorners.data() + mCornerOffsets[p + 1]}; }
    std::span<const Index> neighbours(Index p) const { return {mAdj.data() + mAdjOffsets[p], mAdj.data() + mAdjOffsets[p + 1]}; }

    /** Takes a newer snapshot of the same mesh (same topology and positions, colors may differ) without rebuilding. */
    void rebase(std::shared_ptr<const geo::Mesh> mesh);

    void setSymmetry(geo::SymmetryMap map) { mSymmetry = std::move(map); }
    const geo::SymmetryMap& symmetry() const { return mSymmetry; }
    /** Points allowed to change (component selection); empty allows all. */
    void setMask(std::vector<uint8_t> pointMask) { mMask = std::move(pointMask); }

    bool raycast(const glm::dvec3& origin, const glm::dvec3& dir, geo::BvhHit& hit) const;
    /** The color under a hit, blended from its corners (eyedropper). */
    glm::vec4 sample(const geo::BvhHit& hit) const;

    void beginStroke(const BrushSettings& s);
    void dab(const Dab& d);
    ColorDelta endStroke();
    bool stroking() const { return mStroking; }

    /** Corners changed since the last call (for the viewport), each once. */
    std::vector<Index> takeChanged();
    /** Puts the delta's before (undo) or after state into the working colors. */
    void apply(const ColorDelta& d, bool undo);

private:
    void gather(const Dab& d, std::vector<std::pair<Index, float>>& out) const;
    void touchPoint(Index p, float w);

    std::shared_ptr<const geo::Mesh> mMesh;
    std::string mSet;
    geo::MeshBvh mBvh;
    std::vector<glm::vec4> mColors;
    std::vector<glm::vec3> mNormals;
    std::vector<uint32_t> mCornerOffsets;
    std::vector<Index> mCorners;
    std::vector<uint32_t> mAdjOffsets;
    std::vector<Index> mAdj;
    geo::SymmetryMap mSymmetry;
    std::vector<uint8_t> mMask;

    BrushSettings mSettings;
    std::array<float, 257> mFalloff{};
    bool mStroking = false;
    // Per stroke: each touched point's strongest weight and its corners' colors at stroke start.
    std::vector<uint32_t> mStamp;
    uint32_t mStroke = 0;
    std::vector<float> mWeight;
    std::vector<Index> mTouched;
    std::vector<uint32_t> mStartIndex; // into mStart, per corner
    std::vector<Index> mStartCorners;
    std::vector<glm::vec4> mStart;
    std::vector<uint8_t> mChangedFlag;
    std::vector<Index> mChanged;
};

// ------------------------------------------------------------------------------------------------ whole-set edits

/** Points an edit applies to: all valid points when the selection is empty. */
std::vector<Index> targetPoints(const geo::Mesh& mesh, std::span<const Index> selectedPoints);

/** Points connected to the seeds (whole shells). */
std::vector<Index> elementPoints(const geo::Mesh& mesh, std::span<const Index> seeds);

/** Replaces each corner color c of the points with fn(point, c), collecting the delta. Runs in parallel. */
ColorDelta editPoints(geo::Mesh& mesh, std::string_view set, std::span<const Index> points,
                      const std::function<glm::vec4(Index point, const glm::vec4& color)>& fn);

/** Replaces the set's values with one color per corner (by corner id), collecting the delta. */
ColorDelta setCorners(geo::Mesh& mesh, std::string_view set, std::span<const glm::vec4> perCorner);

ColorDelta fill(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, Blend mode, const glm::vec4& color, float opacity,
                uint8_t channels);

enum class GradientType : uint8_t
{
    Linear,
    Radial,
};

/** from (weight 0, colorA) to to (weight 1, colorB) in object space; radial measures distance from `from` out to |to - from|. */
ColorDelta gradient(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, GradientType type, const glm::dvec3& from,
                    const glm::dvec3& to, const glm::vec4& colorA, const glm::vec4& colorB, uint8_t channels);

ColorDelta invert(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, uint8_t channels);

struct Levels
{
    float inBlack = 0.0f;
    float inWhite = 1.0f;
    float gamma = 1.0f;
    float outBlack = 0.0f;
    float outWhite = 1.0f;
};
float applyLevels(float v, const Levels& l);
ColorDelta levels(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, const Levels& l, uint8_t channels);

/** Laplacian smoothing of point colors over edges. */
ColorDelta blur(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, int iterations, float strength, uint8_t channels);

// ------------------------------------------------------------------------------------------------ bakes, per point 0..1

enum class BakeType : uint8_t
{
    AmbientOcclusion,
    Cavity,
    Curvature,
    Height,
    Count
};
std::string_view bakeName(BakeType t);
std::optional<BakeType> bakeFromName(std::string_view name);

struct BakeOptions
{
    BakeType type = BakeType::AmbientOcclusion;
    /** AO rays per point. */
    int rays = 64;
    /** AO: hits farther than this don't occlude (cm); 0 uses the mesh's size. */
    double maxDistance = 0.0;
    /** Cavity and curvature: higher spreads the 0..1 range over gentler shapes. */
    double contrast = 1.0;
    /** Height: object-space axis 0..2, from the mesh's lowest to highest point. */
    int axis = 1;
    /** Smoothing passes over the result. */
    int blur = 0;
};

/** One value per point (white = open / convex / high; AO 1 = unoccluded). Points not on a face get 1. */
std::vector<float> bake(const geo::Mesh& mesh, const BakeOptions& o);

/** Writes per-point values into the masked channels of the points' corners. */
ColorDelta writeChannel(geo::Mesh& mesh, std::string_view set, std::span<const Index> points, std::span<const float> perPoint, uint8_t channels);
} // namespace lila::vcolor
