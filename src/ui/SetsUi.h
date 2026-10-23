#pragma once

#include "core/Export.h"

class QMenu;
class QWidget;

namespace lila::ui::sets
{
/** "sets.create" (Create > Sets > Set) and "sets.createQuickSelect" (Create > Sets > Quick Select Set...). */
LILA_UI_API void registerActions(QWidget* dialogParent);

/** Fills the menu with the quick select sets each time it opens, one click selecting a set's members. */
LILA_UI_API void attachQuickSelectMenu(QMenu* menu);
} // namespace lila::ui::sets
