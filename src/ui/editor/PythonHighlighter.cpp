#include "ui/editor/PythonHighlighter.h"

#include "ui/editor/EditorPalette.h"
#include "ui/editor/TextUtil.h"

#include <QSet>
#include <QTextBlock>

#include <algorithm>

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
    StringBits = 15,
};

/** What an open bracket is, kept in the block state so multi-line calls and signatures carry on. */
enum class Bracket : uint8_t
{
    Other,
    Call,
    DefParams,
};

/** Bracket depth in bits 4-7, the kinds of the outermost eight levels in two bits each from bit 8. */
constexpr int kDepthShift = 4;
constexpr int kKindShift = 8;
constexpr int kMaxDepth = 15;
constexpr int kKeptKinds = 8;

int encodeBrackets(const std::vector<Bracket>& b)
{
    int depth = std::min(int(b.size()), kMaxDepth);
    int out = depth << kDepthShift;
    for (int i = 0; i < std::min(depth, kKeptKinds); ++i)
        out |= int(b[size_t(i)]) << (kKindShift + 2 * i);
    return out;
}

std::vector<Bracket> decodeBrackets(int state)
{
    int depth = (state >> kDepthShift) & kMaxDepth;
    std::vector<Bracket> b(size_t(depth), Bracket::Other);
    for (int i = 0; i < std::min(depth, kKeptKinds); ++i)
        b[size_t(i)] = Bracket((state >> (kKindShift + 2 * i)) & 3);
    return b;
}

bool isAllCaps(const QString& w)
{
    bool letter = false;
    for (QChar c : w)
    {
        if (c.isLower())
            return false;
        letter |= c.isLetter();
    }
    return letter && w.size() > 1;
}

bool isCapWords(const QString& w)
{
    int i = 0;
    while (i < w.size() && w[i] == '_')
        ++i;
    return i < w.size() && w[i].isUpper() && !isAllCaps(w);
}

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
        mBrackets = decodeBrackets(state);
        int i = 0;
        if (state & StringBits)
        {
            i = tripleBody(0, 0, state & StringBits);
            if (i < 0)
                return mEndState | encodeBrackets(mBrackets);
        }
        else if (!mBrackets.empty() && !mLine.isEmpty() && (text::isIdentChar(mLine[0]) || mLine[0] == '@'))
            // A statement at column 0 cannot be inside brackets, so an unclosed one above stops here.
            mBrackets.clear();
        code(i, int(mLine.size()));
        return mEndState | encodeBrackets(mBrackets);
    }

