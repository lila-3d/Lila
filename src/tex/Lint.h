#pragma once

#include "core/Export.h"

#include "tex/Graph.h"
#include "tex/Library.h"
#include "tex/TextureSet.h"

#include <string>
#include <vector>

/**
 * Physical validation of a material: its channels evaluated small and checked against measured values. Checks:
 *   albedo     dielectric base colour within 30 to 240 sRGB (stylised and emissive materials relaxed)
 *   blackWhite no pure black or pure white base colour
 *   metal      metallic texels bright enough, and near the reflectance of the metal the meta names (iron, gold...)
 *   metallic   metallic is 0 or 1 away from the transitions between metal and non-metal
 *   roughness  within 0 to 1, and not a near-perfect mirror over most of the surface unless the meta says glossy
 *   normal     unit length, pointing out, green up (OpenGL, as graphs make them)
 *   height     within 0 to 1
 *   seams      tileable materials match across their edges
 */
namespace lila::tex::lint
{
struct Check
{
    std::string id;
    /** pass, warn, fail or skip. */
    std::string status = "pass";
    std::string message;
    json data = json::object();
};

struct LILA_SCENE_API Report
{
    std::vector<Check> checks;
    std::string backend;

    bool passed() const;
    int count(const std::string& status) const;
    const Check* check(const std::string& id) const;
    json toJson() const;
};

struct Options
{
    int size = 256;
    /** gpu or cpu; empty picks the GPU when there is one. */
    std::string backend;
};

/** Evaluates the graph's channels and checks them. Throws std::invalid_argument when the graph can't be evaluated. */
LILA_SCENE_API Report run(const Graph& g, const SetSettings& s, const library::Meta& m, const Options& o = {});

/**
 * Clamps what failed or warned with Levels nodes named LintFix_<channel> on the channel's link into the Material Output
 * (or into the Material node feeding it), or by clamping an unlinked value. Returns what it changed, one line each.
 */
LILA_SCENE_API std::vector<std::string> fix(Graph& g, const Report& r, const library::Meta& m);

/** sRGB encoding of a linear value, 0 to 1. */
LILA_SCENE_API double toSrgb(double linear);
LILA_SCENE_API double toLinear(double srgb);
} // namespace lila::tex::lint
