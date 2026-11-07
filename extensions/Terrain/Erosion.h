#pragma once

#include "Terrain.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

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

/** Asks for the cache line holding p ahead of its use. */
inline void prefetchLine(const void* p)
{
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    _mm_prefetch(static_cast<const char*>(p), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p);
#else
    (void)p;
#endif
}

/**
 * visit(i) for every sample in drainage order (outlets first, or uphill first when reverse), with ahead(i) called some
 * samples early to prefetch what visit will read: these walks hop across the grid and wait on memory, not arithmetic.
 * ahead may read receiver[i], which is prefetched further ahead still.
 */
template <class Ahead, class Visit>
void walkDrainage(const std::vector<uint32_t>& order, const std::vector<uint32_t>& receiver, bool reverse, Ahead&& ahead, Visit&& visit)
{
    constexpr size_t kFar = 32, kNear = 16;
    const size_t n = order.size();
    auto at = [&](size_t k) { return order[reverse ? n - 1 - k : k]; };
    for (size_t k = 0; k < n; ++k)
    {
        if (k + kFar < n)
            prefetchLine(&receiver[at(k + kFar)]);
        if (k + kNear < n)
            ahead(at(k + kNear));
        visit(at(k));
    }
}

/** Priority-flood fill, steepest-descent receivers (8 neighbours) and accumulated drainage. Deterministic. */
Drainage drain(const Heightfield& h);

/**
 * drain() from heights already filled exactly the way drain() fills them (the GPU fill): the order is the flood's own
 * (filled height, then index), so receivers, order and area are drain()'s.
 */
Drainage drainFilled(const Heightfield& h, std::vector<float> filled);

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
    /** Masks pinning heights as erosion runs: weight 0 to 1, and the height held there (drawn ridges, valleys, rivers). */
    std::string constraintMask = "constraint";
    std::string constraintHeightMask = "constraintHeight";
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
