#pragma once

#include "Terrain.h"

#include <cstdint>
#include <string>

namespace terrain
{
struct BareEarthParams
{
    /** Widest object removed (a tree stand or building block), cm. */
    double radius = 6000.0;
    /** Tallest object removed, cm; taller bumps are kept as terrain. */
    double maxHeight = 4000.0;
    /** Only where this mask is (OSM landcover_forest, landcover_urban ...); empty is everywhere. */
    std::string mask;
};

struct BareEarthStats
{
    /** Samples lowered, and the most and mean lowering, cm. */
    size_t lowered = 0;
    double most = 0.0;
    double mean = 0.0;
};

/**
 * A surface model (Copernicus, SRTM: tops of trees and roofs) smoothed toward bare ground: a morphological opening at
 * radius finds what stands up from the ground around it (an opening keeps planes and slopes, so only bumps narrower
 * than the radius show), and bumps no taller than maxHeight come down. Writes mask bareEarth (how much came off, 0 to 1
 * of maxHeight).
 */
BareEarthStats bareEarth(Heightfield& h, const BareEarthParams& p);

struct AmplifyParams
{
    /** Samples per original sample along each side: 2, 4 or 8 (rounded up to a power of two). */
    int factor = 4;
    /** Strength of the new detail, scaled by the slope (flat ground stays flat). */
    double detail = 0.5;
    /** Erosion steps at each doubling. */
    int iterations = 15;
    uint32_t seed = 0;
};

/**
 * Coarse real-world data (30 m) refined to game scale: each doubling resamples, adds slope-scaled detail and erodes at
 * the finer cell, then restores the coarse shape, so the place stays where it was and gains gullies, ridgelets and
 * scree. Masks are resampled. Returns the finer heightfield; cell shrinks by factor.
 */
Heightfield amplify(const Heightfield& h, const AmplifyParams& p);
} // namespace terrain
