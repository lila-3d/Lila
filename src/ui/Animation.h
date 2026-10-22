#pragma once

#include <QObject>

#include <functional>

namespace lila::ui
{
/** Dock object names. */
inline constexpr const char* kTimeSliderDock = "timeSlider";
inline constexpr const char* kGraphEditorDock = "graphEditor";

/**
 * Registers the animation actions (window.graphEditor, window.timeSlider, anim.*) and installs the playback timer
 * as the scene's playback driver. Call once the QApplication exists.
 */
void registerAnimationUi();

/**
 * Calls fn on the main thread after the scene time changes, by playback, scrubbing or a script, for as long as
 * owner lives. Script changes are noticed within a frame or so.
 */
void onTimeChanged(QObject* owner, std::function<void()> fn);
/** Tells time listeners now instead of at the next poll. */
void notifyTimeChanged();
} // namespace lila::ui
