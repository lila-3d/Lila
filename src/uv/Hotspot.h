#pragma once

#include "core/Export.h"

#include "uv/Chart.h"
#include "uv/Layout.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** Hotspotting: islands rectified, measured in 3D and mapped onto the best fitting rectangle of a trim sheet or hotspot atlas. */
namespace lila::uv
{
enum class TrimKind : uint8_t
{
    StripU, // full width strip that tiles along U
    StripV, // full height strip that tiles along V
    Rect,   // a hotspot rectangle, islands fit inside it
    Radial, // a round element (caps, discs, rings)
    Unique, // one-off detail, only used when asked for by tag
    Flat,   // plain area with no edge detail, only used when asked for by tag
};

enum class TrimFit : uint8_t
{
    Hotspot,  // scaled uniformly to fit inside the inset rectangle
    Seamless, // the tiling axis set by texel density and free to run past the rectangle
    Fill,     // stretched to fill the inset rectangle
};

/** One target rectangle, in uv (0-1 over the sheet, V up). */
struct HotspotTrim
{
    glm::dvec2 min{0.0};
    glm::dvec2 max{1.0};
    /** World size of the rectangle in cm, from the sheet's texel density. */
    glm::dvec2 worldSize{100.0};
    TrimKind kind = TrimKind::Rect;
    TrimFit fit = TrimFit::Hotspot;
    /** Gap kept inside the rectangle, uv. */
    double inset = 0.0;
    std::vector<std::string> tags;
    /** Trims sharing a group (>= 0) are interchangeable: variation picks among them. */
    int altGroup = -1;
    bool allowRotate = true;
    /** Preferred facing in world space (unit), or zero for any. */
    glm::dvec3 normal{0.0};
    /** Never chosen automatically. */
    bool reserved = false;
};

enum class HotspotOrient : uint8_t
{
    AsIs,     // keep the rectified orientation
    WorldUp,  // world up runs along +V; flat islands fall back to LongestU
    LongestU, // longest side along U
};

struct HotspotOptions
{
    /** Faces meeting sharper than this, degrees, are split into separate islands (seams marked). 0 keeps current seams only. */
    double splitAngle = 30.0;
    HotspotOrient orient = HotspotOrient::WorldUp;
    /** World up, for WorldUp. */
    glm::dvec3 up{0.0, 1.0, 0.0};
    /** Islands facing within this many degrees of up or down count as flat. */
    double flatTolerance = 10.0;
    double aspectWeight = 1.0;
    double sizeWeight = 1.0;
    double normalWeight = 0.5;
    /** Size error (|log2| of island over trim size) past which a trim is rejected. The best one is still used when all are. */
    double maxSizeError = 1.0;
    /** Only trims with this tag, when not empty. */
    std::string tag;
    bool allowRotate = true;
    uint32_t seed = 0;
    /** Takes the n-th best candidate instead of the best: repeated calls cycle through alternatives. */
    int cycle = 0;
    /** Random offset along tiling strips. */
    bool randomOffset = true;
    /** Random 180 degree turn where the trim allows rotation. */
    bool randomFlip = false;
    bool randomMirror = false;
    /** Score difference under which candidates count as tied; variation picks among ties and alt groups. */
    double tieTolerance = 0.05;
    /**
     * Islands too tall for every tiling strip (past maxSizeError) are cut into courses about the tallest strip's height,
     * each its own island, so a wall becomes rows of boards or panels at true scale instead of one strip stretched over it.
     */
    bool sliceOversized = true;
};

/** An island's rectified size in world units and what it looks like. */
struct IslandMeasure
{
    double width = 0.0;  // along U, cm
    double height = 0.0; // along V, cm
    double area3d = 0.0;
    /** Fraction of the uv bounding box the island covers (1 for a clean rectangle). */
    double fill = 0.0;
    bool radial = false;
    glm::dvec3 normal{0.0}; // area weighted, unit
    StraightenResult rectified = StraightenResult::Failed;
};

struct TrimCandidate
{
    int trim = -1;
    double score = 0.0;
    double sizeError = 0.0; // |log2| of island over trim size
    bool rotated = false;   // island turned 90 degrees to fit
};

struct HotspotIsland
{
    std::vector<Index> faces;
    IslandMeasure measure;
    int trim = -1;
    double score = 0.0;
    double sizeError = 0.0;
    bool rotated = false;
    /** Every candidate was past maxSizeError; the best was used anyway. */
    bool poor = false;
};

struct HotspotResult
{
    std::vector<HotspotIsland> islands;
    size_t matched = 0;
    size_t poor = 0;
    size_t failed = 0;
    Index seamsAdded = 0;
    /** Islands cut into courses (sliceOversized). */
    size_t sliced = 0;
};

/**
 * Rectifies the chart (or keeps it round when radial), orients it and measures its world size from 3D. The chart's
 * uvs are left rectified and oriented, in 3D scale.
 */
LILA_SCENE_API IslandMeasure measureChart(Chart& chart, const HotspotOptions& opts = {});

/** Trims that suit the island, best first; ties ordered by trim index. */
LILA_SCENE_API std::vector<TrimCandidate> rankTrims(const IslandMeasure& island, std::span<const HotspotTrim> trims, const HotspotOptions& opts = {});

/** Moves a measured chart onto the trim; rand01 draws the variation (offset, flip, mirror) in [0, 1). */
LILA_SCENE_API void placeChart(Chart& chart, const HotspotTrim& trim, bool rotate, const HotspotOptions& opts, double rand0, double rand1, double rand2);

/** Marks seams where faces meet sharper than degrees (and on the faces' outer edges). Returns how many were added. */
LILA_SCENE_API Index markSeamsByAngle(geo::Mesh& mesh, std::span<const Index> faces, double degrees);

/**
 * Hotspots the faces (all when empty) onto the trims: split by angle, unwrap, rectify, orient, measure, rank, pick
 * (variation from the seed), place. Deterministic for a given seed and cycle.
 */
LILA_SCENE_API HotspotResult hotspot(geo::Mesh& mesh, std::span<const Index> faces, std::span<const HotspotTrim> trims, std::string_view uvSet,
                                     const HotspotOptions& opts = {});

/** Stable hash to [0, 1) from a seed and two keys. */
LILA_SCENE_API double hotspotRandom(uint32_t seed, uint64_t a, uint64_t b);
} // namespace lila::uv
