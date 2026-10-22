#include "ui/editor/PythonHighlighter.h"

#include "ui/editor/EditorPalette.h"
#include "ui/editor/TextUtil.h"

#include <QSet>

namespace lila::ui
{
namespace
{
enum StateBits
{
    TripleSingle = 1,
    TripleDouble = 2,
    FString = 4,
    Raw = 8,
};

const QSet<QString>& keywords()
{
    static const QSet<QString> s{"and",    "as",     "assert", "async",  "await",  "break",    "class", "continue",
                                 "def",    "del",    "elif",   "else",   "except", "finally",  "for",   "from",
                                 "global", "if",     "import", "in",     "is",     "lambda",   "nonlocal",
                                 "not",    "or",     "pass",   "raise",  "return", "try",      "while", "with",
                                 "yield",  "match",  "case",   "type"};
    return s;
}

const QSet<QString>& builtins()
{
    static const QSet<QString> s{
        "abs", "aiter", "all", "anext", "any", "ascii", "bin", "bool", "breakpoint", "bytearray", "bytes", "callable",
        "chr", "classmethod", "compile", "complex", "delattr", "dict", "dir", "divmod", "enumerate", "eval", "exec",
        "filter", "float", "format", "frozenset", "getattr", "globals", "hasattr", "hash", "help", "hex", "id", "input",
        "int", "isinstance", "issubclass", "iter", "len", "list", "locals", "map", "max", "memoryview", "min", "next",
        "object", "oct", "open", "ord", "pow", "print", "property", "range", "repr", "reversed", "round", "set",
        "setattr", "slice", "sorted", "staticmethod", "str", "sum", "super", "tuple", "vars", "zip", "__import__",
        "__name__", "__file__", "__doc__", "Exception", "BaseException", "ValueError", "TypeError", "KeyError",
        "IndexError", "AttributeError", "RuntimeError", "NotImplementedError", "StopIteration", "OSError",
        "ImportError", "NotImplemented", "Ellipsis"};
    return s;
}

/** Soft keywords double as names: `type = 3`, `match.group()`. */
bool softKeywordUsedAsName(QStringView line, int end)
{
    int j = end;
    while (j < line.size() && line[j] == ' ')
        ++j;
    if (j >= line.size())
        return true;
    QChar c = line[j];
    return c == '=' || c == '.' || c == ')' || c == ',' || c == ':' || c == '(' || c == '[';
}

class Tokenizer
{
public:
    Tokenizer(QStringView line, std::vector<Token>& out) : mLine(line), mOut(out) {}

    int run(int state)
    {
        int i = 0;
        if (state != 0)
        {
            i = tripleBody(0, 0, state);
            if (i < 0)
                return mEndState;
        }
        code(i, int(mLine.size()));
        return mEndState;
    }

private:
    void add(int start, int end, TokenKind kind)
    {
        if (end > start)
            mOut.push_back({start, end - start, kind});
    }

    /** Plain code over [i, end). Sets mEndState if a triple-quoted string runs off the line. */
    void code(int i, int end)
    {
        QString prevWord;
        bool lineStart = true;
        while (i < end)
        {
            QChar c = mLine[i];
            if (c.isSpace())
            {
                ++i;
                continue;
            }
            if (c == '#')
            {
                bool cell = lineStart && text::isCellMarker(mLine.mid(i));
                add(i, end, cell ? TokenKind::CellMarker : TokenKind::Comment);
                return;
            }
            if (c == '@' && lineStart)
            {
                int j = i + 1;
                while (j < end && (text::isIdentChar(mLine[j]) || mLine[j] == '.'))
                    ++j;
                add(i, j, TokenKind::Decorator);
                i = j;
                lineStart = false;
                continue;
            }
            lineStart = false;
            if (int q = stringStart(i, end); q >= 0)
            {
                int r = string(i, q, end);
                if (r < 0)
                    return;
                i = r;
                prevWord.clear();
                continue;
            }
            if (c.isDigit() || (c == '.' && i + 1 < end && mLine[i + 1].isDigit()))
            {
                int j = number(i, end);
                add(i, j, TokenKind::Number);
                i = j;
                prevWord.clear();
                continue;
            }
            if (text::isIdentChar(c))
            {
                int j = i;
                while (j < end && text::isIdentChar(mLine[j]))
                    ++j;
                QString word = mLine.mid(i, j - i).toString();
                bool afterDot = i > 0 && mLine[i - 1] == '.';
                bool soft = word == "match" || word == "case" || word == "type";
                if (prevWord == "def")
                    add(i, j, TokenKind::DefName);
                else if (prevWord == "class")
                    add(i, j, TokenKind::ClassName);
                else if (word == "True" || word == "False" || word == "None")
                    add(i, j, TokenKind::Constant);
                else if (afterDot)
                    ;
                else if (keywords().contains(word) && !(soft && softKeywordUsedAsName(mLine, j)))
                    add(i, j, TokenKind::Keyword);
                else if (word == "self" || word == "cls")
                    add(i, j, TokenKind::Self);
                else if (builtins().contains(word) || (word == "type" && soft))
                    add(i, j, TokenKind::Builtin);
                prevWord = word;
                i = j;
                continue;
            }
            prevWord.clear();
            ++i;
        }
    }

