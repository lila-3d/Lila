#pragma once

#include "core/Export.h"

#include <glm/vec3.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Node;
using json = nlohmann::json;
}

namespace lila::scene::anim
{
struct Curve;
struct RotationCurve;

enum class ModifierType : uint8_t
{
    Noise,
    Stepped,
    Limits,
};

LILA_SCENE_API const char* modifierTypeName(ModifierType t);
/** Throws std::invalid_argument for an unknown name. */
LILA_SCENE_API ModifierType modifierTypeFromName(std::string_view name);

/**
 * A non-destructive curve modifier: the keys stay as keyed and the curve's output changes. Stepped holds the value
 * over each step (a time remap, so it applies first); noise and limits then change the value in stack order.
 */
struct Modifier
{
    ModifierType type = ModifierType::Noise;
    // Noise: smooth gradient noise, amplitude in the curve's units, period in seconds per wave.
    double amplitude = 1.0;
    double period = 0.5;
    double phase = 0.0; // seconds
    int seed = 0;
    int octaves = 1;
    // Stepped: holds every `step` seconds, starting at `offset`.
    double step = 1.0 / 12.0;
    double offset = 0.0;
    // Limits
    std::optional<double> min;
    std::optional<double> max;

    bool operator==(const Modifier&) const = default;
};

/** Throws std::invalid_argument on malformed input. */
LILA_SCENE_API Modifier modifierFromJson(const json& j);
LILA_SCENE_API json modifierToJson(const Modifier& m);
/** The stack from a curve node's modifiers attribute; malformed text reads as none. */
LILA_SCENE_API std::vector<Modifier> modifiersFromText(const std::string& text);
LILA_SCENE_API std::string modifiersToText(const std::vector<Modifier>& mods);
/** The stack on a scalar or rotation curve node. */
LILA_SCENE_API std::vector<Modifier> modifiersOf(const Node& curve);

/** The time the keys are sampled at: stepped modifiers hold it. */
LILA_SCENE_API double modifiedTime(const std::vector<Modifier>& mods, double t);
/** Noise and limits applied to v at t. channel decorrelates the noise of a rotation curve's three axes. */
LILA_SCENE_API double modifiedValue(const std::vector<Modifier>& mods, double t, double v, int channel = 0);

LILA_SCENE_API double evaluateModified(const Curve& c, const std::vector<Modifier>& mods, double t);
/** Euler degrees, each axis modified on its own. */
LILA_SCENE_API glm::dvec3 evaluateModified(const RotationCurve& c, const std::vector<Modifier>& mods, double t);
} // namespace lila::scene::anim
