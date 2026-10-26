#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace lila::scene
{
class Graph;

/**
 * Repeating an object by a fixed step, for duplicate count=, Duplicate with Transform and the array tools. The step and
 * run maths are geo/ArrayTransform.h's, shared with the array modifier and the live array group. Matrices are world
 * space, column vectors.
 */
namespace repeat
{
/** One step: translate (cm), rotate (degrees, XYZ order) and scale, applied scale first, then rotate, then translate. */
struct Step
{
    glm::dvec3 translate{0.0};
    glm::dvec3 rotate{0.0};
    glm::dvec3 scale{1.0};

    bool identity() const { return translate == glm::dvec3(0.0) && rotate == glm::dvec3(0.0) && scale == glm::dvec3(1.0); }
};

/** The world matrix of one step about `pivot`, its axes along the columns of `frame` (identity for world axes). */
LILA_SCENE_API glm::dmat4 stepMatrix(const Step& s, const glm::dvec3& pivot, const glm::dmat3& frame = glm::dmat3(1.0));

/** World matrices of copies 1..count of an object whose world matrix is `base`: copy i is step^i * base. */
LILA_SCENE_API std::vector<glm::dmat4> copyMatrices(const glm::dmat4& base, const glm::dmat4& step, int count);

/** The step that takes world matrix `from` to `to`: to = delta * from. */
LILA_SCENE_API glm::dmat4 delta(const glm::dmat4& from, const glm::dmat4& to);

/**
 * Sets translate, rotate and scale so the node's world matrix becomes `world`, keeping its pivots, locked channels
 * included. The rotate channels come out nearest `rotateHint` (default: their current value). Undoable.
 */
LILA_SCENE_API void setWorldMatrix(Graph& g, NodeId id, const glm::dmat4& world, std::optional<glm::dvec3> rotateHint = std::nullopt);
} // namespace repeat
} // namespace lila::scene
