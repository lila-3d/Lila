#pragma once

#include "core/Export.h"

#include "ops/Command.h"

#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

/**
 * Maya-style option boxes: the remembered settings of a command run from a menu, and the fields that edit them,
 * generated from the command's typed flags and docs. Values live in the "optionBoxes" pref file, one object per
 * command holding only the flags that differ from their defaults, so a changed registry default still applies.
 */
namespace lila::ops::options
{
enum class FieldKind : uint8_t
{
    Check,
    Int,
    Float,
    Choice,
    Text,
    Vec3,
    List, // comma separated text
};

struct Field
{
    std::string flag;
    std::string label;
    std::string section; // empty = the dialog's first, untitled group
    FieldKind kind = FieldKind::Text;
    /** What the command does when the flag is left out; for a flag with a null default, the kind's zero. */
    json defaultValue;
    std::vector<std::string> choices;
    std::vector<std::string> choiceLabels; // same size as choices
    std::string doc;
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    double step = std::numeric_limits<double>::quiet_NaN();
    std::string suffix; // " cm", " deg"
};

/** Per-command layout tweaks on top of the generated fields; every member is optional. */
struct Override
{
    std::vector<std::string> order;  // these flags first, in this order; the rest follow in registry order
    std::vector<std::string> hidden; // never shown, never remembered
    std::vector<std::string> shown;  // shown although the generator would skip them (List flags)
    std::vector<std::string> adjust; // Vec3 flags the adjust-last-op popup edits too (it shows scalars only otherwise)
    std::map<std::string, std::string> labels;
    std::map<std::string, std::string> sections;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> choices; // flag -> (value, label)
    struct Range
    {
        double min, max, step;
        std::string suffix;
    };
    std::map<std::string, Range> ranges;
};

LILA_FOUNDATION_API void setOverride(const std::string& command, Override o);
LILA_FOUNDATION_API const Override* findOverride(std::string_view command);

/** "maxDistance" reads "Max Distance", "uvSet" reads "UV Set". */
LILA_FOUNDATION_API std::string prettyLabel(std::string_view camel);

/**
 * Allowed values spelled out in a flag doc: "closestPoint | pointIndex", "world, object, component or uv.".
 * Empty unless every listed value is a single word and the default (when given) is one of them.
 */
LILA_FOUNDATION_API std::vector<std::string> choicesFromDoc(std::string_view doc, const json& defaultValue = nullptr);

/** The field a flag generates, before overrides. */
LILA_FOUNDATION_API Field fieldFor(const FlagDef& flag);

/** The editable fields of a command's option box, minus the flags `fixed` already sets (an action's own call). */
LILA_FOUNDATION_API std::vector<Field> fields(std::string_view command, const json& fixed = json::object());
LILA_FOUNDATION_API bool hasOptions(std::string_view command, const json& fixed = json::object());

/** The stored changes, {flag: value}. */
LILA_FOUNDATION_API json saved(std::string_view command);
/** Every field's current value: the stored one, else its default. */
LILA_FOUNDATION_API json values(std::string_view command, const json& fixed = json::object());

/** Stores one value; setting it back to the default forgets it. */
LILA_FOUNDATION_API void setValue(std::string_view command, const std::string& flag, const json& value);
/** Edit > Reset Settings. */
LILA_FOUNDATION_API void reset(std::string_view command);

/** `call` with the remembered values filled in under it; flags the call sets itself win. */
LILA_FOUNDATION_API json callWith(std::string_view command, const json& call = json::object());

inline constexpr const char* kPrefFile = "optionBoxes";
} // namespace lila::ops::options
