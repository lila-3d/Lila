#pragma once

#include "core/Export.h"
#include "curve/Sweep.h"

/** Carved channels: cross-sections of trenches, grooves and gutters, and the closed cutter swept along a path. */
namespace lila::curve
{
enum class ChannelShape : uint8_t
{
    UChannel,  // straight walls, rounded bottom corners
    VGroove,   // two walls meeting at the floor
    HalfRound, // a half circle (an ellipse when depth isn't half the width)
    Stepped,   // terraces down to a narrow floor
};
LILA_SCENE_API const char* channelShapeName(ChannelShape s);

struct ChannelOptions
{
    ChannelShape shape = ChannelShape::UChannel;
    /** Width at the surface and depth below it, cm. */
    double width = 100.0;
    double depth = 50.0;
    /** U channel: radius of the bottom corners as a fraction of half the width (0 square, 1 a round floor). */
    double cornerRound = 0.25;
    /** Stepped: terraces from the rim to the floor. */
    int steps = 3;
    /** Stepped: floor width as a fraction of the width. */
    double floorWidth = 0.3;
    /** Points on each rounded part. */
    int segments = 8;
    /** How far the cutter rises above the path, cm (0: the larger of the depth and half the width), so it clears surfaces that slope across it. */
    double lift = 0.0;
    /** Rounds the path's sharp corners with arcs this wide, cm (0 leaves them mitred). */
    double bendRadius = 0.0;
    /** Up at the path: the channel's floor is below the path along it. */
    glm::dvec3 up{0.0, 1.0, 0.0};
    TessOptions tess{.mode = TessMode::Game};
    /** UVs of the channel's walls and floor (where the cutter's surface is left behind). */
    StripUv uv;
};

/**
 * The channel's cross-section from the left rim to the right rim, (across, up) in cm with the surface at up = 0: an open
 * polyline whose first and last points sit on the surface.
 */
LILA_SCENE_API std::vector<glm::dvec2> channelSection(const ChannelOptions& o);

/** The closed cutter: the section closed above the surface by the lift, swept along the path (world space in and out). */
LILA_SCENE_API geo::Mesh channelCutter(const Spline& path, const ChannelOptions& o);
} // namespace lila::curve
