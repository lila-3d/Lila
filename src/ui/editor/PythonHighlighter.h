#pragma once

#include "core/Export.h"

#include <QSyntaxHighlighter>
#include <QTextBlockUserData>
#include <QTextCharFormat>

#include <array>
#include <vector>

namespace lila::ui
{
enum class TokenKind : uint8_t
{
    Keyword,
    Constant, // True, False, None
    Builtin,
    Self,
    DefName,
    ClassName,
    Decorator,
    String,
    StringEscape,
    FStringBrace,
    Number,
    Comment,
    CellMarker,
    Count,
};

struct Token
{
    int start = 0;
    int length = 0;
    TokenKind kind = TokenKind::Keyword;
};

/**
 * Tokenizes one line of Python. `state` carries an open triple-quoted string across lines: 0 is plain code, and the
 * return value is the state for the next line.
 */
LILA_UI_API int tokenizePython(QStringView line, int state, std::vector<Token>& out);

/** Per-block tokens, kept so bracket matching and completion can tell code from strings and comments. */
class LILA_UI_API PythonBlockData : public QTextBlockUserData
{
public:
    std::vector<Token> tokens;

    /** True when `col` sits inside a string or comment. */
    bool inStringOrComment(int col) const;
};

class LILA_UI_API PythonHighlighter : public QSyntaxHighlighter
{
public:
    explicit PythonHighlighter(QTextDocument* doc);

protected:
    void highlightBlock(const QString& text) override;

private:
    std::array<QTextCharFormat, size_t(TokenKind::Count)> mFormats;
};
} // namespace lila::ui
