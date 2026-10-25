#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "scene/Plug.h"
#include "seq/MeshSequence.h"

#include <span>
#include <string>
#include <vector>

/**
 * Rigid pieces of an animated mesh (a broken-apart object) and the transform of each piece on each frame, fitted from
 * its points. Shared by the rigid vertex animation bake and baking pieces onto joints.
 */
namespace lila::seq
{
struct RigidPieces
{
    /** The piece of each point, in the combined point order of the meshes given. */
    std::vector<geo::Index> of;
    geo::Index count = 0;
    /** Every mesh had the int `piece` point attribute. */
    bool fromAttribute = true;
};

/** Pieces of compacted meshes in combine order: each mesh's `piece` values (renumbered from 0), or its connected parts. */
LILA_SCENE_API RigidPieces findRigidPieces(const std::vector<geo::Mesh>& meshes);

/** Per piece pivots and per frame per piece fitted transforms, in the basis the fit was given. */
struct RigidFit
{
    /** Rest centroid of each piece. */
    std::vector<glm::dvec3> pivots;
    /** Frame-major, [frame * count + piece]: where the pivot went, and the rotation about it (unit quaternion xyzw). */
    std::vector<glm::dvec3> centres;
    std::vector<glm::dvec4> rotations;
    /** The worst distance between a point and where its piece's transform puts it, and where that was. */
    double residual = 0.0;
    geo::Index residualPiece = 0;
    double residualFrame = 0.0;
};

/**
 * Fits each piece's motion from rest to every frame (Horn's quaternion method, double precision). Points are given in
 * the same order as RigidPieces::of, every frame with the rest's count; basis maps them first (Lila to Unreal axes, or
 * identity). Rotations are sign-continuous from frame to frame.
 */
LILA_SCENE_API RigidFit fitRigidPieces(std::span<const std::span<const glm::dvec3>> frames, std::span<const double> times,
                                       std::span<const glm::dvec3> rest, const RigidPieces& pieces, const glm::dmat3& basis);

/** v rotated by the unit quaternion q (xyzw), the same expression the rigid vertex animation material evaluates. */
LILA_SCENE_API glm::dvec3 rotateByQuat(const glm::dvec4& q, const glm::dvec3& v);

/** Lila space to the space Unreal's importer gives an FBX's vertices for an axis profile (ue5, ue6, generic): a signed permutation. */
LILA_SCENE_API glm::dmat3 unrealBasis(const std::string& profile);

struct PieceJointsResult
{
    std::string mesh;                // the new skinned mesh, at the first frame's shape
    std::string root;                // its root joint, at the origin
    std::vector<std::string> joints; // one per piece, at the piece's pivot, keyed every frame sampled
    double residual = 0.0;           // worst point error of the rigid fit, cm
};

/**
 * Bakes an animated mesh of rigid pieces (a destruction simulation) onto joints: a new mesh skinned one piece per joint,
 * the joints keyed on every frame sampled, ready to edit with the animation tools or export as an FBX skeletal mesh and
 * animation. Main thread; throws std::invalid_argument when the mesh changes point count.
 */
LILA_SCENE_API PieceJointsResult bakePiecesToJoints(scene::Graph& g, scene::NodeId mesh, const SequenceOptions& sampling,
                                                    const std::string& name, bool hideSource);
} // namespace lila::seq
