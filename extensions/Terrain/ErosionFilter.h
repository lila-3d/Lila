#pragma once

#include "Terrain.h"

#include <cstdint>
#include <string>

namespace terrain
{
/**
 * The erosion filter: a per-sample approximation of eroded detail after Rune Skovbo Johansen's published idea. Each
 * octave lays gullies running straight down the local slope (stripes across the gradient, blended over jittered cells),
 * deepest on steep ground and absent on flat ground, and the next octave follows the slope the previous ones made, so
 * small gullies branch off big ones. Not hydrologically true, but cheap, local and world-space: tiles match and it runs
 * per sample on the GPU, so it suits previews, distant terrain and endless worlds.
 */
struct ErosionFilterParams
{
    /** Spacing of the largest gullies, cm. */
    double scale = 4000.0;
    int octaves = 4;
    /** Gully depth as a fraction of the spacing, on a 45 degree slope. */
    double strength = 0.12;
    /** Each octave's spacing and depth against the previous one. */
    double lacunarity = 2.0;
    double gain = 0.5;
    /** Slope where gullies reach full depth (rise over run); gentler ground gets shallower ones. */
    double slopeFull = 0.6;
    uint32_t seed = 0;
};

/** Writes the wear mask (gully floors) next to changing the heights. */
void erosionFilter(Heightfield& h, const ErosionFilterParams& p);

/** The filter's height offset at one world point, given the input gradient there (rise over run along x and z). */
double erosionFilterOffset(const ErosionFilterParams& p, double wx, double wz, double gx, double gz);
} // namespace terrain
