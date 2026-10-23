#pragma once

#include "core/Export.h"

namespace lila::ui
{
/** Space switching actions (Rig > Spaces) and the attribute editor's Spaces section. */
LILA_UI_API void registerSpacesUi();
/** Fills the Rig menu's Spaces submenu; the main window calls it once the Rig menu exists. */
LILA_UI_API void setupSpacesMenu();
} // namespace lila::ui
