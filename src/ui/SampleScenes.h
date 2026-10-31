#pragma once

#include "io/Samples.h"

#include <functional>

class QWidget;

namespace lila::ui::sampleScenes
{
/**
 * Shows a sample's details (size, disk space, license, credit) over parent and, once confirmed, gets it ready to open,
 * downloading, unpacking and setting it up first with progress when it isn't installed yet. Setting up replaces the
 * current scene, so mayReplaceScene (when given) runs after the confirmation and can stop there. ready runs once the
 * scene exists; nothing runs if the user cancels or a step fails (failures are reported in a message box).
 */
void fetch(QWidget* parent, const io::samples::Sample& sample, std::function<bool()> mayReplaceScene, std::function<void()> ready);
} // namespace lila::ui::sampleScenes
