#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"

#include <span>
#include <vector>

namespace lila::bake
{
struct LowInput;

struct RasterSettings
{
    int width = 2048;
    int height = 2048;
    /** 1, 4, 16 or 64: an n x n stratified grid per texel. Other values round down to a square. */
    int supersampling = 4;
    /** Texel rows per band, the unit of parallel work. */
    int bandRows = 32;
};

/** How sample rays are set up from the low (resolved group distances). */
struct CageParams
{
    float frontal = 1.0f;
    float rear = 1.0f;
    /** The group's cage offset multiplier. */
    float offset = 1.0f;
    /** The group's skew: 0 pushes along the averaged smooth normals, 1 along each low face's geometric normal. */
    float skew = 0.0f;
};

/**
 * Rasterises a low's UV layout into stratified samples, tile by tile and band by band. Deterministic: the same low and
 * settings give the same samples in the same order on any machine and thread count. Samples sit at
 * (x + (i + j) / n, y + (k + j') / n) with a hashed jitter j in the stratum (centred when supersampling is 1). Image row
 * 0 is the top of the tile (V = 1), as textures are stored. A sample falls in the first triangle that covers it, so
 * shared edges and overlapping UVs never count a sample twice. Triangles are assigned to the UDIM tile of their UV
 * centroid; tiles outside U 0-9, V 0+ are skipped.
 */
class LILA_SCENE_API TexelRaster
{
public:
    /** material >= 0 rasters only that material slot's triangles, so overlapping UVs of other texture sets don't claim its texels. */
    TexelRaster(const LowInput& low, const RasterSettings& settings, int material = -1);

    /** UDIM numbers with at least one triangle, ascending. */
    const std::vector<int>& udims() const { return mUdims; }
    int samplesPerTexel() const { return mGrid * mGrid; }
    int gridSize() const { return mGrid; }
    size_t bandCount() const { return mBandCount; }
    const RasterSettings& settings() const { return mSettings; }
    /** UDIM of each low triangle, 0 for skipped ones. */
    const std::vector<int>& triangleUdims() const { return mTriUdim; }
    /** Triangles skipped for lying outside the UDIM range or having no area in UV. */
    uint32_t skippedTriangles() const { return mSkipped; }

    /**
     * Appends the samples of one band of a tile (rows band * bandRows onwards) to out, with origin, direction and
     * rayLength set from the cage. Thread safe.
     */
    void band(int udim, size_t band, const CageParams& cage, std::vector<TexelSample>& out) const;
    /** A band's triangles in a tile, ascending: a sample belongs to the first of them that covers it. */
    const std::vector<uint32_t>& bandTriangles(int udim, size_t band) const;

private:
    struct Tile
    {
        int udim = 1001;
        int u = 0;
        int v = 0;
        std::vector<std::vector<uint32_t>> bands;
    };

    const LowInput& mLow;
    RasterSettings mSettings;
    int mGrid = 1;
    size_t mBandCount = 0;
    uint32_t mSkipped = 0;
    std::vector<Tile> mTiles;
    std::vector<int> mUdims;
    std::vector<int> mTriUdim;
};
} // namespace lila::bake
