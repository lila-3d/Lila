#pragma once

#include "core/Export.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class QCompleter;
class QLineEdit;

namespace lila::ui::completion
{
/** The options holding typed, ignoring case: prefix matches first, then the rest, each in the options' order. */
LILA_UI_API std::vector<std::string> rank(const std::vector<std::string>& options, std::string_view typed);
LILA_UI_API QStringList rank(const QStringList& options, const QString& typed);

/** Completions for the token being typed, each a whole replacement for it. Empty for none. */
using Source = std::function<QStringList(const QString& token)>;

/**
 * Pops a list under edit as the user types, completing the token at the cursor (text between separators). Up and
 * Down pick, Tab or Enter accepts (Tab takes the first when none is picked), Esc dismisses. A completion ending in
 * ':' is a filter key, so its values are offered next.
 */
LILA_UI_API QCompleter* attach(QLineEdit* edit, Source source, const QString& separators = " ,");

/** attach() offering values() for the whole text, or for each comma separated item when list is true. */
LILA_UI_API QCompleter* attachValues(QLineEdit* edit, std::function<QStringList()> values, bool list = false);

/** attach() for an outliner style scene filter: completes `type:` and the node type names after it. */
LILA_UI_API QCompleter* attachSceneFilter(QLineEdit* edit);
} // namespace lila::ui::completion
