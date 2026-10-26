#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
/**
 * Heights over the XZ plane on a regular grid, Y up, in scene units. Sample (x, z) sits at origin + (x, z) * cell and is
 * stored at z * nx + x, so rows run along +X and advance along +Z. Masks are named values on the same grid (0 to 1 by
 * convention). Everything that generates heights works from world positions, so neighbouring heightfields match.
 */
struct LILA_SCENE_API Heightfield
{
    int nx = 0;
    int nz = 0;
    double cell = 100.0;
    glm::dvec2 origin{0.0};
    std::vector<float> heights;
    std::map<std::string, std::vector<float>, std::less<>> masks;

    size_t size() const { return size_t(nx) * size_t(nz); }
    bool empty() const { return nx < 2 || nz < 2; }
    size_t index(int x, int z) const { return size_t(z) * size_t(nx) + size_t(x); }
    float at(int x, int z) const { return heights[index(x, z)]; }
    glm::dvec2 samplePos(int x, int z) const { return origin + glm::dvec2(x, z) * cell; }
    glm::dvec3 position(int x, int z) const;
    /** Width and depth covered, sample centre to sample centre. */
    glm::dvec2 extent() const { return glm::dvec2(nx - 1, nz - 1) * cell; }
    bool contains(double wx, double wz) const;

    /** Bilinear height at a world XZ position, clamped to the edge. */
    double heightAt(double wx, double wz) const;
    /** Unit upward normal of the bilinear surface. */
    glm::dvec3 normalAt(double wx, double wz) const;
    /** Bilinear mask value, clamped to the edge; 0 when the mask doesn't exist. */
    double maskAt(std::string_view name, double wx, double wz) const;
    /** Lowest and highest height. */
    glm::dvec2 range() const;
    Bounds bounds() const;

    /** The mask's values, added as zeros when missing. */
    std::vector<float>& mask(std::string_view name);
    const std::vector<float>* findMask(std::string_view name) const;
};

/** nx by nz samples, cell apart, all at height h. */
LILA_SCENE_API Heightfield makeHeightfield(int nx, int nz, double cell, glm::dvec2 origin, float h = 0.0f);
/** Covering size (width along X, depth along Z) centred on center, with resolution samples along the longer side. */
LILA_SCENE_API Heightfield makeHeightfieldCovering(glm::dvec2 center, glm::dvec2 size, int resolution, float h = 0.0f);

struct HeightfieldMeshOptions
{
    /** Keep every step-th sample; the last row and column are always kept so the edges stay put. */
    int step = 1;
    /** Corner uv 0 to 1 over the whole grid. */
    bool uvs = true;
    /** Masks as point float attributes of the same name. */
    bool masks = true;
};

/** Quads facing +Y. */
LILA_SCENE_API Mesh heightfieldToMesh(const Heightfield& h, const HeightfieldMeshOptions& o = {});

/**
 * The highest surface of the mesh's faces above each sample, found by rasterizing them from above. Samples no face
 * covers get fill; covered (optional) receives how many were hit.
 */
LILA_SCENE_API Heightfield meshToHeightfield(const Mesh& m, int nx, int nz, double cell, glm::dvec2 origin, float fill,
                                             size_t* covered = nullptr);

/** Resampled bilinearly onto another grid, masks included; outside the source the edge values carry on. */
LILA_SCENE_API Heightfield resampled(const Heightfield& h, int nx, int nz, double cell, glm::dvec2 origin);
} // namespace lila::geo
