#pragma once

#include "core/Export.h"

#include "ui/editor/EditorTypes.h"
#include "ui/editor/PythonHighlighter.h"

#include <QColor>
#include <QHash>
#include <QString>

class QFont;

/**
 * HTML for the script editor's doc popups (hover, signature help, completion docs, gutter diagnostics), in Qt's rich
 * text subset. Pure functions of their inputs and a Theme, so they are testable without widgets.
 */
namespace lila::ui::doc
{
struct Theme
{
    QColor background, text, dim, border, codeBackground;
    QColor error, warning, info;
    QString monoFamily;
    /** Width of a space in the code font, for indentation in px. */
    int charWidth = 7;
    SyntaxFormats syntax;
};

/** The current editor look's colours, with `mono` as the code font. */
LILA_UI_API Theme currentTheme(const QFont& mono);

/**
 * Marks a table that should span the popup. DocPopup measures the content without it, so a full-width strip never
 * forces the popup to its maximum width.
 */
inline constexpr const char* kFullWidth = " width=\"100%\"";

/** Python source as spans in the syntax colours; [hlStart, hlEnd) is emphasised (the active parameter). */
LILA_UI_API QString codeHtml(const QString& code, const Theme& theme, int hlStart = -1, int hlEnd = -1);

/** A code block: the editor's background behind syntax-coloured, wrapping monospace text. */
LILA_UI_API QString codeBlockHtml(const QString& code, const Theme& theme, int hlStart = -1, int hlEnd = -1);

/** Parameter name to its annotation without the `| None = None` every command flag carries, from a signature. */
LILA_UI_API QHash<QString, QString> parameterAnnotations(const QString& signature);

/**
 * A Google-style docstring: paragraphs keep their indentation, an Args section becomes a list of names with their
 * annotation (from `annotations`) and description, Examples become code, and `backticks` become inline code.
 */
LILA_UI_API QString docstringHtml(const QString& docstring, const Theme& theme, const QHash<QString, QString>& annotations = {});

/** One strip per diagnostic, tinted by severity. */
LILA_UI_API QString diagnosticsHtml(const QList<Diagnostic>& diagnostics, const Theme& theme);

/**
 * A hover: diagnostics first, then `info`, which is a signature, a blank line and a docstring (the language server's
 * plaintext hover or the interpreter's description). A leading "(function)" style tag is dimmed.
 */
LILA_UI_API QString hoverHtml(const QList<Diagnostic>& diagnostics, const QString& info, const Theme& theme);
} // namespace lila::ui::doc
