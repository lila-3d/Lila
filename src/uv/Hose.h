#pragma once

#include "core/Export.h"

#include "uv/Seams.h"

#include <span>
#include <string_view>
#include <vector>

namespace lila::uv
{
struct HoseOptions
{
    /** Longest strip as a multiple of its circumference; longer runs are cut where they are least seen. 0 never cuts. */
    double maxAspect = 8.0;
    /** Longest strip in object units; when set it replaces maxAspect. */
    double maxLength = 0.0;
    /** Hide cuts where they are least visible: occluded, concave and downward facing. */
    bool visibility = true;
    /** Edges these see cost more to cut. */
    std::vector<SeamView> views;
    /** Existing seams inside the faces stay. */
    bool keepSeams = false;
};

struct HoseResult
{
    /** Tube islands: each straightened into a strip. */
    size_t strips = 0;
    /** Rings cut across runs longer than the limit. */
    size_t lengthCuts = 0;
    /** Ends, rims and flat faces between tubes, each projected flat. */
    size_t caps = 0;
    /** Faces that are neither tube nor flat, given auto seams and a plain unwrap. */
    size_t otherFaces = 0;
    /** Strips straighten could not lay as a grid, unwrapped instead. */
    size_t unstraightened = 0;
};

/**
 * Seams for tubes: pipes, hoses, cables and rings. Closed quad strips around the shape are found by topology, stacked
 * into runs, cut once along their length on the least visible side and across where a run is longer than the limit.
 * Ends and flat rims between runs are split off as caps. Writes "seam" on the faces' edges (all live faces when empty).
 */
LILA_SCENE_API HoseResult hoseSeams(geo::Mesh& mesh, std::span<const Index> faces = {}, const HoseOptions& opts = {});

/**
 * hoseSeams, then each strip straightened into an exact grid, caps projected flat (circles on round ends) and anything
 * else unwrapped. UVs are at 3D scale and not packed.
 */
LILA_SCENE_API HoseResult hoseUv(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const HoseOptions& opts = {});
} // namespace lila::uv
