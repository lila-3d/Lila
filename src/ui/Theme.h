#pragma once

#include "core/Export.h"

class QApplication;
class QFont;

namespace lila::ui
{
LILA_UI_API void applyTheme(QApplication& app);
/** Code/output font from prefs (default JetBrains Mono). */
LILA_UI_API QFont monoFont();
} // namespace lila::ui
