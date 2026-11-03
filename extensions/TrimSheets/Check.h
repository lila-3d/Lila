#pragma once

#include "TrimSheet.h"

#include <string>
#include <vector>

/** Checking trim-mapped meshes against their sheet: density, alignment, orientation, stretch, seams, mips, readability, repetition. */
namespace trims
{
struct CheckOptions
{
    std::string uvSet = "uv0";
    /** Mesh to world, so sizes are world cm. */
    glm::dmat4 world{1.0};
    /** Texel density islands should have, px per metre; zero is the sheet's own. */
    double targetPxPerM = 0.0;
    /** Allowed density error, in doublings (0.25 is about +-19%). */
    double densityTolerance = 0.25;
    /** Allowed anisotropic stretch: the larger uv scale over the smaller, minus one. */
    double maxStretch = 0.1;
    /** Allowed skew off the nearest quarter turn, degrees. */
    double maxSkew = 3.0;
    /** Gameplay viewing distance (cm), screen height (px) and vertical field of view (degrees) for readability. */
    double distanceCm = 1000.0;
    double screenHeightPx = 1080.0;
    double fovDeg = 60.0;
    /** A trim's smallest feature (its bevel or detail) should cover at least this many screen px. */
    double minFeaturePx = 2.0;
};

struct IslandCheck
{
    std::vector<geo::Index> faces;
    /** Trim the island is on (from the face's trim attribute, else where its uvs lie), -1 for none. */
    int trim = -1;
    double pxPerM = 0.0;
    /** log2 of the island's density over the target. */
    double densityError = 0.0;
    /** How far the uvs run outside the trim's rectangle (across a tiling strip, both ways on others), px. */
    double outsidePx = 0.0;
    /** Smallest gap between the uvs and the trim's border on the clamped axes, px; negative when outside. */
    double marginPx = 0.0;
    /** Quarter turns from world up to +V (0 upright), -1 for flat islands whose up is free. */
    int quarterTurns = 0;
    double skewDeg = 0.0;
    bool mirrored = false;
    double stretch = 0.0;
    /** Distinct trims the island's triangles fall on; above one, a seam of the sheet runs through it. */
    int trimsCrossed = 0;
    /** The highest mip at which the trim's neighbours don't bleed into the island. */
    int cleanToMip = 0;
    /** The trim's smallest feature at the viewing distance, screen px. */
    double featurePx = 0.0;
    /** An adjacent island with the same trim, offset and turn, so the two read as one texture repeated; -1 for none. */
    int repeatsWith = -1;
    /** What is wrong: density, outside, crossesTrims, rotated, skewed, mirrored, stretched, mipBleed, unreadable, repeats, unmapped. */
    std::vector<std::string> issues;

    nlohmann::json toJson(const Sheet& sheet) const;
};

struct MeshCheck
{
    std::vector<IslandCheck> islands;
    double targetPxPerM = 0.0;

    /** Issue name to island count. */
    std::map<std::string, int> issueCounts() const;
    nlohmann::json toJson(const Sheet& sheet, bool all) const;
};

/** Checks the faces of mesh mapped onto sheet (those with a trim attribute, or every face when the mesh has none). */
MeshCheck checkMesh(const geo::Mesh& mesh, const Sheet& sheet, const CheckOptions& opts = {});
} // namespace trims
