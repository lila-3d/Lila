// SPDX-License-Identifier: Apache-2.0
#pragma once

// Render-time level of detail (LOD) and camera culling for large grooms.
//
// Strands far from the camera are thinned (each kept with a probability that
// falls from 1 at `nearDistance` to `minDensity` at `farDistance`, decided by
// its stable ID, so the same strands stay from frame to frame), with the
// survivors widened to keep the coverage. Strands wholly outside the camera's
// view (plus a margin, for shadows and reflections close to frame) are
// dropped. With backface culling, strands whose root faces away from the camera
// (its normal more than the padding angle past side-on: the back of the head)
// are dropped too. A plan made on one motion key applies to the others, so every
// key keeps the same strands.

#include <cstddef>
#include <vector>

#include "groomlab/strand_set.h"

namespace groomlab {

class ScalpMesh;

struct LodCamera {
    // The camera in the groom's space, row-vector convention (Maya and
    // Arnold): rows 0-2 are the camera's x, y and z axes, row 3 its position.
    // The camera looks down its -z axis.
    float matrix[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    float tanHalfFovX = 0.414f;  // tan of half the horizontal field of view (perspective)
    float tanHalfFovY = 0.311f;  // tan of half the vertical field of view
    // An orthographic camera sees a box, not a wedge: half its width and
    // height in the groom's units, and no distance thinning (everything is
    // the same size on screen).
    bool orthographic = false;
    float orthoHalfWidth = 1.0f;
    float orthoHalfHeight = 1.0f;
};

struct LodSettings {
    bool distance = false;         // thin with distance
    float nearDistance = 10.0f;    // full density up to here (groom space units)
    float farDistance = 100.0f;    // minDensity from here on
    float minDensity = 0.1f;       // density kept far away, 0-1
    float widthCompensation = 1.0f;  // survivors' width x density^-compensation (1 keeps coverage)
    bool frustum = false;          // drop strands outside the view
    float margin = 0.1f;           // view widened by this fraction on each side
    bool backface = false;         // drop strands whose root faces away from the camera
    float backfaceAngle = 20.0f;   // degrees past side-on a root may face away and stay (padding: silhouettes, long hair)
};

struct LodPlan {
    std::vector<std::size_t> kept;   // strand indices, in order
    std::vector<float> widthScale;   // per kept strand
    std::size_t culled = 0;          // outside the view
    std::size_t thinned = 0;         // dropped for distance
    std::size_t backfacing = 0;      // rooted facing away from the camera
};

// The plan for `strands`. A root's facing is the normal of `scalp` under it (the scalp the
// strands were evaluated on), else its strand's first segment (hair grows out of the skin).
LodPlan planLod(const StrandSet& strands, const LodCamera& camera, const LodSettings& settings,
                const ScalpMesh* scalp = nullptr);

// The kept strands, widths scaled. `strands` must have the topology the plan
// was made on (another motion key of it).
StrandSet applyLod(const StrandSet& strands, const LodPlan& plan);

}  // namespace groomlab