    int number(int i, int end) const
    {
        int j = i;
        if (mLine[j] == '0' && j + 1 < end && QStringView(u"xXoObB").contains(mLine[j + 1]))
        {
            j += 2;
            while (j < end && (mLine[j].isLetterOrNumber() || mLine[j] == '_'))
                ++j;
            return j;
        }
        while (j < end)
        {
            QChar c = mLine[j];
            if (c.isDigit() || c == '_' || c == '.' || c == 'j' || c == 'J')
                ++j;
            else if ((c == 'e' || c == 'E') && j + 1 < end)
            {
                ++j;
                if (mLine[j] == '+' || mLine[j] == '-')
                    ++j;
            }
            else
                break;
        }
        return j;
    }

    /** Index of the opening quote if a literal (with optional prefix) starts at i, else -1. */
    int stringStart(int i, int end) const
    {
        if (i > 0 && text::isIdentChar(mLine[i - 1]))
            return -1;
        int j = i;
        while (j < end && j - i < 2 && QStringView(u"rRbBfFuUtT").contains(mLine[j]))
            ++j;
        if (j < end && (mLine[j] == '"' || mLine[j] == '\''))
            return j;
        return -1;
    }

    /** Scans a literal whose prefix starts at i and quote at q. Returns the index past it, or -1 if it runs on. */
    int string(int i, int q, int end)
    {
        int bits = 0;
        for (int k = i; k < q; ++k)
        {
            QChar p = mLine[k].toLower();
            if (p == 'f' || p == 't')
                bits |= FString;
            else if (p == 'r')
                bits |= Raw;
        }
        QChar quote = mLine[q];
        bool triple = q + 2 < end && mLine[q + 1] == quote && mLine[q + 2] == quote;
        if (triple)
            return tripleBody(i, q + 3, bits | (quote == '"' ? TripleDouble : TripleSingle));
        return singleBody(i, q + 1, quote, bits, end);
    }

    int singleBody(int segStart, int i, QChar quote, int bits, int end)
    {
        while (i < end)
        {
            QChar c = mLine[i];
            if (c == '\\' && !(bits & Raw))
            {
                add(segStart, i, TokenKind::String);
                add(i, std::min(i + 2, end), TokenKind::StringEscape);
                i += 2;
                segStart = i;
                continue;
            }
            if (c == quote)
            {
                add(segStart, i + 1, TokenKind::String);
                return i + 1;
            }
            if ((bits & FString) && c == '{')
            {
                add(segStart, i, TokenKind::String);
                i = interpolation(i, end);
                segStart = i;
                continue;
            }
            ++i;
        }
        // Unterminated: string to the end of the line, and the next line starts fresh.
        add(segStart, end, TokenKind::String);
        return end;
    }

