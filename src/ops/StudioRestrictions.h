#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace lila::ext
{
class Context;
}

/**
 * Studio restrictions: prefs a studio folder can lock with "$locked" (see PrefStore.h), named and explained so studio
 * admins can find them. Any pref is lockable, restriction or not; registering one adds the label and doc the
 * studioRestrictions command lists. Lila, C++ extensions and Python register alike. Main thread only.
 */
namespace lila::restrictions
{
struct Restriction
{
    std::string label; // "Extensions"
    std::string doc;   // what a studio gets by locking it
    std::string file = "general";
    std::string key; // JSON pointer: "/extensions/disabled"
};

LILA_FOUNDATION_API Registry<Restriction>& registry();

/** Adds or replaces id. Throws std::invalid_argument without a label or a key starting with '/'. */
LILA_FOUNDATION_API void add(std::string id, Restriction r, std::string owner = {});
/** For extensions: owned by ctx and removed when it unloads. False, with a log message, when id is taken or invalid. */
LILA_FOUNDATION_API bool add(ext::Context& ctx, std::string id, Restriction r);

/** Whether a studio folder locks id's pref; false for an unknown id. */
LILA_FOUNDATION_API bool locked(std::string_view id);
/** id's pref as it applies now (the studio's value when locked); null for an unknown id or an unset pref. */
LILA_FOUNDATION_API nlohmann::json value(std::string_view id);

/** {id, label, doc, file, key, owner, locked, value} per restriction, sorted by id. */
LILA_FOUNDATION_API nlohmann::json describe();

/** Lila's own restrictions and the studioRestrictions command. */
LILA_FOUNDATION_API void registerBuiltins();
} // namespace lila::restrictions
