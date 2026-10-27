#pragma once

#include "core/Export.h"

#include "scene/Components.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

/** orientPivot: turns a pivot to aim from one picked piece of a component selection to the next, or along a single piece. */
namespace lila::scene::orientpivot
{
/** One connected piece of a component selection, in the space of the mesh it came from. */
struct Piece
{
    glm::dvec3 centre{0.0};
    /** Where the piece points on its own: a face's normal, an edge's direction, a loop's plane normal. Unit or zero. */
    glm::dvec3 aim{0.0};
    /** The up that goes with aim: a face's longest edge, the faces beside an edge, a loop's first point. Unit or zero. */
    glm::dvec3 up{0.0};
    /** Area-weighted normal of the surface under it. Unit or zero. */
    glm::dvec3 normal{0.0};
    /** From the centre toward its first picked point. Unit or zero. */
    glm::dvec3 toStart{0.0};
    /** Sum and count of its points, for the selection centre. */
    glm::dvec3 pointSum{0.0};
    size_t pointCount = 0;
    /** Pick order of its earliest picked component. */
    size_t rank = 0;
    /** Whether aim and up are surface normals, which a non-uniform scale turns differently from directions along it. */
    bool aimIsNormal = false;
    bool upIsNormal = false;
};

struct Pick
{
    uint32_t id = 0;
    size_t rank = 0;
};

/**
 * Splits picks (in rank order) into pieces connected through the mesh: points by edges, edges by points, faces by edges.
 * With `split` every pick is a piece of its own. Sorted by rank.
 */
LILA_SCENE_API std::vector<Piece> pieces(const geo::Mesh& mesh, ComponentMode kind, const std::vector<Pick>& picks, bool split);

/** The piece moved by an object-to-world matrix, directions kept unit. */
LILA_SCENE_API Piece toWorld(const Piece& piece, const glm::dmat4& world);

struct Aim
{
    glm::dvec3 aim{0.0};
    /** Up candidates the selection itself suggests, best first. */
    std::vector<glm::dvec3> ups;
};

/**
 * From the first piece's centre to the last's when there are several, else along the single piece. Throws
 * std::invalid_argument when there is nothing to aim along.
 */
LILA_SCENE_API Aim solve(const std::vector<Piece>& sorted);

struct Axis
{
    int index = 0;
    double sign = 1.0;
};
/** "x", "-x", "y", "-y", "z" or "-z"; throws std::invalid_argument otherwise. */
LILA_SCENE_API Axis parseAxis(const std::string& name);

/**
 * Right-handed rotation (columns) whose aim axis points along aim and whose up axis leans toward the first of ups not
 * parallel to aim, else the world axis least aligned with it. Throws std::invalid_argument when both axes are the same.
 */
LILA_SCENE_API glm::dmat3 aimFrame(const glm::dvec3& aim, const std::vector<glm::dvec3>& ups, Axis aimAxis, Axis upAxis);

/** Set by the UI: puts the component manipulator at a world position and frame until the selection changes, with undo. */
using ManipulatorSink = std::function<void(const glm::dvec3& position, const glm::dmat3& frame)>;
LILA_SCENE_API void setManipulatorSink(ManipulatorSink sink);

void registerCommand();
} // namespace lila::scene::orientpivot
