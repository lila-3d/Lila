#pragma once

#include "core/Export.h"
#include "groom/Groom.h"

#include <string_view>
#include <vector>

/** Lock shapes drawn along a stroke (a lock, braid, rope, ringlet) and hairstyle presets placed on a scalp. */
namespace lila::groom
{
enum class LockShape : uint8_t
{
    Lock,
    Braid,   // three strands plaited
    Rope,    // two strands twisted
    Ringlet, // one lock wound into a spiral
    Count
};

LILA_SCENE_API const char* lockShapeName(LockShape s);
LILA_SCENE_API bool parseLockShape(std::string_view name, LockShape* out);

struct ShapeOptions
{
    LockShape shape = LockShape::Lock;
    /** Braid and rope: half width of the plait; ringlet: radius of the spiral, cm. */
    double size = 1.0;
    /** Length of one full turn of the plait or spiral, cm. */
    double period = 4.0;
    /** Points per cm along the stroke. */
    double density = 2.0;
    /** Width of each strand relative to the lock width (braid and rope). */
    double strandWidth = 0.5;
};

/**
 * Guides along a stroke (root first): one curve for a lock or ringlet, three for a braid, two for a rope. pressure is per
 * stroke point, 0 to 1, and becomes each guide's radius (width scale); empty for full width. up orients the plait at
 * the root (the scalp normal). Guides come without ids; strands of one plait share a clump id.
 */
LILA_SCENE_API Splines shapeAlong(const std::vector<D3>& stroke, const std::vector<double>& pressure, const D3& up, const ShapeOptions& o);

enum class Preset : uint8_t
{
    Ponytail,
    Bun,
    Bangs,
    Count
};

LILA_SCENE_API const char* presetName(Preset p);
LILA_SCENE_API bool parsePreset(std::string_view name, Preset* out);

struct PresetOptions
{
    Preset preset = Preset::Ponytail;
    /** Ponytail and bun: where the hair gathers; bangs: one end of the fringe. */
    D3 at{0.0};
    /** Bangs: the other end of the fringe. */
    D3 to{0.0};
    /** Which way is down (gravity). */
    D3 down{0.0, -1.0, 0.0};
    int count = 16;
    double length = 20.0;
    /** Ponytail: how far from the gather point roots spread over the scalp; bun: its radius, cm. */
    double radius = 6.0;
    uint32_t seed = 1;
};

/** Guides for a preset, roots on the scalp. Needs a scalp; empty without one. */
LILA_SCENE_API Splines preset(const Surface& scalp, const PresetOptions& o);
} // namespace lila::groom
