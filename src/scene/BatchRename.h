#pragma once

#include "core/Export.h"
#include "scene/Naming.h"

#include <string>
#include <string_view>

namespace lila::scene
{
/**
 * One batch rename, applied to each name's short part (after the namespace) in this order: name, find/replace, strip,
 * case, prefix and suffix. In the name, replacement, prefix and suffix, a run of '#' is the object's number padded to the run's
 * length, {name} the short name before any rule, {parent} the parent's short name and {type} the node type.
 */
struct BatchRename
{
    std::string name; // replaces the whole name when set, as "tail_##" names a chain tail_01, tail_02...
    std::string find;
    std::string replace;
    bool regex = false;
    bool matchCase = false;
    int stripStart = 0;      // characters dropped from the front
    int stripEnd = 0;        // characters dropped from the back, after trailing digits when stripDigits
    bool stripDigits = false; // drop the trailing number, and a separator left before it
    std::string caseStyle;   // "", "PascalCase", "camelCase", "snake_case", "UPPER_SNAKE", "lower", "UPPER"
    Underscores underscores; // which underscores PascalCase and camelCase keep; batchRename follows the naming prefs
    std::string prefix;
    std::string suffix;
    int start = 1; // the number '#' gives the first object
    int step = 1;
};

/** What the rule set makes of one name; throws std::regex_error on a bad pattern. Leaves the result unsanitised. */
LILA_SCENE_API std::string batchRenamed(const BatchRename& r, std::string_view shortName, int index, std::string_view parent, std::string_view type);

/** A node name made from any text: invalid characters become '_' and a leading digit gets one in front. */
LILA_SCENE_API std::string sanitizeName(std::string_view s);

void registerBatchRenameCommands();
} // namespace lila::scene
