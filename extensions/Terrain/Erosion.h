#pragma once

#include "Terrain.h"

#include <cstdint>
#include <string>
#include <vector>

namespace terrain
{
/**
 * Where water goes on a heightfield: depressions filled so every sample drains to the edge, each sample's steepest
 * downhill neighbour on that filled surface, and how many samples drain through it.
 */
struct Drainage
{
    /** Heights with every pit filled to its spill level, plus a tiny slope so flats drain. */
    std::vector<float> filled;
    /** Index of the sample each one drains to; itself on the edge. */
    std::vector<uint32_t> receiver;
    /** Samples ordered from the outlets uphill: every sample comes after its receiver. */
    std::vector<uint32_t> order;
    /** Samples draining through each one, itself included. */
    std::vector<float> area;
};

/** Priority-flood fill, steepest-descent receivers (8 neighbours) and accumulated drainage. Deterministic. */
Drainage drain(const Heightfield& h);

struct ErosionParams
{
    /** Steps of stream-power erosion; more is an older landscape. */
    int iterations = 60;
    /** How fast rivers cut: the stream-power coefficient per step. */
    double strength = 0.0005;
    /** Exponent on drainage area; 0.5 is typical of real rivers. */
    double areaExponent = 0.5;
    /** Rise per step, cm; with erosion this builds ranges whose valleys all drain. */
    double uplift = 0.0;
    /** Mask scaling uplift per sample (empty: uniform). */
    std::string upliftMask;
    /** Mask from 0 soft to 1 hard rock; hard rock erodes slower. */
    std::string hardnessMask;
    /** Talus angle, degrees: slopes steeper than this shed material downhill. */
    double talusAngle = 38.0;
    /** Thermal steps after each stream-power step. */
    int thermalIterations = 2;
};

/**
 * Stream-power erosion with uplift (implicit, Braun and Willett 2013) on the drainage network, with thermal erosion at
 * the talus angle. Writes the masks flow (0 to 1, log drainage), wear (where material was removed), deposit (where it
 * landed) and lakes (filled depressions of the result). Edge samples are base level and never move.
 */
void erode(Heightfield& h, const ErosionParams& p);

/** Thermal erosion alone: slopes over the talus angle relax. Adds to the deposit mask when it exists. */
void thermal(Heightfield& h, double talusAngle, int iterations);

/** Writes the flow and lakes masks without changing the heights. */
void writeFlowMasks(Heightfield& h, const Drainage& d);
} // namespace terrain
