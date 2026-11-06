#pragma once

#include "core/Export.h"

#include "deform/Surface.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

/**
 * Rigid bind: separate pieces (scales, plates, shingles, strips) ride a deforming surface without bending. Each piece
 * follows the best rigid fit of its proximity-wrapped points, then neighbours relax their spacing and push apart where
 * they would pass through each other or the surface. Points are world space; results don't depend on thread count.
 */
namespace lila::deform
{
enum class RigidShape : uint8_t
{
    Off,
    Sphere,
    Capsule,
    Box, // oriented to the piece's principal axes at rest
};

/** What rigidBindRig derives from the rest shape and the piece ids alone. */
struct RigidBindRig
{
    std::vector<uint32_t> offsets{0}, points;   // each piece's point ids
    std::vector<glm::dvec3> centroid;           // rest, of the points
    std::vector<glm::dvec3> shapeCenter;        // rest, of the collision shape
    std::vector<glm::dmat3> axes;               // rest principal axes, columns, right handed
    std::vector<glm::dvec3> extent;             // collision shape half extents along axes, scale applied
    std::vector<double> radius;                 // bounding radius about shapeCenter, scale applied
    std::vector<uint32_t> sampleOffsets{0}, samples; // the outermost point ids, which collide with the drivers
    std::vector<uint32_t> nbOffsets{0}, nb;     // neighbouring pieces
    std::vector<double> restGap;                // per nb entry: separation at rest, negative where the shapes overlap
    RigidShape shape = RigidShape::Box;
    double scale = 1.0;

    size_t pieces() const { return centroid.size(); }
};

/** Piece -1 points belong to no piece. scale grows the collision shapes; neighbours are pieces whose grown bounds touch. */
LILA_SCENE_API RigidBindRig rigidBindRig(std::span<const glm::dvec3> rest, std::span<const int32_t> piece, RigidShape shape, double scale);

/**
 * The proximity binding of every point, plus each piece's surface normal and lowest point above the drivers at rest.
 * Depends on the pieces only, so changing the rig's shape or scale keeps it valid.
 */
struct RigidBindBinding
{
    ProximityBinding wrap;
    std::vector<glm::dvec3> restNormal; // per piece
    std::vector<double> restHeight;     // per piece: its lowest sample's height above the plane of the surface under it
};

LILA_SCENE_API RigidBindBinding rigidBindSetup(std::span<const geo::Mesh* const> drivers, std::span<const glm::dvec3> rest, const RigidBindRig& rig,
                                              const ProximityParams& o);

struct RigidBindParams
{
    double normalOffset = 0.0;    // along the surface normal, cm
    double rotationStrength = 1.0; // 0 keeps the rest orientation, 1 turns with the surface
    double tangentBlend = 0.0;    // 1 turns only to follow the normal, ignoring twist and shear in the surface
    double rotationSmoothing = 0.5; // 0 to 1: each piece turns toward the turn of the surface under it and its neighbours
    double relaxation = 0.0;      // 0 to 1: pieces slide to restore their rest spacing
    int relaxIterations = 4;
    double attachment = 0.0;      // 0 to 1: each pass pulls pieces back toward their wrapped place
    double collisionStrength = 1.0;
    int collisionIterations = 4;
    double collisionLift = 0.5;   // 0 resolves overlaps by sliding along the surface, 1 by lifting the upper piece
    double collisionSoftness = 0.1; // overlaps shallower than this times the piece's size are only partly resolved
    double thickness = 0.0;       // gap kept between pieces that were apart at rest, cm
    bool collideDriver = true;    // pieces never sink deeper into the drivers than at rest
    double reach = 0.5;           // farthest relaxation and collision move a piece from its wrapped place, times its size; 0 no limit
    double maxSlide = 0.0;        // farthest a piece moves along the surface from its wrapped place, cm; 0 no limit
    double maxLift = 0.0;         // farthest a piece lifts off it, cm; 0 no limit
};

/** Moves rest by the binding. transforms, if given, receives each piece's rest-to-current rigid motion. */
LILA_SCENE_API std::vector<glm::dvec3> rigidBindApply(const RigidBindBinding& b, const RigidBindRig& rig, std::span<const std::span<const glm::dvec3>> drivers,
                                                      std::span<const glm::dvec3> rest, const RigidBindParams& o, std::vector<RigidFit>* transforms = nullptr);
} // namespace lila::deform
