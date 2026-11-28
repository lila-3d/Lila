#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <array>
#include <span>
#include <vector>

namespace lila::geo
{
/**
 * Where each sliding point can go: along the edge to ends[0] for a positive amount, to ends[1] for a negative one. kInvalid
 * where a side has no edge to follow (a border), so the point stays put that way.
 */
struct SlideRails
{
    std::vector<Index> points;
    std::vector<std::array<Index, 2>> ends;
};

/**
 * Edge slide rails: every point of the edges moves along the unselected edge leaving it on each side, so a loop slides
 * between its neighbours without changing the surface. Each chain of edges is oriented on its own; with a direction, each
 * chain's positive side is the one whose rails point most along it. toWorld turns object-space offsets into the space the
 * direction is in.
 */
LILA_SCENE_API SlideRails edgeSlideRails(const Mesh& mesh, std::span<const Index> edges, const glm::dvec3* direction = nullptr,
                                         const glm::dmat3& toWorld = glm::dmat3(1.0));

/** Vertex slide rails: each point follows its edge most along direction for a positive amount, most against it for a negative one. */
LILA_SCENE_API SlideRails vertexSlideRails(const Mesh& mesh, std::span<const Index> points, const glm::dvec3& direction,
                                           const glm::dmat3& toWorld = glm::dmat3(1.0));

/**
 * Object-space position of each rails point (same order) slid by amount, -1 to 1. As a fraction of each point's own rail, or
 * with even, the same world distance for all: amount times the shortest rail on that side, and no point past its rail's end.
 */
LILA_SCENE_API std::vector<glm::dvec3> slidePositions(const Mesh& mesh, const SlideRails& rails, double amount, bool even,
                                                      const glm::dmat3& toWorld = glm::dmat3(1.0));
} // namespace lila::geo
