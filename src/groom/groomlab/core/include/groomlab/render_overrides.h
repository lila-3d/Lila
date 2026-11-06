// SPDX-License-Identifier: Apache-2.0
#pragma once

// Render overrides (Maya: renderDensity, renderWidthScale and renderLengthScale on the groom):
// what a render changes of a groom without changing the groom, so the viewport and the groom's
// look stay as groomed. Applied by the Arnold translator and procedural, and by exports asked to
// (Maya's groomLabExportAlembic / groomLabExportUsd -renderOverrides); never the viewport. The
// defaults change nothing.
//
// - density: the share of the strands rendered, by the preview's thinning (the evaluation's
//   EvalContext::previewDensity, the hosts' density): every strand kept is the one the full groom
//   has (stable IDs), and nodes reading neighbours read the full set's (preview_core.h), so the
//   look is the same with fewer strands. 0-1: a render cannot add strands the groom does not have
//   (raise Scatter's density for that).
// - widthScale: times every strand's width (instances sized by their strand's width keep theirs:
//   they read the width's profile, not its size).
// - lengthScale: each strand scaled about its root, as a Transform node's scale does: shorter or
//   longer with its shape (curls, waves and clumps scaled with it), its root where it was. Not a
//   trim: a strand at 0.5 is the whole strand at half size.

#include "groomlab/graph.h"

namespace groomlab {

struct RenderOverrides {
    float density = 1.0f;
    float widthScale = 1.0f;
    float lengthScale = 1.0f;

    // Whether the shape overrides (width, length) change nothing.
    bool keepsShape() const { return widthScale == 1.0f && lengthScale == 1.0f; }
    // The density clamped to [0, 1] (NaN: 1), times a host's own (an export's -density).
    float densityTimes(float density0) const;
};

// `strands` with the overrides' width and length scales (density is the evaluation's: see
// above); `strands` itself when they change nothing. Points, widths alone: roots, IDs, channels,
// instances and card layers as they are.
StrandsPtr applyRenderOverrides(const StrandsPtr& strands, const RenderOverrides& overrides);

}  // namespace groomlab
