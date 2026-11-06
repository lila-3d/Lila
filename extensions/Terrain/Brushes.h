#pragma once

#include "Stamps.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * Terrain height and mask brushes: every SculptOp, with brush falloffs, alpha tips, pen pressure per dab and mirroring.
 * A stroke's settings (all lengths and heights in the heightfield's own space, cm):
 *
 *   flatten   plane: true levels to the plane fitted under the stroke instead of one height
 *   ramp      endHeight; the ramp runs from the first point (at height, else the ground there) to the last
 *   terrace   step, sharpness (0 to 1), offset
 *   stamp     stamp: a library name (stampNames) or an image file; angle (degrees); blend: add, raise or lower
 *   erode     iterations of stream-power and thermal erosion under the brush
 *   uplift    mask (default uplift) and value: paints the mask Erode Terrain lifts by
 *   relax     iterations
 *   mask      mask (default paint) and value (0 to 1)
 *   patch     patch: {x0, z0, step, nx, nz, delta} heights added over a block of samples (delta packed by packFloats),
 *             faded out at the block's rim; written by terrainSculptRegion, with the volume it could not represent
 *   any       alpha: a library stamp or image (alpha:// or a file) shaping the tip; angle turns it
 */
namespace terrain
{
/** True when a stroke goes through applyBrushStroke rather than the classic sculpt path. */
bool usesBrushPath(const SculptStroke& s);

void applyBrushStroke(Heightfield& h, const SculptStroke& s);

/** The stamp library: procedural height shapes. */
const std::vector<std::string>& stampNames();
bool isStampName(const std::string& name);

/** A library stamp's height at (u, v) in the unit disc, about -1 to 1, falling to 0 at the rim. */
double stampShape(const std::string& name, double u, double v, uint32_t seed = 0);

/** Floats as base64 text, and back (empty on malformed input). */
std::string packFloats(const std::vector<float>& v);
std::vector<float> unpackFloats(const std::string& text);

/** A stamp as a size x size image from 0 to 1, 0.5 being no change. */
std::vector<float> stampImage(const std::string& name, int size, uint32_t seed = 0);
} // namespace terrain
