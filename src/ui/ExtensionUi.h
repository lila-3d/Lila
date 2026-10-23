#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>

/** How extensions reach the UI registries beyond what they register through their ext::Context. */
namespace lila::ui::extensionUi
{
/**
 * Loads the resources of every extension already loaded and of each one that loads later, and removes everything an
 * extension owns in the UI registries when it unloads (Python registrations included, which no context tracks).
 */
LILA_UI_API void install();

/**
 * Registers what <root>/resources holds, owned by owner: markingMenus/**.json ("set/name.json" goes in a set),
 * shelves/*.json ({"items": [...]}), layouts/*.json and hotkeys/*.json ({"contexts": {...}, "mode": optional}).
 * Returns how many it registered.
 */
LILA_UI_API int loadResources(const std::string& owner, const std::filesystem::path& root);

/** Removes everything owner registered, in every registry (see owners::removeOwned). Returns the count. */
LILA_UI_API size_t removeOwned(const std::string& owner);
} // namespace lila::ui::extensionUi
