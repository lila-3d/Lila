#pragma once

#include "core/Export.h"
#include "curve/Sweep.h"
#include "geo/CrossSection.h"

#include <optional>
#include <span>
#include <string>

/**
 * Straps, bands and rope around meshes: a slash across the view picks a plane and what it crosses, the cut around them
 * becomes the strap's path, and a band or twisted rope is swept along it, sitting on the surface.
 */
namespace lila::curve
{
/**
 * The slash: two rays (from the eye through the slash's ends, or parallel ones in an orthographic view). The cutting plane
 * holds both, and whatever the cut finds between them is strapped.
 */
struct StrapSlash
{
    glm::dvec3 start{0.0}, startDir{0.0, 0.0, -1.0};
    glm::dvec3 end{0.0}, endDir{0.0, 0.0, -1.0};

    /** The plane's normal (unit), or zero when the rays don't span a plane. */
    glm::dvec3 normal() const;
    /** p lies in the wedge (or strip) between the rays, in front of the eye. */
    bool between(const glm::dvec3& p) const;
};

struct StrapPathOptions
{
    /** cm between the surface and the strap's underside (the tolerance is added, so simplifying never cuts in). */
    double gap = 0.0;
    /** Hollows narrower than twice this (cm) are spanned, as a stiff strap would; 0 follows every dip. */
    double bridge = 0.0;
    /** Pulled tight: the strap spans every hollow (the convex hull of what it goes around). */
    bool taut = false;
    /** Simplify the path within this many cm; 0 keeps every crossing. */
    double tolerance = 0.01;
    /** The strap's width: it clears the surface across all of it, not only along its centre. 0 checks the centre only. */
    double width = 0.0;
};

struct StrapPathResult
{
    /** Closed loops wind counter-clockwise about up; open runs have the outside on tangent x up. */
    std::optional<Spline> path;
    glm::dvec3 up{0.0};
    /** Loops the slash crossed (several are strapped together). */
    int loops = 0;
    std::string warning;
};

/**
 * The strap's path around what the slash crosses: the closed loops of the cut between the rays (several are bundled into
 * one, as a strap round a stack of logs), or, through open meshes with no loop, the longest open run, its outside toward
 * the slash's eye.
 */
LILA_SCENE_API StrapPathResult strapPath(std::span<const geo::SectionMesh> meshes, const StrapSlash& slash, const StrapPathOptions& o = {});

enum class StrapStyle : uint8_t
{
    Band, // a flat strap: width across, thickness off the surface
    Rope, // twisted strands, width across
};

struct StrapOptions
{
    StrapStyle style = StrapStyle::Band;
    double width = 4.0;
    double thickness = 0.35;
    /** Band corners cut back this far (cm); 0 is square. */
    double bevel = 0.0;
    int strands = 3;
    int strandSides = 8;
    /** cm along the rope per turn of the strands; closed loops round it to whole turns. */
    double lay = 8.0;
    /** A buckle (a block round a band, a whipping round rope) at this fraction of the length. */
    bool buckle = false;
    double buckleAt = 0.0;
    double buckleLength = 4.0;
    /** Buckle width over the strap's. */
    double buckleScale = 1.3;
    /** Open paths: tangent x up is outside. Zero takes the path's plane. Closed loops use their own winding. */
    glm::dvec3 up{0.0};
    /** The other side of the path is outside. */
    bool flip = false;
    double vPerCm = 0.0;
};

/** Face attr "strapPart" values. */
enum class StrapPart : int32_t
{
    Body = 0,
    Buckle = 1,
};

struct StrapResult
{
    geo::Mesh mesh;
    double length = 0.0;
    std::vector<std::string> warnings;
};

/** Sweeps the strap along a path lying on the surface (world space in and out); its underside follows the path. */
LILA_SCENE_API StrapResult makeStrap(const Spline& path, const StrapOptions& o);
} // namespace lila::curve
