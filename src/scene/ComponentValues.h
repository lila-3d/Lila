#pragma once

#include "core/Export.h"

namespace lila::scene::componentValues
{
/**
 * componentValues and setComponentValues: per-vertex columns (skin weights per influence, position, colour) of the
 * selected or given components, read and written as the Component Spreadsheet does.
 */
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::componentValues