private:
    void add(int start, int end, TokenKind kind)
    {
        if (end > start)
            mOut.push_back({start, end - start, kind});
    }

    enum class Import
    {
        None,
        Modules,
        FromNames,
    };

    /** Plain code over [i, end). Sets mEndState if a triple-quoted string runs off the line. */
    void code(int i, int end)
    {
        enum class Last
        {
            Other,
            Name,
            Close,
            DefName,
        };
        QString prevWord;
        bool lineStart = true;
        bool firstWord = true;
        Last last = Last::Other;
        Import import = Import::None;
        bool expectParam = top() == Bracket::DefParams;
        int lambdaDepth = -1;
        bool lambdaParam = false;
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
                last = Last::Name;
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
                last = Last::Other;
                continue;
            }
            if (c.isDigit() || (c == '.' && i + 1 < end && mLine[i + 1].isDigit()))
            {
                int j = number(i, end);
                add(i, j, TokenKind::Number);
                i = j;
                prevWord.clear();
                last = Last::Other;
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
                bool keyword = !afterDot && keywords().contains(word) && !(soft && softKeywordUsedAsName(mLine, j));
                last = keyword ? Last::Other : Last::Name;
                if (prevWord == "def")
                {
                    add(i, j, TokenKind::DefName);
                    last = Last::DefName;
                }
                else if (prevWord == "class")
                    add(i, j, TokenKind::ClassName);
                else if (word == "True" || word == "False" || word == "None")
                    add(i, j, TokenKind::Constant);
                else if (keyword)
                {
                    add(i, j, TokenKind::Keyword);
                    if (word == "from" && firstWord)
                        import = Import::Modules;
                    else if (word == "import")
                        import = firstWord ? Import::Modules : Import::FromNames;
                    else if (word == "lambda")
                    {
                        lambdaDepth = int(mBrackets.size());
                        lambdaParam = true;
                    }
                }
                else if (!afterDot && (word == "self" || word == "cls"))
                    add(i, j, TokenKind::Self);
                else if (!afterDot && (builtins().contains(word) || (word == "type" && soft)))
                    add(i, j, TokenKind::Builtin);
                else
                {
                    bool param = expectParam || (lambdaParam && lambdaDepth == int(mBrackets.size()));
                    add(i, j, nameKind(word, j, end, import, param));
                }
                prevWord = word;
                firstWord = false;
                i = j;
                continue;
            }
            prevWord.clear();
            int depth = int(mBrackets.size());
            if (c == '(' || c == '[' || c == '{')
            {
                Bracket kind = Bracket::Other;
                if (c == '(' && last == Last::DefName)
                    kind = Bracket::DefParams;
                else if (c == '(' && (last == Last::Name || last == Last::Close))
                    kind = Bracket::Call;
                mBrackets.push_back(kind);
                expectParam = kind == Bracket::DefParams;
                last = Last::Other;
            }
            else if (c == ')' || c == ']' || c == '}')
            {
                if (!mBrackets.empty())
                    mBrackets.pop_back();
                expectParam = false;
                if (lambdaDepth > int(mBrackets.size()))
                    lambdaDepth = -1;
                last = Last::Close;
            }
            else
            {
                if (c == ',')
                {
                    expectParam = top() == Bracket::DefParams;
                    if (lambdaDepth == depth)
                        lambdaParam = true;
                }
                else if (c == '=' || c == ':')
                {
                    expectParam = false;
                    if (lambdaDepth == depth)
                    {
                        lambdaParam = false;
                        if (c == ':')
                            lambdaDepth = -1;
                    }
                }
                else if (c == ';')
                {
                    import = Import::None;
                    firstWord = true;
                }
                last = Last::Other;
            }
            ++i;
        }
    }

    Bracket top() const { return mBrackets.empty() ? Bracket::Other : mBrackets.back(); }

    /** A plain name: keyword argument, parameter, module, constant, class, call or variable. */
    TokenKind nameKind(const QString& word, int j, int end, Import import, bool param) const
    {
        if (import == Import::Modules)
            return TokenKind::Module;
        if (param)
            return TokenKind::Parameter;
        while (j < end && mLine[j] == ' ')
            ++j;
        QChar next = j < end ? mLine[j] : QChar();
        if (next == '=' && top() == Bracket::Call && !(j + 1 < end && mLine[j + 1] == '='))
            return TokenKind::Parameter;
        if (isAllCaps(word))
            return TokenKind::ConstantName;
        if (isCapWords(word))
            return TokenKind::ClassName;
        if (next == '(')
            return TokenKind::Function;
        return TokenKind::Variable;
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
    std::vector<Bracket> mBrackets;
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

SyntaxFormats syntaxFormats()
{
    using palette::gray;
    SyntaxFormats out;
    auto format = [](QColor c, bool bold = false, bool italic = false) {
        QTextCharFormat f;
        f.setForeground(c);
        if (bold)
            f.setFontWeight(QFont::Bold);
        f.setFontItalic(italic);
        return f;
    };
    auto set = [&](TokenKind k, QColor c, bool bold = false, bool italic = false) { out.tokens[size_t(k)] = format(c, bold, italic); };
    palette::Look look = palette::currentLook();
    if (look.monotone)
    {
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
        set(TokenKind::Function, gray(236));
        set(TokenKind::Variable, gray(206));
        set(TokenKind::Parameter, gray(206), false, true);
        set(TokenKind::Module, gray(186));
        set(TokenKind::ConstantName, gray(228));
        out.control = out.tokens[size_t(TokenKind::Keyword)];
        out.builtinType = out.tokens[size_t(TokenKind::Builtin)];
        return out;
    }
    struct Hues
    {
        const char *keyword, *control, *function, *type, *string, *escape, *number, *comment, *variable, *constant;
    };
    const Hues h = look.light ? Hues{"#0000ff", "#af00db", "#795e26", "#267f99", "#a31515", "#ee0000", "#098658", "#008000", "#001080", "#0070c1"}
                              : Hues{"#569cd6", "#c586c0", "#dcdcaa", "#4ec9b0", "#ce9178", "#d7ba7d", "#b5cea8", "#6a9955", "#9cdcfe", "#4fc1ff"};
    auto c = [](const char* hex) { return QColor(QString::fromLatin1(hex)); };
    set(TokenKind::Keyword, c(h.keyword));
    set(TokenKind::Constant, c(h.keyword));
    set(TokenKind::Builtin, c(h.function));
    set(TokenKind::Self, c(h.keyword), false, true);
    set(TokenKind::DefName, c(h.function));
    set(TokenKind::ClassName, c(h.type));
    set(TokenKind::Decorator, c(h.function), false, true);
    set(TokenKind::String, c(h.string));
    set(TokenKind::StringEscape, c(h.escape));
    set(TokenKind::FStringBrace, c(h.keyword));
    set(TokenKind::Number, c(h.number));
    set(TokenKind::Comment, c(h.comment), false, true);
    set(TokenKind::CellMarker, c(h.comment), true, true);
    set(TokenKind::Function, c(h.function));
    set(TokenKind::Variable, c(h.variable));
    set(TokenKind::Parameter, c(h.variable), false, true);
    set(TokenKind::Module, c(h.type));
    set(TokenKind::ConstantName, c(h.constant));
    out.control = format(c(h.control));
    out.builtinType = format(c(h.type));
    return out;
}

const QTextCharFormat& SyntaxFormats::forToken(QStringView line, const Token& t) const
{
    static const QSet<QString> controlWords{"if",  "elif",  "else",    "for",   "while", "break", "continue", "return",
                                            "try", "except", "finally", "raise", "with",  "yield", "import",   "from",
                                            "as",  "pass",  "await",   "match", "case",  "assert", "del"};
    static const QSet<QString> types{"int",    "float", "str",   "bool",      "list",        "dict",         "set",
                                     "tuple",  "bytes", "bytearray", "frozenset", "object", "type",        "complex",
                                     "range",  "slice", "memoryview", "property", "classmethod", "staticmethod", "super"};
    QString word = line.mid(t.start, t.length).toString();
    if (t.kind == TokenKind::Keyword && controlWords.contains(word))
        return control;
    if (t.kind == TokenKind::Builtin && (word.front().isUpper() || types.contains(word)))
        return builtinType;
    return tokens[size_t(t.kind)];
}

PythonHighlighter::PythonHighlighter(QTextDocument* doc) : QSyntaxHighlighter(doc), mFormats(syntaxFormats())
{
    palette::onLookChanged(this, [this] {
        mFormats = syntaxFormats();
        rehighlight();
    });
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
        setFormat(t.start, t.length, mFormats.forToken(text, t));
    if (data->semanticText != text)
        data->semantic.clear();
    for (const Token& s : data->semantic)
    {
        auto lexical = std::find_if(data->tokens.begin(), data->tokens.end(), [&](const Token& t) { return t.start == s.start; });
        if (lexical != data->tokens.end() && !overridable(lexical->kind))
            continue;
        setFormat(s.start, s.length, mFormats.tokens[size_t(s.kind)]);
    }
    setCurrentBlockState(state);
}

bool PythonHighlighter::overridable(TokenKind k)
{
    switch (k)
    {
    case TokenKind::Function:
    case TokenKind::Variable:
    case TokenKind::Parameter:
    case TokenKind::Module:
    case TokenKind::ConstantName:
    case TokenKind::ClassName:
        return true;
    default:
        return false;
    }
}

void PythonHighlighter::setSemanticTokens(const QHash<int, std::vector<Token>>& byLine)
{
    static const std::vector<Token> none;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
    {
        auto it = byLine.constFind(b.blockNumber());
        const std::vector<Token>& tokens = it == byLine.constEnd() ? none : *it;
        auto* data = static_cast<PythonBlockData*>(b.userData());
        if (!data || (data->semantic == tokens && data->semanticText == b.text()))
            continue;
        data->semantic = tokens;
        data->semanticText = b.text();
        rehighlightBlock(b);
    }
}
} // namespace lila::ui
