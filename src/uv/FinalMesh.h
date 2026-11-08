#pragma once

#include "core/Export.h"
#include "uv/Chart.h"
#include "uv/Optimize.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::uv
{
/** One modifier of a mesh's stack as exported, run on a copy of its input. */
struct StackStep
{
    std::string name;
    std::function<geo::Mesh(const geo::Mesh&)> apply;
};
using Stack = std::vector<StackStep>;

/** How a FinalMap relates cage uvs to the exported mesh's. */
enum class FinalMethod : uint8_t
{
    Cage,   // the stack leaves the surface alone, or its uvs can't be traced: solve on the cage
    Probed, // the stack blends cage uvs linearly; the map was measured by probe uv layers sent through it
};

/**
 * The exported mesh's corner uvs as a sparse linear map of the cage's: exported uv = sum of weight * cage uv over the
 * row's cage corners. Holds for any stack whose modifiers only blend corner uvs (bevel, subdivision, mirror...), for a
 * fixed cage topology and seam layout. Columns are cage corners standing for their whole uv wedge.
 */
struct LILA_SCENE_API FinalMap
{
    FinalMethod method = FinalMethod::Cage;
    /** Why the cage is used although the stack changes the surface, naming the modifier; empty otherwise. */
    std::string warning;
    /** The stack's output for the cage. Null when the stack is empty. */
    std::shared_ptr<const geo::Mesh> exported;
    /** Per exported corner (capacity), its entries in cols and weights; one extra at the end. */
    std::vector<Index> rowStart;
    std::vector<Index> cols;
    std::vector<double> weights;
    /** Per exported face (capacity), the cage face it came from. */
    std::vector<Index> sourceFace;
    /** Face rings around an exported corner's cage face that held its whole row. */
    int rings = 0;
    /** Stack runs the probes took. */
    int runs = 0;

    bool active() const { return method == FinalMethod::Probed; }
    const char* methodName() const { return active() ? "probed" : "cage"; }
};

/**
 * The exported mesh and which cage face each of its faces came from, from one run of the stack, without the uv map:
 * enough to measure exported uvs per cage face (distortion, texel density). Exported is null when the stack leaves the
 * surface alone.
 */
LILA_SCENE_API FinalMap traceFaces(const geo::Mesh& cage, const Stack& stack);

/** The exported faces that came from the cage faces (all live exported faces when empty), ascending. */
LILA_SCENE_API std::vector<Index> exportedFaces(const FinalMap& map, std::span<const Index> cageFaces);

/** True when the stack's output differs from the cage in topology or point positions, so uvs land on another surface. */
LILA_SCENE_API bool changesSurface(const geo::Mesh& cage, const geo::Mesh& exported);

/** Runs the stack on a copy of the mesh. */
LILA_SCENE_API geo::Mesh runStack(const geo::Mesh& mesh, const Stack& stack);

/** Measures the map. Cage (with no warning) when the stack leaves the surface unchanged. */
LILA_SCENE_API FinalMap buildFinalMap(const geo::Mesh& cage, std::string_view uvSet, const Stack& stack);

/** The exported triangles of one chart, with their uvs as a map of the chart's vertices. */
struct LILA_SCENE_API FinalIsland
{
    std::vector<glm::dvec3> pos;    // per local corner
    std::vector<glm::dvec2> offset; // per local corner: the part of its uv from cage wedges outside the chart
    std::vector<Index> rowStart;    // per local corner, into cols and weights; one extra at the end
    std::vector<Index> cols;        // chart vertices
    std::vector<double> weights;
    std::vector<std::array<Index, 3>> tris; // local corners
    std::vector<Index> triFace;             // exported face per triangle

    double area3d() const;
    /** Exported uv per local corner for the chart's current uvs. */
    std::vector<glm::dvec2> uvs(const Chart& chart) const;
    /** Signed exported uv area for the chart's current uvs. */
    double areaUv(const Chart& chart) const;
};

/** Per chart, its exported triangles: each exported face goes to the chart holding most of its corners' weight. */
LILA_SCENE_API std::vector<FinalIsland> finalIslands(const FinalMap& map, const geo::Mesh& cage, std::span<const Chart> charts,
                                                     std::string_view uvSet);

/** Distortion of the exported triangles for the chart's current uvs. */
LILA_SCENE_API ChartStats finalStats(const FinalIsland& island, const Chart& chart);

/**
 * Exported triangles squeezed under kCollapsedFraction of the uv area their island's density gives them, as bake lint
 * counts them (uvMissing).
 */
LILA_SCENE_API size_t collapsedTriangles(const FinalIsland& island, const Chart& chart);
inline constexpr double kCollapsedFraction = 0.02;

/** Sets chart.densityArea so areaUv() / densityArea() is the exported island's texel density. */
LILA_SCENE_API void setDensityArea(Chart& chart, const FinalIsland& island);

struct FinalSolveOptions
{
    /** Isometric pulls each exported triangle to its own shape at the island's density (as rigid as possible). */
    OptimizeMode mode = OptimizeMode::Isometric;
    double mix = 0.5;
    int iterations = 30;
    bool usePins = true;
};

/**
 * Moves the chart's uvs so the exported triangles have the least distortion (local/global through the map, the
 * factorisation reused every iteration). Keeps the island's centroid and its density on the exported mesh; with two or
 * more pins those hold the place and scale instead. Steps never fold an exported triangle when the start has none.
 */
LILA_SCENE_API OptimizeResult solveFinal(Chart& chart, const FinalIsland& island, const FinalSolveOptions& opts = {});

struct FinalRefineResult
{
    FinalMethod method = FinalMethod::Cage;
    std::string warning;
    size_t islands = 0;
    size_t failed = 0;
    size_t collapsedBefore = 0;
    size_t collapsedAfter = 0;
    double distortionBefore = 0.0; // area weighted angle distortion of the exported triangles
    double distortionAfter = 0.0;
};

/**
 * Re-solves the uv islands of the faces (all when empty) against the exported mesh and writes them back. Does nothing
 * when the stack leaves the surface alone.
 */
LILA_SCENE_API FinalRefineResult refineFinal(geo::Mesh& cage, std::span<const Index> faces, std::string_view uvSet, const Stack& stack,
                                             const FinalSolveOptions& opts = {});

/** Sets densityArea on each chart from the exported mesh, when the stack changes the surface. Returns the map used. */
LILA_SCENE_API FinalMap applyDensityAreas(const geo::Mesh& cage, std::span<Chart> charts, std::string_view uvSet, const Stack& stack);
} // namespace lila::uv
