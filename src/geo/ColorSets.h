#pragma once

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Named vertex color sets, Maya style: each set is a corner (face-vertex) float4 RGBA layer named "color:<set>", linear
 * values. Being ordinary corner layers, they follow every op that carries attributes. One set is current (displayed and
 * painted); sets marked half precision keep their values rounded to 16-bit floats. The first set is what exporters
 * write as the primary vertex color (Unreal's).
 */
namespace lila::geo::colors
{
inline constexpr std::string_view kLayerPrefix = "color:";

std::string layerName(std::string_view set);
/** The set a layer holds, empty when the layer is not a color set. */
std::string_view setOf(std::string_view layer);

/** Set names in layer order. */
std::vector<std::string> sets(const Mesh& mesh);
bool has(const Mesh& mesh, std::string_view set);
/** Empty when missing. */
std::span<glm::vec4> values(Mesh& mesh, std::string_view set);
std::span<const glm::vec4> values(const Mesh& mesh, std::string_view set);

/** Adds a set filled with fill (new faces get fill too), or returns the existing one. The first set becomes current. */
std::span<glm::vec4> add(Mesh& mesh, std::string_view set, const glm::vec4& fill = glm::vec4(1.0f));
bool remove(Mesh& mesh, std::string_view set);
bool rename(Mesh& mesh, std::string_view from, std::string_view to);
/** Copies from into to (created or overwritten). */
bool copy(Mesh& mesh, std::string_view from, std::string_view to);
/** Moves a set to position index among the sets (0 is the primary, exported first). */
bool move(Mesh& mesh, std::string_view set, size_t index);

/** The current set: the stored one when it exists, otherwise the first set; empty when the mesh has none. */
std::string current(const Mesh& mesh);
bool setCurrent(Mesh& mesh, std::string_view set);

bool isHalf(const Mesh& mesh, std::string_view set);
/** Marks the set half or full precision; going to half rounds its values. */
void setHalf(Mesh& mesh, std::string_view set, bool half);
float roundToHalf(float v);
void roundToHalf(std::span<glm::vec4> values);
/** Rounds a half set's values after they were written; does nothing to full-precision sets. */
void settle(Mesh& mesh, std::string_view set);

/** A free set name: base, else base1, base2, ... */
std::string uniqueName(const Mesh& mesh, std::string_view base = "colorSet");

/** Average of each point's corners in the set; points without corners get white. */
std::vector<glm::vec4> pointColors(const Mesh& mesh, std::string_view set);
/** Writes per-point colors to every corner of each point. */
void setFromPoints(Mesh& mesh, std::string_view set, std::span<const glm::vec4> perPoint);

/**
 * Turns plain "color" layers (point or corner, and "color1", "color2"... from older importers) into color sets named
 * colorSet, colorSet1, ... and restores the white default of every set (files keep values, not defaults).
 */
void adopt(Mesh& mesh);
} // namespace lila::geo::colors
