#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Joint drawing and picking in every viewport, the joint tool, and the "rig.*" actions (joint tool, orient, mirror,
 * bind, paint weights, constraints). Call once after the built-in actions are registered.
 */
LILA_UI_API void registerRigViewport();

/** general prefs: the selected joint's colour (sRGB "#rrggbb"), and how sharply its ancestors step away from it (1 normal). */
inline constexpr const char* kJointSelectedColor = "/rig/jointSelectedColor";
inline constexpr const char* kJointDefaultSelectedColor = "#47ff85";
inline constexpr const char* kJointChainSeparation = "/rig/jointChainSeparation";
/** general pref: an ancestor's bones to children off the selected chain take the chain colour too. Off by default. */
inline constexpr const char* kJointChainBranches = "/rig/jointChainBranches";
} // namespace lila::ui
