#pragma once

#include "core/Export.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace lila::ui::format
{
/** The script editor's formatting prefs, all in the "scriptEditor" file. */
namespace key
{
inline constexpr const char* onSave = "/formatOnSave";
inline constexpr const char* organizeImportsOnSave = "/format/organizeImportsOnSave";
inline constexpr const char* fixAllOnSave = "/format/fixAllOnSave";
inline constexpr const char* onPaste = "/format/onPaste";
inline constexpr const char* onType = "/format/onType";
inline constexpr const char* onFocusLoss = "/format/onFocusLoss";
inline constexpr const char* trimTrailingWhitespace = "/format/trimTrailingWhitespace";
inline constexpr const char* finalNewline = "/format/finalNewline";
inline constexpr const char* useProjectConfig = "/format/useProjectConfig";
inline constexpr const char* lineLength = "/format/lineLength";
inline constexpr const char* quoteStyle = "/format/quoteStyle";
inline constexpr const char* indentWidth = "/format/indentWidth";
} // namespace key

/**
 * Formatting behaviour. The defaults are the reference editors': nothing reformats code unless asked, while saving
 * trims trailing whitespace and ends the file with a newline, which never changes what the code does.
 */
struct Settings
{
    bool onSave = false;
    bool organizeImportsOnSave = false;
    bool fixAllOnSave = false;
    bool onPaste = false;
    bool onType = false;
    bool onFocusLoss = false;
    bool trimTrailingWhitespace = true;
    bool finalNewline = true;
    /** A ruff.toml, .ruff.toml or pyproject.toml with [tool.ruff] beside the file or above it wins over the style below. */
    bool useProjectConfig = true;
    int lineLength = 88;
    QString quoteStyle = "double";
    int indentWidth = 4;

    /** Read from the prefs on every call, so changes apply at once. */
    LILA_UI_API static Settings load();
};

/** The ruff config file governing `filePath`, found the way ruff finds it; empty when there is none. */
LILA_UI_API QString findProjectConfig(const QString& filePath);

/** Style overrides for ruff: none when a project config governs the file and the prefs defer to it. */
LILA_UI_API QStringList styleArgs(const Settings& s, bool hasProjectConfig, bool forCheck);

/** `--range=` for ruff, from 0-based lines and UTF-16 columns to its 1-based lines and code point columns. */
LILA_UI_API QString rangeArg(const QStringList& lines, int line, int col, int endLine, int endCol);

/** A replacement of before[start, end) with `text`. */
struct Edit
{
    int start = 0;
    int end = 0;
    QString text;
    bool operator==(const Edit&) const = default;
};

/** Edits turning `before` into `after`, ascending and minimal per changed run of lines, so untouched lines keep their place. */
LILA_UI_API std::vector<Edit> diff(const QString& before, const QString& after);

/** Where `pos` in `before` lands after the edits; inside an edit it stays after the same non-blank character. */
LILA_UI_API int mapPosition(int pos, const QString& before, const std::vector<Edit>& edits);

/** First and last line of a statement, 0-based. */
struct Statement
{
    int first = 0;
    int last = 0;
};

/** Lexical state at the start of a line. */
struct LineState
{
    int depth = 0;       // open brackets
    bool inString = false; // inside a triple-quoted string
    bool continued = false; // the previous line ended with a backslash
};

/** States at the start of each line, plus one for the end of the text. */
LILA_UI_API std::vector<LineState> scan(const QStringList& lines);

/** The complete statement ending on `line`: nothing when it is blank or still open (brackets, strings, backslash). */
LILA_UI_API std::optional<Statement> statementAt(const QStringList& lines, int line);

/** True when the statement is a block header whose body is not written yet, which would not parse on its own. */
LILA_UI_API bool needsPlaceholder(const QStringList& lines, Statement statement);

/** A body line for a header at `headerLine`'s indent; removePlaceholder() takes it out of ruff's output again. */
LILA_UI_API QString placeholderLine(const QString& headerLine);
LILA_UI_API QString removePlaceholder(const QString& text);

/** Trailing whitespace trimmed (not inside strings, not on `keepLine`) and a final newline ensured, as asked. */
LILA_UI_API QString cleanupForSave(const QString& text, bool trim, bool finalNewline, int keepLine = -1);

/** For `else:`, `elif ...:`, `except ...:` and `finally:` typed too deep, the indent of the block they continue. */
LILA_UI_API std::optional<int> keywordDedent(const QStringList& lines, int line);
} // namespace lila::ui::format
