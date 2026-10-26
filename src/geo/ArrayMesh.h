#pragma once

#include "core/Export.h"

#include "geo/ArrayTransform.h"
#include "geo/Mesh.h"

namespace lila::geo
{
struct ArrayOptions
{
    ArrayLayout layout;
    /** Merge points of neighbouring copies that land within tolerance, and drop the walls two copies share. */
    bool weld = false;
    /** Also weld the last copy to the first. A radial layout spanning a whole turn always does. */
    bool mergeEnds = false;
    double tolerance = 0.01;
};

/** Mesh copied into place per copy transform, with all attributes. Mirroring copies have their faces flipped. */
LILA_SCENE_API void transformCopy(Mesh& mesh, const glm::dmat4& xf);

/**
 * The mesh repeated by the layout: copy 0 is the input, the rest follow in layout order. With weld on, seams between
 * neighbouring copies merge and faces two copies both put on a seam (the cut walls of a modelled wedge) are removed, so a
 * closed wedge arrayed round a full turn becomes one closed, manifold ring.
 */
LILA_SCENE_API Mesh arrayMesh(const Mesh& in, const ArrayOptions& opts);
} // namespace lila::geo
