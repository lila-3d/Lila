#pragma once

#include "core/Export.h"

#include <QString>

#include <vector>

class QWidget;

namespace lila::ui::presenter
{
/**
 * Presenter mode, for streams and tutorials: the keys pressed, the actions run (with the menu path and hotkey), commands
 * from drags and panels, the tool and mode, captions and a cursor highlight, drawn over Lila's window. It ignores the
 * mouse unless its layout is being edited, when each block can be dragged, scaled and hidden.
 */
LILA_UI_API void install(QWidget* main);

LILA_UI_API bool enabled();
LILA_UI_API void setEnabled(bool on);
LILA_UI_API bool editing();
LILA_UI_API void setEditing(bool on);

/** Shows a caption for a while; empty clears it. */
LILA_UI_API void showCaption(const QString& text);

/** The action cards showing now, oldest first, as "title|detail|key". */
LILA_UI_API std::vector<QString> cards();
} // namespace lila::ui::presenter
