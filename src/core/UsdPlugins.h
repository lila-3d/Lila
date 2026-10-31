#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include <filesystem>

namespace lila::ext
{
class Context;
}

/**
 * Folders OpenUSD searches for plugins, as it searches each PXR_PLUGINPATH_NAME entry: the folder's own plugInfo.json
 * and each subfolder's plugInfo.json or resources/plugInfo.json. Hydra render delegates found there join every
 * viewport's Renderer menu. An extension lists its folders under "usdPlugins" in extension.json, or adds them here.
 */
namespace lila::usdPlugins
{
LILA_FOUNDATION_API Registry<std::filesystem::path>& registry();
/** Owned by ctx and removed when it unloads. False, logged, when the folder doesn't exist. */
LILA_FOUNDATION_API bool add(ext::Context& ctx, const std::filesystem::path& folder);
} // namespace lila::usdPlugins
