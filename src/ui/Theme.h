#pragma once

class QApplication;
class QFont;

namespace lila::ui
{
void applyTheme(QApplication& app);
/** Code/output font from prefs (default JetBrains Mono). */
QFont monoFont();
} // namespace lila::ui
