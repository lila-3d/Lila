#pragma once

#include "core/Export.h"

#include <QHash>
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
    Function,     // a call, or a function the language server names
    Variable,     // any other name, attributes included
    Parameter,    // def and lambda parameters, keyword arguments in calls
    Module,       // names in import statements
    ConstantName, // ALL_CAPS names
    Count,
};

struct Token
{
    int start = 0;
    int length = 0;
    TokenKind kind = TokenKind::Keyword;

    bool operator==(const Token&) const = default;
};

/**
 * Tokenizes one line of Python. `state` carries an open triple-quoted string and the open brackets across lines: 0 is
 * plain code, and the return value is the state for the next line.
 */
LILA_UI_API int tokenizePython(QStringView line, int state, std::vector<Token>& out);

/** Character formats per token kind for the current palette look; the editor and its doc popups share them. */
struct LILA_UI_API SyntaxFormats
{
    std::array<QTextCharFormat, size_t(TokenKind::Count)> tokens;
    /** Colour look only: control-flow keywords and builtin types, which monotone leaves as Keyword and Builtin. */
    QTextCharFormat control, builtinType;

    /** The format for token `t` of `line`. */
    const QTextCharFormat& forToken(QStringView line, const Token& t) const;
};

LILA_UI_API SyntaxFormats syntaxFormats();

/** Per-block tokens, kept so bracket matching and completion can tell code from strings and comments. */
class LILA_UI_API PythonBlockData : public QTextBlockUserData
{
public:
    std::vector<Token> tokens;
    /** Language server tokens laid over `tokens`, valid while the block's text is still `semanticText`. */
    std::vector<Token> semantic;
    QString semanticText;

    /** True when `col` sits inside a string or comment. */
    bool inStringOrComment(int col) const;
};

class LILA_UI_API PythonHighlighter : public QSyntaxHighlighter
{
public:
    explicit PythonHighlighter(QTextDocument* doc);

    /** Semantic tokens by line, from the language server; rehighlights the lines whose tokens changed. */
    void setSemanticTokens(const QHash<int, std::vector<Token>>& byLine);

protected:
    void highlightBlock(const QString& text) override;

private:
    /** Lexical kinds a semantic token may recolour; keywords, builtins, self and literals stay lexical. */
    static bool overridable(TokenKind k);

    SyntaxFormats mFormats;
};
} // namespace lila::ui
