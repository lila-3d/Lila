#pragma once

#include "TrimSheet.h"

#include <span>
#include <vector>

/** Trims as geometry of their own: strips along curves, decals over mesh edges, and the slabs Trim Cut carves with. */
namespace trims
{
/** One cross-section of a strip: points across it, the first at the trim's bottom, and the surface normal there. */
struct Station
{
    std::vector<glm::dvec3> across;
    glm::dvec3 normal{0.0, 1.0, 0.0};
};

struct StripOptions
{
    /** The trim the strip runs along: tiling strips repeat in U at the sheet's density, other trims stretch over the length. */
    size_t trim = 0;
    /** Head and tail caps from this trim, mirrored at the tail; none when -1, on loops, or when the strip is too short. */
    int cap = -1;
    /** V of each point across, 0 the trim's bottom to 1 its top; spread evenly when empty. */
    std::vector<double> v;
    /** Slides the strip along a tiling trim, in sheet widths. */
    double uOffset = 0.0;
    bool closed = false;
    /** Above zero: a closed slab reaching this far under the strip (and a hair above it) instead of a surface, its floor mapped like the strip. */
    double cutDepth = 0.0;
};

/** The strip's mesh with uv0 on the sheet and face int "trim" (trim index + 1). Empty for fewer than two stations. */
geo::Mesh stripMesh(const Sheet& sheet, std::span<const Station> stations, const StripOptions& opts);

enum class StripOrient : uint8_t
{
    Flat, // lies on the ground: up is its normal
    Wall, // stands up: across runs up
};

/** Stations width cm across along a polyline, offset cm along their normal. */
std::vector<Station> stationsAlong(std::span<const glm::dvec3> points, bool closed, double width, StripOrient orient, double offset,
                                   glm::dvec3 up = {0.0, 1.0, 0.0});

/** A run of connected edges as its points in order. */
struct EdgeChain
{
    std::vector<geo::Index> points;
    bool closed = false;
};

/** The edges grouped into chains; branching points end chains. */
std::vector<EdgeChain> edgeChains(const geo::Mesh& mesh, std::span<const geo::Index> edges);

/**
 * Stations over a chain of edges, in the mesh's space: the strip wraps over each edge, half its width on each face beside it
 * (V 0 on the right of the chain, 0.5 on the edge, 1 on the left), offset cm off the surface. A boundary edge's strip carries
 * on flat past the border.
 */
std::vector<Station> stationsOverEdges(const geo::Mesh& mesh, const EdgeChain& chain, double width, double offset);

/** The trim a strip width cm across fits best: closest world height among trims with the tag (all non-reserved ones when empty), -1 for none. */
int bestStripTrim(const Sheet& sheet, double width, std::string_view tag);
/** The first trim tagged cap, or -1. */
int capTrim(const Sheet& sheet);
} // namespace trims
