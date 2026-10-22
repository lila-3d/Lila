#pragma once

#include <glm/vec2.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace lila::scene::softSelect
{
using json = nlohmann::json;

/** General prefs keys. */
inline constexpr const char* kEnabled = "/softSelect/enabled";
inline constexpr const char* kRadius = "/softSelect/radius";
inline constexpr const char* kCurve = "/softSelect/curve";
inline constexpr const char* kFalloffMode = "/softSelect/falloffMode";
inline constexpr const char* kUserPresets = "/softSelect/userPresets";

/** How the segment that starts at a key reaches the next key. */
enum class Interp : uint8_t
{
    Linear,
    Smooth, // ease in and out, flat at both keys
    Spline, // monotone cubic through the neighbouring keys
    Flat,   // hold this key's value up to the next key
};

const char* interpName(Interp i);
Interp interpFromName(std::string_view name, Interp fallback = Interp::Spline);

struct Key
{
    double pos = 0.0;   // 0 at the selection, 1 at the radius
    double value = 0.0; // weight, 0..1
    Interp interp = Interp::Spline;

    bool operator==(const Key&) const = default;
};

/**
 * Maya's soft select falloff curve: weight against normalised distance, through keys with per-key interpolation.
 * Spline segments use Fritsch-Carlson tangents so they never overshoot their keys; the curve's ends have flat tangents.
 * Results are clamped to 0..1. With no keys it is the default smooth falloff.
 */
class Curve
{
public:
    Curve();
    explicit Curve(std::vector<Key> keys);

    double evaluate(double t) const;
    const std::vector<Key>& keys() const { return mKeys; }

    /** Index of the new key. */
    int addKey(Key k);
    void removeKey(int index);
    /** Moves a key, keeping the keys sorted; returns its new index. */
    int moveKey(int index, double pos, double value);
    void setInterp(int index, Interp i);

    /** [[pos, value, "interp"], ...]. */
    json toJson() const;
    /** Accepts the toJson form or [{pos, value, interp}, ...]; anything unusable gives the default curve. */
    static Curve fromJson(const json& j);

    bool operator==(const Curve& o) const { return mKeys == o.mKeys; }

private:
    void sortAndClamp();
    void updateTangents();

    std::vector<Key> mKeys;
    std::vector<double> mTangents;
};

/** Ramer-Douglas-Peucker: keeps the endpoints and every point further than epsilon from the simplified polyline. */
std::vector<glm::dvec2> simplify(const std::vector<glm::dvec2>& points, double epsilon);

/**
 * A freehand stroke (x = distance, y = weight, both 0..1) as spline keys: points that run backwards in x are dropped,
 * the stroke is extended to x = 0 and 1, then simplified.
 */
Curve curveFromStroke(const std::vector<glm::dvec2>& stroke, double epsilon = 0.02);

struct Preset
{
    std::string name;
    Curve curve;
};

/** Built-in presets; the first is the default curve. */
const std::vector<Preset>& builtinPresets();
const Preset* findBuiltin(std::string_view name);

/** The curve in prefs. */
Curve currentCurve();
void setCurrentCurve(const Curve& c);

/** Saved user presets, in order. */
std::vector<Preset> userPresets();
/** Adds or replaces the named preset. */
void saveUserPreset(const std::string& name, const Curve& c);
bool deleteUserPreset(const std::string& name);
bool renameUserPreset(const std::string& from, const std::string& to);

/** True when falloff distances are measured along the surface rather than straight through space. */
bool surfaceFalloff();

void registerCommands();
} // namespace lila::scene::softSelect
