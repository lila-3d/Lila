#pragma once

#include "core/Export.h"

class QWidget;

namespace lila::ui
{
inline constexpr const char* kPoseLibraryDock = "poseLibrary";

/**
 * The Pose Library panel: thumbnails of the current character's poses across the scene, workspace, user and template
 * tiers, with search, a blend weight, save, rename and delete. Double-click applies; Ctrl+double-click applies mirrored.
 */
LILA_UI_API QWidget* createPoseLibraryPanel(QWidget* parent);

/** window.poseLibrary and pose.mirror, plus the viewport thumbnail savePose uses. Call once the QApplication exists. */
LILA_UI_API void registerPoseLibraryUi();
} // namespace lila::ui
