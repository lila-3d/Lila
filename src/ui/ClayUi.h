#pragma once

#include "core/Export.h"

namespace lila::ui
{
/** The clay actions: create, add shapes and cuts, group, refresh, convert. Mesh > Clay lists them. */
LILA_UI_API void registerClayUi();
/** The Clay tool: click shapes and cuts onto the clay, or draw capsule strokes over it. */
void registerClayTool();
} // namespace lila::ui
