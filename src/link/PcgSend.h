#pragma once

#include "core/Export.h"
#include "link/RpcClient.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace lila::link
{
/** A PCG point in Lila space: transform (position, rotation, scale), local bounds, density and per-point attributes. */
struct PcgPoint
{
    glm::dmat4 world{1.0};
    glm::dvec3 boundsMin{-50.0};
    glm::dvec3 boundsMax{50.0};
    double density = 1.0;
    /** Numbers, bools, strings, and [x, y, z] arrays (vectors, taken as Lila-space directions). */
    json attributes = json::object();
};

/** One tagged entry of a PCG data asset: points, a spline (open or closed polyline) or a polygon, in Lila space. */
struct PcgSet
{
    enum class Kind
    {
        Points,
        Spline,
        Polygon,
    };
    Kind kind = Kind::Points;
    /** Output pin name in Unreal's Load Data Asset node. */
    std::string pin;
    std::string tag;
    std::vector<PcgPoint> points;
    /** Spline and polygon vertices. */
    std::vector<glm::dvec3> path;
    bool closed = false;
    /** Attributes on the data itself (numbers, bools, strings). */
    json attributes = json::object();
};

/** setPcgData params: positions, rotations, bounds and vector attributes converted to Unreal space. */
LILA_SCENE_API json pcgDataParams(const std::string& asset, const std::string& label, const std::vector<PcgSet>& sets);

/**
 * Writes the sets to a PCG data asset (a package path such as /Game/LilaLink/Seams/PCG_Seams) through the LilaLinkPCG
 * plugin. Throws std::runtime_error when Unreal lacks the "pcgData" feature or the call fails.
 */
LILA_SCENE_API json sendPcgData(RpcClient& client, const json& helloFeatures, const std::string& asset, const std::string& label,
                                const std::vector<PcgSet>& sets);
} // namespace lila::link
