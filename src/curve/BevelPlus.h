#pragma once

#include "core/Export.h"
#include "curve/Planar.h"
#include "geo/Mesh.h"

#include <string>

/** Bevel Plus (Maya's): closed planar curves, holes found by containment, into a capped, bevelled solid. */
namespace lila::curve
{
enum class BevelProfile : uint8_t
{
    Straight,
    Convex,  // a quarter round bulging out, tangent to the side and the cap
    Concave, // a cove
    Step,    // stairs, one per segment
    Custom,  // the profile points given
};
LILA_SCENE_API const char* bevelProfileName(BevelProfile p);
LILA_SCENE_API bool parseBevelProfile(std::string_view name, BevelProfile* out);

struct BevelSide
{
    /** How far in from the outline the bevel reaches (cm); 0 is no bevel. */
    double width = 0.3;
    /** How far it rises past the side (cm). */
    double depth = 0.5;
    BevelProfile profile = BevelProfile::Straight;
    int segments = 1;
    /** Custom: (inset, rise) points from the side to the cap, scaled to run from (0, 0) to (1, 1). */
    std::vector<glm::dvec2> custom;
};

/** The profile as (inset, rise) fractions from the side (0, 0) to the cap (1, 1), the inset never decreasing. */
LILA_SCENE_API std::vector<glm::dvec2> bevelProfilePoints(const BevelSide& side);

struct BevelPlusOptions
{
    /** Length of the straight side wall (cm), from the curves' plane along the normal; negative extrudes the other way. */
    double extrudeDepth = 2.0;
    int extrudeDivisions = 1;
    BevelSide front;
    BevelSide back;
    bool frontCap = true;
    bool backCap = true;
    /** Quad grid spacing on the caps and longest outline edge (cm); 0 picks it from the outline. */
    double capSpacing = 0.0;
    /** Caps as quads where they fit; off leaves triangles. */
    bool capQuads = true;
    /** Edges whose faces meet at more than this many degrees are hard. */
    double smoothingAngle = 30.0;
    /** cm per UV unit; 0 fits the caps into 0..1 and scales the sides to match. */
    double uvScale = 0.0;
    /** Plane normal (the extrusion direction); zero fits it to the curves. */
    glm::dvec3 normal{0.0};
    double tolerance = 0.01; // tessellation chord error, cm
    double maxAngle = 8.0;   // tessellation turn per segment, degrees
};

struct BevelPlusResult
{
    geo::Mesh mesh;
    std::vector<std::string> warnings;
};

/**
 * Builds the solid: side walls, front and back bevels following the profile with the corners mitred along the straight
 * skeleton (they meet in a ridge where the shape is thinner than the bevel, never crossing), and quad-dominant caps.
 * Faces carry "part" (0 front cap, 1 front bevel, 2 side, 3 back bevel, 4 back cap), uv0 (caps planar, sides and bevels
 * unwrapped along the outline) and hard edges by smoothingAngle.
 */
LILA_SCENE_API BevelPlusResult bevelPlus(const Splines& curves, const BevelPlusOptions& o);
} // namespace lila::curve
