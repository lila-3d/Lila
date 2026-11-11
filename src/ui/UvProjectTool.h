#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

namespace lila::ui
{
/**
 * The uv projection manipulator: after a uvProject from the UI, a viewport tool session draws the projection's plane,
 * box, cylinder or sphere with handles for its centre, rotation, size, seam and uv scale and offset. Each drag re-runs
 * the projection through the last-op re-run, so the whole session stays one undo step. Enter, Esc, a click away from
 * the handles or another tool ends it. Also feeds uvProject the active view's direction for seam placement.
 */
LILA_UI_API void installUvProjectTool();

/**
 * uvSeamView flags for a seam view at the last perspective view's camera: a point there, or with plane, a plane through
 * it facing that camera. Empty before any view has drawn.
 */
LILA_UI_API nlohmann::json seamViewAtCamera(bool plane);
} // namespace lila::ui
