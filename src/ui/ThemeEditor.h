#pragma once

#include "core/Export.h"

class QWidget;

namespace lila::ui
{
/**
 * Preferences > Interface > Theme: pick the theme (live), edit a saved theme's tokens with colour pickers (live
 * preview), Duplicate / Rename / Delete saved themes, Import from a file or pasted text, Export to a file or copy as text.
 */
LILA_UI_API QWidget* makeThemeEditor(QWidget* parent = nullptr);
} // namespace lila::ui
