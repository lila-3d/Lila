#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <functional>
#include <string>
#include <string_view>

namespace lila::ext
{
class Context;
}

namespace lila::scene::anim
{
struct Curve;

/**
 * A custom interpolation type for animation curves (Maya's MPxAnimCurveInterpolator). A key whose `interpolator`
 * names one hands the segment from that key to the next to it instead of the tangent types; an unknown name falls back
 * to the tangents and is kept, so a scene saved with an extension still loads without it.
 */
struct Interpolator
{
    std::string name; // what keys store, "smoothStep"
    std::string doc;
    /**
     * The value at time t in the segment from curve.keys[segment] to curve.keys[segment + 1], with the first key's time
     * <= t < the second's. Called on worker threads, so it must only read its arguments.
     */
    std::function<double(const Curve& curve, size_t segment, double t)> evaluate;
};

LILA_SCENE_API Registry<Interpolator>& interpolators();
/** Throws std::invalid_argument when the name is taken. */
LILA_SCENE_API const Interpolator& registerInterpolator(Interpolator ip);
/** For extensions: owned by ctx and removed when it unloads. Null, with a log message, if the name is taken. */
LILA_SCENE_API const Interpolator* registerInterpolator(ext::Context& ctx, Interpolator ip);
LILA_SCENE_API const Interpolator* findInterpolator(std::string_view name);
} // namespace lila::scene::anim
