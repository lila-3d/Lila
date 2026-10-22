#pragma once

namespace lila::ui
{
/**
 * Joint drawing and picking in every viewport, the joint tool, and the "rig.*" actions (joint tool, orient, mirror,
 * bind, paint weights, constraints). Call once after the built-in actions are registered.
 */
void registerRigViewport();
} // namespace lila::ui