    /** Triple-quoted body from i. Returns the index past the close, or -1 with mEndState set when it runs on. */
    int tripleBody(int segStart, int i, int bits)
    {
        QChar quote = (bits & TripleDouble) ? '"' : '\'';
        int end = int(mLine.size());
        while (i < end)
        {
            QChar c = mLine[i];
            if (c == '\\' && !(bits & Raw))
            {
                add(segStart, i, TokenKind::String);
                add(i, std::min(i + 2, end), TokenKind::StringEscape);
                i += 2;
                segStart = i;
                continue;
            }
            if (c == quote && i + 2 < end && mLine[i + 1] == quote && mLine[i + 2] == quote)
            {
                add(segStart, i + 3, TokenKind::String);
                return i + 3;
            }
            if ((bits & FString) && c == '{')
            {
                add(segStart, i, TokenKind::String);
                i = interpolation(i, end);
                segStart = i;
                continue;
            }
            ++i;
        }
        add(segStart, end, TokenKind::String);
        mEndState = bits;
        return -1;
    }

    /** f-string replacement field at `{`: braces bright, the expression as code, any format spec as string. */
    int interpolation(int i, int end)
    {
        if (i + 1 < end && mLine[i + 1] == '{')
        {
            add(i, i + 2, TokenKind::String);
            return i + 2;
        }
        add(i, i + 1, TokenKind::FStringBrace);
        int depth = 0;
        int j = i + 1;
        int exprEnd = -1;
        QChar inQuote;
        for (; j < end; ++j)
        {
            QChar c = mLine[j];
            if (!inQuote.isNull())
            {
                if (c == inQuote)
                    inQuote = QChar();
                continue;
            }
            if (c == '"' || c == '\'')
                inQuote = c;
            else if (c == '(' || c == '[' || c == '{')
                ++depth;
            else if ((c == ')' || c == ']') && depth > 0)
                --depth;
            else if (c == '}')
            {
                if (depth == 0)
                    break;
                --depth;
            }
            else if (depth == 0 && exprEnd < 0 && (c == ':' || (c == '!' && !(j + 1 < end && mLine[j + 1] == '='))))
                exprEnd = j;
        }
        Tokenizer inner(mLine, mOut);
        inner.code(i + 1, exprEnd >= 0 ? exprEnd : j);
        if (exprEnd >= 0)
            add(exprEnd, j, TokenKind::String);
        if (j < end)
        {
            add(j, j + 1, TokenKind::FStringBrace);
            return j + 1;
        }
        return end;
    }

    QStringView mLine;
    std::vector<Token>& mOut;
    int mEndState = 0;
};
} // namespace

int tokenizePython(QStringView line, int state, std::vector<Token>& out)
{
    return Tokenizer(line, out).run(state < 0 ? 0 : state);
}

bool PythonBlockData::inStringOrComment(int col) const
{
    for (const Token& t : tokens)
    {
        if (t.kind == TokenKind::Comment || t.kind == TokenKind::CellMarker)
        {
            if (col > t.start)
                return true;
        }
        else if ((t.kind == TokenKind::String || t.kind == TokenKind::StringEscape) && col > t.start &&
                 col < t.start + t.length)
            return true;
    }
    return false;
}

PythonHighlighter::PythonHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc)
{
    using palette::gray;
    auto set = [this](TokenKind k, QColor c, bool bold = false, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)
            f.setFontWeight(QFont::Bold);
        f.setFontItalic(italic);
        mFormats[size_t(k)] = f;
    };
    set(TokenKind::Keyword, gray(242), true);
    set(TokenKind::Constant, gray(242), true, true);
    set(TokenKind::Builtin, gray(224));
    set(TokenKind::Self, gray(160), false, true);
    set(TokenKind::DefName, gray(255), true);
    set(TokenKind::ClassName, gray(255), true);
    set(TokenKind::Decorator, gray(172), false, true);
    set(TokenKind::String, gray(146));
    set(TokenKind::StringEscape, gray(210), true);
    set(TokenKind::FStringBrace, gray(228), true);
    set(TokenKind::Number, gray(232));
    set(TokenKind::Comment, gray(112), false, true);
    set(TokenKind::CellMarker, gray(185), true, true);
}

void PythonHighlighter::highlightBlock(const QString& text)
{
    auto* data = static_cast<PythonBlockData*>(currentBlockUserData());
    if (!data)
    {
        data = new PythonBlockData;
        setCurrentBlockUserData(data);
    }
    data->tokens.clear();
    int state = tokenizePython(text, previousBlockState(), data->tokens);
    for (const Token& t : data->tokens)
        setFormat(t.start, t.length, mFormats[size_t(t.kind)]);
    setCurrentBlockState(state);
}
} // namespace lila::ui
