#pragma once

#include "Terrain.h"

#include <cstdint>
#include <string>

/** Shaping operators on heightfields: terraces, rock strata, folds, warps, sharpened ridges and cliffs. Heights in cm. */
namespace terrain
{
/** The height stepped into treads every step cm; sharpness 0 is no change, 1 sheer risers. */
double terraceHeight(double height, double step, double sharpness, double offset);

struct TerraceParams
{
    double step = 2000.0;
    double sharpness = 0.8;
    double offset = 0.0;
    /** Mask scaling the effect per sample (empty: everywhere). */
    std::string mask;
    double strength = 1.0;
};
void terrace(Heightfield& h, const TerraceParams& p);

/** Rock layers: bands of hard and soft rock (mask strata, 1 hard) tilted across the terrain, hard bands standing out as benches. */
struct StrataParams
{
    double thickness = 1500.0;
    /** Dip of the layers, degrees, toward dipDirection (degrees from +X toward +Z). */
    double dip = 5.0;
    double dipDirection = 0.0;
    /** How far the hard bands stand out, as a fraction of the thickness. */
    double strength = 0.3;
    /** Waviness of the layers, as a fraction of the thickness. */
    double noise = 0.3;
    uint32_t seed = 0;
};
void strata(Heightfield& h, const StrataParams& p);

/** Folding: the ground buckled into parallel ridges across direction, their crests bent by noise. */
struct FoldParams
{
    double wavelength = 30000.0;
    double amplitude = 3000.0;
    /** Degrees from +X toward +Z that the folds run across. */
    double direction = 0.0;
    /** Bend of the crests, as a fraction of the wavelength. */
    double bend = 0.3;
    uint32_t seed = 0;
};
void fold(Heightfield& h, const FoldParams& p);

/** Domain warp: heights and masks pulled sideways by noise, so straight features meander. */
struct WarpParams
{
    double amount = 2000.0;
    double featureSize = 15000.0;
    uint32_t seed = 0;
};
void warp(Heightfield& h, const WarpParams& p);

/** Ridges pushed up where the ground stands above its surroundings within radius cm. */
struct RidgeSharpenParams
{
    double amount = 0.6;
    double radius = 3000.0;
};
void ridgeSharpen(Heightfield& h, const RidgeSharpenParams& p);

/** Cliffs: ground steeper than the threshold stepped into sheer rock bands (mask cliff). */
struct CliffParams
{
    double threshold = 35.0;
    /** Height of each rock band, cm. */
    double step = 1500.0;
    double strength = 0.8;
    uint32_t seed = 0;
};
void cliff(Heightfield& h, const CliffParams& p);

/** Separable box blur of a grid, radius in samples, clamped at the edges; passes 2 is close to a Gaussian. */
std::vector<float> boxBlurred(const std::vector<float>& v, int nx, int nz, int radius, int passes = 2);
} // namespace terrain
