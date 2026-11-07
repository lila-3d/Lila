#pragma once

#include "core/Export.h"

#include "scene/AnimCurve.h"
#include "scene/RotationCurve.h"

#include <array>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

namespace lila::scene::anim
{
/**
 * A move, scale or both of keys: new time = timePivot + (t - timePivot) * timeScale + timeOffset, and the same for
 * values. Falloff (seconds) makes unselected keys of the same curve follow the value change, fully at a selected key's
 * time and not at all `falloff` away.
 */
struct LILA_SCENE_API KeyTransform
{
    double timeOffset = 0.0;
    double timeScale = 1.0;
    double timePivot = 0.0;
    double valueOffset = 0.0;
    double valueScale = 1.0;
    double valuePivot = 0.0;
    double falloff = 0.0;
    /** Moved keys land on whole frames. */
    bool snap = false;

    double time(double t, double fps) const;
    /** weight 1 is the full change, 0 none. */
    double value(double v, double weight = 1.0) const;
};

/** Smooth falloff: 1 at distance 0, 0 at radius and beyond. */
LILA_SCENE_API double falloffWeight(double distance, double radius);

/**
 * Transforms the keys at `times` (and their falloff neighbours' values). Moved keys replace keys already at their new
 * time. Fixed tangents keep their shape: slopes scale by valueScale / timeScale.
 */
LILA_SCENE_API int transformKeys(Curve& c, const std::vector<double>& times, const KeyTransform& x, double fps);
/**
 * Per Euler axis of a rotation curve: the keys picked on each axis take that axis's value change; a key picked on any
 * axis moves in time (the time part of every axis's transform must match).
 */
LILA_SCENE_API int transformKeys(RotationCurve& rc, const std::array<std::vector<double>, 3>& times, const std::array<KeyTransform, 3>& x,
                                 double fps);

enum class BlendMode : uint8_t
{
    Neighbour, // towards the key before (amount < 0) or after (amount > 0) the picked run
    Ease, // onto an ease-in (< 0) or ease-out (> 0) curve between the run's neighbours
    PushPull, // away from (> 0) or onto (< 0) the straight line between the run's neighbours
    Smooth, // towards (> 0) or away from (< 0) the average of each key's neighbours
    Default, // towards (> 0) or away from (< 0) the channel's default value
};

LILA_SCENE_API const char* blendModeName(BlendMode m);
/** Throws std::invalid_argument for an unknown name. */
LILA_SCENE_API BlendMode blendModeFromName(std::string_view name);

/** New values for the picked keys (selected[i] != 0); others come back unchanged. amount runs -1..1. */
LILA_SCENE_API std::vector<double> blendValues(const std::vector<double>& times, const std::vector<double>& values,
                                               const std::vector<char>& selected, BlendMode mode, double amount, double defaultValue);
LILA_SCENE_API int blendKeys(Curve& c, const std::vector<double>& times, BlendMode mode, double amount, double defaultValue);
/** axis 0..2: one Euler axis. */
LILA_SCENE_API int blendKeys(RotationCurve& rc, int axis, const std::vector<double>& times, BlendMode mode, double amount, double defaultValue);

enum class Filter : uint8_t
{
    Butterworth, // zero-phase low-pass on per-frame samples; a key every frame
    Gaussian, // each key towards a Gaussian-weighted average of the keys around it; key count unchanged
    Simplify, // the fewest keys that stay within tolerance of the curve at every frame
    Resample, // a key every `step` frames
};

LILA_SCENE_API const char* filterName(Filter f);
/** Throws std::invalid_argument for an unknown name. */
LILA_SCENE_API Filter filterFromName(std::string_view name);

struct FilterSettings
{
    Filter filter = Filter::Butterworth;
    double cutoff = 7.0; // Hz, Butterworth
    double width = 1.0; // frames, Gaussian standard deviation
    double tolerance = 0.05; // curve units (degrees on rotations), Simplify
    double step = 1.0; // frames, Resample
};

/** Filters the curve between t0 and t1 (inclusive). Returns the key count after. */
LILA_SCENE_API int filterKeys(Curve& c, double t0, double t1, const FilterSettings& s, double fps);
/** axis 0..2 filters one Euler axis and keeps the others' motion; -1 filters all three. */
LILA_SCENE_API int filterKeys(RotationCurve& rc, int axis, double t0, double t1, const FilterSettings& s, double fps);

/** transformKeys, blendKeys, filterCurve, curveModifier. */
LILA_SCENE_API void registerKeyOpCommands();
} // namespace lila::scene::anim
