#pragma once

#include "core/Export.h"

namespace lila::ui
{
/** Texture menu actions for exporting images and PSDs, importing image layers and sending a set to Unreal. */
LILA_UI_API void registerTexturePipelineUi();

/** The render_material_preview MCP tool (needs the renderer for its shaded view). */
LILA_UI_API void registerTexturePipelineMcpTools();
} // namespace lila::ui
