#pragma once

#include "core/Export.h"

namespace lila::render
{
struct FrameState;
}

namespace lila::ui
{
/**
 * Shows debug drawing (debug/DebugDraw.h) in every viewport, redraws them when it changes or expires, and adds
 * Show > Debug Drawing and Clear Debug Drawing. Main thread, once.
 */
LILA_UI_API void registerDebugDrawUi();

/** The general pref debugDraw/show (on by default). */
LILA_UI_API bool debugDrawShown();

/** Adds the debug lines and triangles to a frame drawn outside a viewport (captures); points and text need a view's painter. */
LILA_UI_API void addDebugDraw(render::FrameState& s);
} // namespace lila::ui
