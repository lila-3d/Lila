#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <string>
#include <vector>

namespace lila::geo
{
struct SmartJoinOptions
{
    /** Rows of quads in the bridge; 0 picks them from the gap and the loop's edge length. */
    int segments = 0;
    /** Scales the Hermite tangents leaving the limb and entering the body; 0 is a straight bridge. */
    double tension = 1.0;
    /** Rotates the loop correspondence by this many points. */
    int twist = 0;
    /** Scales the projected loop about its centre on the body. */
    double holeScale = 1.0;
    /** Slides the hole across the body surface, in world units along the hole's tangent frame. */
    double offsetU = 0.0;
    double offsetV = 0.0;
    /** Which limb loop to use: -1 auto (the nearest loop fully outside the body), else the index into the candidates nearest first. */
    int limbLoop = -1;
    /** Removes the limb beyond the loop (the part toward or inside the body); off detaches it as a separate shell. */
    bool deleteBuried = true;
    /** Laplacian passes over the bridge's inner rows and the hole ring. */
    int relax = 0;
    /** Where a trim loop goes past the intersection, as a fraction of the limb's radius. */
    double trimMargin = 0.25;
};

struct SmartJoinResult
{
    bool ok = false;
    Mesh mesh;
    /** 0 to 100. */
    int confidence = 0;
    /** Why the confidence is what it is, most important first; the error when not ok. */
    std::vector<std::string> reasons;
    /** One line, e.g. "matching 8-point loops, square-on, clean: 98%". */
    std::string summary;
    int limbLoopSize = 0;
    int holeSize = 0;
    int triangles = 0; // among the new faces
    int ngons = 0;     // among the new faces
    int supportLoops = 0;
    int segments = 0;
    int loopIndex = -1;
    int loopCandidates = 0;
    bool trimmed = false;
    bool flippedLimb = false;
    int preservedPoints = 0; // input points kept unchanged
    int originalPoints = 0;
};

/**
 * Joins a limb mesh to a body mesh (both in the same space) the way a modeller would by hand, touching only a
 * local region of each.
 *
 * Approach: pick a closed loop on the limb (open border, edge loop, n-gon border, or a new loop inserted just
 * outside where the limb enters the body), ranked by distance to the body along the loop's axis. Project that loop
 * onto the body along the axis to get the hole's shape. Carve a disc of body faces around the hit whose border has
 * as many points as the loop: first by growing a region over the existing faces, otherwise by inserting support
 * edge loops that make an a x b quad block with 2(a+b) border points (the circularize-a-quad-block workflow). The
 * region is replaced by one quad ring from its border to the projected loop, and the limb loop is bridged to that
 * ring with rows of quads along a Hermite curve that leaves the limb along its rails and meets the body along its
 * normal. Counts that cannot match (an odd loop) are stitched with the fewest triangles.
 *
 * The confidence drops for count mismatches, grazing angles, poles or n-gons in the hole region, holes the region
 * cannot contain, projection misses, non-planar loops and long bridges.
 */
LILA_SCENE_API SmartJoinResult smartJoin(const Mesh& limb, const Mesh& body, const SmartJoinOptions& opts = {});
} // namespace lila::geo
