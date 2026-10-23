#pragma once

#include "core/Export.h"

class QWidget;

namespace lila::ui
{
inline constexpr const char* kControlPickerDock = "controlPicker";

/**
 * The Control Picker panel: a picker document's pages as tabs of clickable shapes that select (click, Shift toggles,
 * Ctrl removes, Ctrl+Shift adds, drag for a marquee), toggle, pose or run commands on a character picked from the
 * namespace list. Highlights follow the scene selection. Edit mode moves, resizes, adds, mirrors, aligns and recolours
 * items; every change is an editPicker call, so it undoes.
 */
LILA_UI_API QWidget* createControlPickerPanel(QWidget* parent);

/** The panel plus window.controlPicker, picker.editMode, picker.addFromSelection, picker.mirrorItems, picker.frame. */
LILA_UI_API void registerControlPickerUi();
} // namespace lila::ui
