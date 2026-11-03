#pragma once

#include "uv/Chart.h"
#include "uv/Unwrap.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

/** Small helpers the uv sources share. Internal to src/uv. */
namespace lila::uv::detail
{
inline constexpr double kTwoPi = glm::two_pi<double>();

/** A triangle's three uvs. */
using Tri2 = std::array<glm::dvec2, 3>;

/** One key for the edge between points a and b, either way round. */
inline uint64_t edgeKey(Index a, Index b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

/** The chart has real uv area and no folded triangles, so its layout is worth keeping. */
inline bool usable(const Chart& ch)
{
    double a = std::abs(ch.areaUv());
    return std::isfinite(a) && a > 1e-14 * std::max(1.0, ch.area3d()) && foldedTriangles(ch) == 0;
}
} // namespace lila::uv::detail
