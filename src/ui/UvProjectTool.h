#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * The uv projection manipulator: after a uvProject from the UI, a viewport tool session draws the projection's plane,
 * box, cylinder or sphere with handles for its centre, rotation, size, seam and uv scale and offset. Each drag re-runs
 * the projection through the last-op re-run, so the whole session stays one undo step. Enter, Esc, a click away from
 * the handles or another tool ends it. Also feeds uvProject the active view's direction for seam placement.
 */
LILA_UI_API void installUvProjectTool();
} // namespace lila::ui
