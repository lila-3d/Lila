#pragma once

#include <nlohmann/json.hpp>

#include <QList>
#include <QString>

namespace lila::ui
{
/** LSP severities: 1 error, 2 warning, 3 information, 4 hint. */
enum class Severity : int
{
    Error = 1,
    Warning = 2,
    Info = 3,
    Hint = 4,
};

/** Replaces a range, in lines and UTF-16 columns, with `text`. */
struct TextEdit
{
    int line = 0;
    int col = 0;
    int endLine = 0;
    int endCol = 0;
    QString text;
};

/** A document range in lines and UTF-16 columns, which is what both QTextDocument and LSP count in. */
struct Diagnostic
{
    int line = 0;
    int col = 0;
    int endLine = 0;
    int endCol = 0;
    Severity severity = Severity::Warning;
    QString message;
    QString source;
    QString code;
    bool unnecessary = false; // rendered faded rather than underlined
    /** The quick fix offered for it, if any. */
    QString fixTitle;
    QList<TextEdit> fix;
    bool fixUnsafe = false;
};

/** LSP completion kinds, the subset that changes how a row is drawn. */
enum class CompletionKind : int
{
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Keyword = 14,
    Snippet = 15,
    Constant = 21,
};

struct CompletionEntry
{
    QString label;
    QString detail;
    QString documentation;
    QString insertText;
    QString sortText;
    CompletionKind kind = CompletionKind::Text;
    nlohmann::json lspItem; // kept for completionItem/resolve
    bool fromInterpreter = false;
    bool resolved = false;
};
} // namespace lila::ui
