#pragma once

#include "core/Export.h"

class QWidget;

namespace lila::ui
{
/** Help > Credits: sponsors and supporters by tier, then contributors and testers. One window, raised if already open. */
LILA_UI_API void showCredits(QWidget* parent);
} // namespace lila::ui
