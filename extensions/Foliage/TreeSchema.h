#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

/**
 * The Tree Generator's parameter schema, built-in presets, palettes and the settings maths that needs no scene.
 *
 * Settings are one flat json object keyed "section.name" ("trunk.height"), ranges as two element arrays. Lengths are
 * held in metres and converted to Lila's centimetres on the way into the build, so presets read the same whatever the
 * display units.
 */
namespace treegen
{
using json = nlohmann::json;

inline constexpr const char* kVersion = "1.0";
/** Lila's linear unit is the centimetre. */
inline constexpr double kUnitsPerMetre = 100.0;
inline constexpr double kTrunkHeightMetres = 6.0;
inline constexpr double kTrunkRadiusMetres = 0.22;

enum class Kind
{
    Enum,
    Slider,
    Length,
    Float,
    Int,
    Range,
    LengthRange,
    Bool,
    String,
    Folder,
};

struct Field
{
    std::string section;
    std::string name;
    std::string label;
    Kind kind = Kind::Float;
    json defaultValue;
    std::vector<std::string> options; // Enum
    double sliderMin = 0.0, sliderMax = 1.0; // Slider: the slider's span
    double fieldMin = 0.0, fieldMax = 1.0;   // Slider: what a typed value is clamped to

    std::string key() const { return section + "." + name; }
};

/** Section ids in UI order: global, trunk, blob, leaf, plane, export. */
const std::vector<std::string>& sections();
const std::vector<Field>& fields();
/** Null when key is not in the schema. */
const Field* field(const std::string& key);

/** Every parameter at its default, lengths in metres. */
json defaults();
/** A stored value forced back into the type the schema expects; the default when it cannot be. */
json coerce(const std::string& key, const json& value);
/** Partial or stale settings merged onto the defaults. Unknown keys are dropped. */
json sanitize(const json& values);

/** Built-in preset names in menu order, "Default" first. */
const std::vector<std::string>& builtInPresets();
bool isBuiltIn(const std::string& name);
/** The partial settings a built-in preset lists; empty for an unknown name. */
const json& presetOverrides(const std::string& name);
/** Full settings for a preset, user presets ({name: settings}) first. */
json presetSettings(const std::string& name, const json& userPresets = json::object());
/** Built-ins, then the user presets that do not shadow one, sorted. */
std::vector<std::string> presetNames(const json& userPresets = json::object());

/** A fattened copy for chunkier trees off the same preset; amount 0 returns the values unchanged. */
json stylize(const json& values, double amount);

/** One section's values with lengths converted to centimetres, keyed by the bare field name. */
json sectionValues(const json& values, const std::string& section);

/** {"leaves": {season: tones}, "bark": tones} for a species, the default species for an unknown one. */
json paletteFor(const std::string& species);
/** "kaede" from "Kaede Mature": the first word of a preset name, lower case. */
std::string speciesOf(const std::string& preset);
} // namespace treegen
