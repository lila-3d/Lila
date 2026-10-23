#pragma once

#include "core/Export.h"

class QMainWindow;

namespace lila::ui::progressui
{
/**
 * Shows progress::Scope roots: a long operation on the main thread (scene load, Maya conversion, save) as a card over
 * `window` once it has run for 300 ms, pumping events meanwhile with input blocked so Windows never flags Lila as hung;
 * background ones (autosave, exports on other threads) as a small indicator in the status bar.
 */
LILA_UI_API void install(QMainWindow* window);
} // namespace lila::ui::progressui
