#include "ui/editor/VimEngine.h"

#include "ui/editor/TextUtil.h"

#include <QRegularExpression>

#include <algorithm>
#include <climits>

namespace lila::ui::vim
{
namespace
{
/** 0 whitespace, 1 word, 2 punctuation; WORDs (big) only tell blank from not. */
int cls(QChar c, bool big)
{
    if (c.isSpace())
        return 0;
    if (big || c.isLetterOrNumber() || c == '_')
        return 1;
    return 2;
}

bool isBlank(QChar c)
{
    return c == ' ' || c == '\t';
}

bool isDigitKey(const QString& k)
{
    return k.size() == 1 && k[0] >= '0' && k[0] <= '9';
}

int readCount(const QStringList& keys, int& i)
{
    int count = 0;
    while (i < keys.size() && isDigitKey(keys[i]) && (keys[i] != "0" || count > 0))
    {
        count = std::min(count * 10 + (keys[i][0].unicode() - '0'), 99999);
        ++i;
    }
    return count;
}

int combineCounts(int a, int b)
{
    if (a == 0 && b == 0)
        return 0;
    return std::max(1, a) * std::max(1, b);
}

const QStringList& singleMotions()
{
    static const QStringList m{"h", "j", "k", "l", "w", "b", "e", "W", "B", "E", "0", "^", "$", "G", ";", ",", "%", "n", "N",
                               "*", "#", "{", "}", "_", "+", "-", "<Left>", "<Right>", "<Up>", "<Down>", "<Home>", "<End>",
                               "<BS>", " ", "<CR>"};
    return m;
}

bool isMotion(const QString& name)
{
    static const QStringList multi{"gg", "ge", "gE", "f", "F", "t", "T"};
    return singleMotions().contains(name) || multi.contains(name);
}

bool isTextObject(const QString& name)
{
    return name.size() == 2 && (name[0] == 'i' || name[0] == 'a');
}

bool keepsColumn(const QString& m)
{
    return m == "j" || m == "k" || m == "<Up>" || m == "<Down>";
}

/** Vim's default "magic" pattern syntax to PCRE: \( \) \| \+ \? \{ are operators, bare ones are literal. */
QRegularExpression compilePattern(const QString& pat, bool caseless)
{
    QString out;
    bool veryMagic = false;
    for (int i = 0; i < pat.size(); ++i)
    {
        QChar c = pat[i];
        if (c == '\\' && i + 1 < pat.size())
        {
            QChar d = pat[++i];
            if (d == 'v')
                veryMagic = true;
            else if (d == 'c')
                caseless = true;
            else if (d == 'C')
                caseless = false;
            else if (d == '<' || d == '>')
                out += "\\b";
            else if (!veryMagic && QStringLiteral("()|+?{}=").contains(d))
                out += d == '=' ? QChar('?') : d;
            else
                out += QString('\\') + d;
            continue;
        }
        if (!veryMagic && QStringLiteral("()|+?{}").contains(c))
            out += QString('\\') + c;
        else
            out += c;
    }
    QRegularExpression re(out, caseless ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption);
    if (!re.isValid())
        re = QRegularExpression(QRegularExpression::escape(pat));
    return re;
}

/** A :s replacement: \1..\9, & for the whole match, \n or \r for a line break. */
QString expandReplacement(const QString& rep, const QRegularExpressionMatch& m)
{
    QString out;
    for (int i = 0; i < rep.size(); ++i)
    {
        QChar c = rep[i];
        if (c == '\\' && i + 1 < rep.size())
        {
            QChar d = rep[++i];
            if (d >= '0' && d <= '9')
                out += m.captured(d.unicode() - '0');
            else if (d == 'n' || d == 'r')
                out += '\n';
            else if (d == 't')
                out += '\t';
            else
                out += d;
        }
        else if (c == '&')
            out += m.captured(0);
        else
            out += c;
    }
    return out;
}

/** Splits "pat/rep/flags" on unescaped separators; escaped separators lose their backslash. */
QStringList splitSubstitute(const QString& s, QChar sep)
{
    QStringList parts{QString()};
    for (int i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            if (s[i + 1] != sep)
                parts.back() += '\\';
            parts.back() += s[++i];
        }
        else if (s[i] == sep)
            parts.push_back(QString());
        else
            parts.back() += s[i];
    }
    return parts;
}

/** What was typed into `before` to make `after`, when it was a single insertion; empty otherwise. */
QString insertedText(const QString& before, const QString& after)
{
    if (after.size() <= before.size())
        return {};
    const int m = int(before.size());
    int p = 0;
    while (p < m && before[p] == after[p])
        ++p;
    int s = 0;
    while (s < m - p && before[m - 1 - s] == after[after.size() - 1 - s])
        ++s;
    if (p + s != m)
        return {};
    return after.mid(p, after.size() - before.size());
}
} // namespace

Engine::Engine(Buffer& buffer) : mBuf(buffer)
{
}

// ------------------------------------------------------------------- text helpers

void Engine::reload()
{
    mText = mBuf.text();
    mCursor = std::clamp(mBuf.cursor(), 0, int(mText.size()));
    if (mCursor != mKnownCursor)
        mWantCol = mCursor - lineStart(mCursor);
}

void Engine::edit(int pos, int length, const QString& with)
{
    mBuf.replace(pos, length, with);
    mText.replace(pos, length, with);
}

void Engine::setCursor(int pos, bool keepWantCol)
{
    mCursor = std::clamp(pos, 0, int(mText.size()));
    mKnownCursor = mCursor;
    mBuf.setCursor(mCursor);
    if (!keepWantCol)
        mWantCol = mCursor - lineStart(mCursor);
}

int Engine::lineStart(int pos) const
{
    pos = std::clamp(pos, 0, int(mText.size()));
    if (pos == 0)
        return 0;
    return int(mText.lastIndexOf('\n', pos - 1)) + 1;
}

int Engine::lineEnd(int pos) const
{
    int i = int(mText.indexOf('\n', std::clamp(pos, 0, int(mText.size()))));
    return i < 0 ? int(mText.size()) : i;
}

int Engine::lineOf(int pos) const
{
    pos = std::clamp(pos, 0, int(mText.size()));
    return int(std::count(mText.begin(), mText.begin() + pos, QChar('\n')));
}

int Engine::lineCount() const
{
    return int(mText.count('\n')) + 1;
}

int Engine::posOfLine(int line) const
{
    int pos = 0;
    for (int l = 0; l < line; ++l)
    {
        int i = int(mText.indexOf('\n', pos));
        if (i < 0)
            return pos;
        pos = i + 1;
    }
    return pos;
}

int Engine::firstNonBlank(int pos) const
{
    int ls = lineStart(pos), le = lineEnd(pos);
    int i = ls;
    while (i < le && isBlank(mText[i]))
        ++i;
    return i < le ? i : std::max(ls, le - 1);
}

int Engine::clampNormal(int pos) const
{
    pos = std::clamp(pos, 0, int(mText.size()));
    int ls = lineStart(pos), le = lineEnd(pos);
    return le > ls && pos >= le ? le - 1 : pos;
}

bool Engine::isEmptyLine(int line) const
{
    int ls = posOfLine(line);
    return lineEnd(ls) == ls;
}

int Engine::nextWordStart(int p, bool big) const
{
    const int n = int(mText.size());
    if (p >= n)
        return n;
    int c = cls(mText[p], big);
    if (c != 0)
        while (p < n && cls(mText[p], big) == c)
            ++p;
    while (p < n && cls(mText[p], big) == 0)
    {
        // An empty line counts as a word.
        if (mText[p] == '\n' && p + 1 < n && mText[p + 1] == '\n')
            return p + 1;
        ++p;
    }
    return p;
}

int Engine::nextWordEnd(int p, bool big) const
{
    const int n = int(mText.size());
    ++p;
    while (p < n && cls(mText[p], big) == 0)
        ++p;
    if (p >= n)
        return std::max(0, n - 1);
    int c = cls(mText[p], big);
    while (p + 1 < n && cls(mText[p + 1], big) == c)
        ++p;
    return p;
}

int Engine::prevWordStart(int p, bool big) const
{
    if (p <= 0)
        return 0;
    --p;
    while (p > 0 && cls(mText[p], big) == 0)
    {
        if (mText[p] == '\n' && mText[p - 1] == '\n')
            return p;
        --p;
    }
    int c = cls(mText[p], big);
    if (c == 0)
        return p;
    while (p > 0 && cls(mText[p - 1], big) == c)
        --p;
    return p;
}

int Engine::prevWordEnd(int p, bool big) const
{
    if (p <= 0 || p >= mText.size())
        return std::max(0, std::min(p, int(mText.size()) - 1));
    int c = cls(mText[p], big);
    while (p > 0 && c != 0 && cls(mText[p], big) == c)
        --p;
    while (p > 0 && cls(mText[p], big) == 0)
    {
        if (mText[p] == '\n' && mText[p - 1] == '\n')
            return p;
        --p;
    }
    return p;
}

int Engine::matchBracket(int p) const
{
    static const QString opens = "([{", closes = ")]}";
    QChar c = mText[p];
    int kind = int(opens.indexOf(c));
    bool forward = kind >= 0;
    if (!forward)
        kind = int(closes.indexOf(c));
    if (kind < 0)
        return -1;
    QChar open = opens[kind], close = closes[kind];
    int depth = 0;
    for (int i = p; forward ? i < mText.size() : i >= 0; i += forward ? 1 : -1)
    {
        if (mText[i] == open)
            depth += forward ? 1 : -1;
        else if (mText[i] == close)
            depth += forward ? -1 : 1;
        if (depth == 0)
            return i;
    }
    return -1;
}

// ------------------------------------------------------------------- keys

QString Engine::status() const
{
    QString s;
    switch (mMode)
    {
    case Mode::Normal: s = "NORMAL"; break;
    case Mode::Insert: s = "INSERT"; break;
    case Mode::Visual: s = "VISUAL"; break;
    case Mode::VisualLine: s = "V-LINE"; break;
    case Mode::CommandLine: return mCmdLine;
    }
    if (!mPending.isEmpty())
        s += "  " + mPending.join(QString());
    return s;
}

void Engine::reset()
{
    mPending.clear();
    if (mMode == Mode::Insert)
        leaveInsert();
    else if (mMode == Mode::Visual || mMode == Mode::VisualLine)
        exitVisual();
    else if (mMode == Mode::CommandLine)
    {
        mCmdLine.clear();
        mMode = Mode::Normal;
    }
}

bool Engine::key(const QString& token)
{
    const QString k = token == "<C-[>" ? QStringLiteral("<Esc>") : token;
    switch (mMode)
    {
    case Mode::Insert: return insertKey(k);
    case Mode::Normal: return normalKey(k);
    case Mode::Visual:
    case Mode::VisualLine: return visualKey(k);
    case Mode::CommandLine: return commandLineKey(k);
    }
    return false;
}

bool Engine::insertKey(const QString& k)
{
    if (k != "<Esc>")
        return false;
    leaveInsert();
    return true;
}

bool Engine::normalKey(const QString& k)
{
    static const QStringList ownCtrl{"<C-r>", "<C-d>", "<C-u>"};
    if (k.startsWith("<C-") && !ownCtrl.contains(k) && mPending.isEmpty())
        return false;
    reload();
    mCursor = clampNormal(mCursor);
    if (k == "<Esc>")
    {
        mPending.clear();
        return true;
    }
    mPending << k;
    Cmd cmd;
    Parse r = parseNormal(mPending, cmd);
    if (r == Parse::Incomplete)
        return true;
    mPending.clear();
    if (r == Parse::Done)
        execute(cmd);
    return true;
}

bool Engine::visualKey(const QString& k)
{
    static const QStringList ownCtrl{"<C-d>", "<C-u>"};
    if (k.startsWith("<C-") && !ownCtrl.contains(k) && mPending.isEmpty())
        return false;
    reload();
    if (k == "<Esc>")
    {
        mPending.clear();
        exitVisual();
        return true;
    }
    mPending << k;
    Cmd cmd;
    Parse r = parseVisual(mPending, cmd);
    if (r == Parse::Incomplete)
        return true;
    mPending.clear();
    if (r == Parse::Done)
        executeVisual(cmd);
    return true;
}

bool Engine::commandLineKey(const QString& k)
{
    if (k == "<Esc>")
    {
        mCmdLine.clear();
        mMode = mCmdLineFrom;
        return true;
    }
    if (k == "<CR>")
    {
        QString line = mCmdLine;
        mCmdLine.clear();
        mMode = Mode::Normal;
        reload();
        runCommandLine(line);
        return true;
    }
    if (k == "<BS>")
    {
        mCmdLine.chop(1);
        if (mCmdLine.isEmpty())
            mMode = mCmdLineFrom;
        return true;
    }
    if (k.size() == 1)
        mCmdLine += k;
    return true;
}

// ------------------------------------------------------------------- parsing

Engine::Parse Engine::parseMotion(const QStringList& keys, int& i, Cmd& cmd) const
{
    const int n = int(keys.size());
    const QString& k = keys[i];
    if (singleMotions().contains(k))
    {
        cmd.name = k;
        return Parse::Done;
    }
    if (k == "f" || k == "F" || k == "t" || k == "T")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        if (keys[i + 1].size() != 1)
            return Parse::Invalid;
        cmd.name = k;
        cmd.arg = keys[i + 1][0];
        return Parse::Done;
    }
    if (k == "g")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        const QString& k2 = keys[i + 1];
        if (k2 == "g" || k2 == "e" || k2 == "E")
        {
            cmd.name = "g" + k2;
            return Parse::Done;
        }
    }
    return Parse::Invalid;
}

Engine::Parse Engine::parseNormal(const QStringList& keys, Cmd& cmd) const
{
    const int n = int(keys.size());
    int i = 0;
    if (keys[0] == "\"")
    {
        if (n < 2)
            return Parse::Incomplete;
        if (keys[1].size() != 1)
            return Parse::Invalid;
        cmd.reg = keys[1][0];
        i = 2;
    }
    int count1 = readCount(keys, i);
    if (i >= n)
        return Parse::Incomplete;
    QString k = keys[i];

    QString op;
    if (k == "d" || k == "c" || k == "y" || k == ">" || k == "<")
    {
        op = k;
        ++i;
    }
    else if (k == "g")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        const QString& k2 = keys[i + 1];
        if (k2 == "~" || k2 == "u" || k2 == "U")
        {
            op = "g" + k2;
            i += 2;
        }
    }

    if (!op.isEmpty())
    {
        cmd.op = op;
        int count2 = readCount(keys, i);
        cmd.count = combineCounts(count1, count2);
        if (i >= n)
            return Parse::Incomplete;
        k = keys[i];
        const QString last = op.right(1);
        // dd, cc, yy, >>, <<, g~~, guu, gUU (and g~g~, gugu, gUgU).
        if ((op.size() == 1 && k == op) || (op.size() == 2 && k == last))
        {
            cmd.name = "line";
            return Parse::Done;
        }
        if (op.size() == 2 && k == "g")
        {
            if (i + 1 >= n)
                return Parse::Incomplete;
            if (keys[i + 1] == last)
            {
                cmd.name = "line";
                return Parse::Done;
            }
        }
        if (k == "i" || k == "a")
        {
            if (i + 1 >= n)
                return Parse::Incomplete;
            const QString& obj = keys[i + 1];
            if (obj.size() != 1 || !QStringLiteral("wWp\"'`()b[]{}B<>").contains(obj[0]))
                return Parse::Invalid;
            cmd.name = k + obj;
            return Parse::Done;
        }
        return parseMotion(keys, i, cmd);
    }

    cmd.count = count1;
    Parse r = parseMotion(keys, i, cmd);
    if (r != Parse::Invalid)
        return r;

    static const QStringList actions{"x", "X", "<Del>", "s", "S", "D", "C", "Y", "p", "P", "u", "<C-r>", ".", "i", "a", "I",
                                     "A", "o", "O", "J", "~", "v", "V", ":", "/", "?", "<C-d>", "<C-u>", "<Insert>"};
    if (actions.contains(k))
    {
        cmd.name = k;
        return Parse::Done;
    }
    if (k == "r")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        const QString& c = keys[i + 1];
        if (c != "<CR>" && c.size() != 1)
            return Parse::Invalid;
        cmd.name = "r";
        cmd.arg = c == "<CR>" ? QChar('\n') : c[0];
        return Parse::Done;
    }
    if (k == "Z" || k == "g")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        QString two = k + keys[i + 1];
        if (two == "ZZ" || two == "ZQ" || two == "gJ")
        {
            cmd.name = two;
            return Parse::Done;
        }
    }
    return Parse::Invalid;
}

Engine::Parse Engine::parseVisual(const QStringList& keys, Cmd& cmd) const
{
    const int n = int(keys.size());
    int i = 0;
    if (keys[0] == "\"")
    {
        if (n < 2)
            return Parse::Incomplete;
        if (keys[1].size() != 1)
            return Parse::Invalid;
        cmd.reg = keys[1][0];
        i = 2;
    }
    cmd.count = readCount(keys, i);
    if (i >= n)
        return Parse::Incomplete;
    const QString& k = keys[i];
    if (k == "i" || k == "a")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        const QString& obj = keys[i + 1];
        if (obj.size() != 1 || !QStringLiteral("wWp\"'`()b[]{}B<>").contains(obj[0]))
            return Parse::Invalid;
        cmd.name = k + obj;
        return Parse::Done;
    }
    Parse r = parseMotion(keys, i, cmd);
    if (r != Parse::Invalid)
        return r;
    static const QStringList actions{"d", "x", "X", "D", "<Del>", "c", "s", "C", "S", "R", "y", "Y", ">", "<", "~", "u",
                                     "U", "J", "p", "P", "o", "O", "v", "V", "<C-d>", "<C-u>"};
    if (actions.contains(k))
    {
        cmd.name = k;
        return Parse::Done;
    }
    if (k == "r")
    {
        if (i + 1 >= n)
            return Parse::Incomplete;
        if (keys[i + 1].size() != 1)
            return Parse::Invalid;
        cmd.name = "r";
        cmd.arg = keys[i + 1][0];
        return Parse::Done;
    }
    return Parse::Invalid;
}

// ------------------------------------------------------------------- motions and objects

std::optional<Engine::Target> Engine::motion(const QString& m, QChar arg, int count, bool forOperator)
{
    const int n = std::max(1, count);
    const int size = int(mText.size());
    const int p = mCursor;

    if (m == "h" || m == "<Left>" || m == "<BS>")
        return Target{std::max(lineStart(p), p - n)};
    if (m == "l" || m == "<Right>" || m == " ")
        return Target{std::min(lineEnd(p), p + n)};
    if (m == "j" || m == "k" || m == "<Up>" || m == "<Down>" || m == "+" || m == "-" || m == "<CR>")
    {
        bool down = m == "j" || m == "<Down>" || m == "+" || m == "<CR>";
        int line = lineOf(p);
        int target = down ? std::min(line + n, lineCount() - 1) : std::max(line - n, 0);
        if (target == line && forOperator)
            return std::nullopt;
        int ls = posOfLine(target);
        if (!keepsColumn(m))
            return Target{firstNonBlank(ls), true};
        int last = std::max(ls, lineEnd(ls) - 1);
        int pos = mWantCol == INT_MAX ? last : std::min(ls + mWantCol, last);
        return Target{pos, true};
    }
    if (m == "w" || m == "W")
    {
        int q = p;
        for (int k = 0; k < n; ++k)
            q = nextWordStart(q, m == "W");
        return Target{q};
    }
    if (m == "e" || m == "E")
    {
        int q = p;
        for (int k = 0; k < n; ++k)
            q = nextWordEnd(q, m == "E");
        return Target{q, false, true};
    }
    if (m == "b" || m == "B")
    {
        int q = p;
        for (int k = 0; k < n; ++k)
            q = prevWordStart(q, m == "B");
        return Target{q};
    }
    if (m == "ge" || m == "gE")
    {
        int q = p;
        for (int k = 0; k < n; ++k)
            q = prevWordEnd(q, m == "gE");
        return Target{q, false, true};
    }
    if (m == "0" || m == "<Home>")
        return Target{lineStart(p)};
    if (m == "^")
        return Target{firstNonBlank(p)};
    if (m == "$" || m == "<End>")
    {
        int ls = posOfLine(std::min(lineOf(p) + n - 1, lineCount() - 1));
        int le = lineEnd(ls);
        if (le == ls)
            return Target{ls};
        return Target{le - 1, false, true};
    }
    if (m == "_")
        return Target{firstNonBlank(posOfLine(std::min(lineOf(p) + n - 1, lineCount() - 1))), true};
    if (m == "gg" || m == "G")
    {
        int line = count > 0 ? count - 1 : (m == "gg" ? 0 : lineCount() - 1);
        return Target{firstNonBlank(posOfLine(std::clamp(line, 0, lineCount() - 1))), true};
    }
    if (m == "f" || m == "F" || m == "t" || m == "T" || m == ";" || m == ",")
    {
        QChar kind = m[0], c = arg;
        bool repeat = m == ";" || m == ",";
        if (repeat)
        {
            if (mLastFindKind.isNull())
                return std::nullopt;
            c = mLastFindChar;
            kind = mLastFindKind;
            if (m == ",")
                kind = kind.isUpper() ? kind.toLower() : kind.toUpper();
        }
        else
        {
            mLastFindKind = kind;
            mLastFindChar = c;
        }
        const bool forward = kind == 'f' || kind == 't';
        const bool till = kind == 't' || kind == 'T';
        const int ls = lineStart(p), le = lineEnd(p);
        int q = p;
        for (int k = 0; k < n; ++k)
        {
            int step = till && repeat && k == 0 ? 2 : 1;
            int start = forward ? q + step : q - step;
            q = forward ? int(mText.indexOf(c, start)) : (start >= ls ? int(mText.lastIndexOf(c, start)) : -1);
            if (q < 0 || (forward && q >= le) || (!forward && q < ls))
                return std::nullopt;
        }
        if (till)
            q += forward ? -1 : 1;
        return Target{q, false, forward};
    }
    if (m == "%")
    {
        for (int i = p, le = lineEnd(p); i < le; ++i)
            if (QStringLiteral("()[]{}").contains(mText[i]))
            {
                int q = matchBracket(i);
                if (q < 0)
                    return std::nullopt;
                return Target{q, false, true};
            }
        return std::nullopt;
    }
    if (m == "n" || m == "N")
    {
        auto q = search(p, m == "n" ? mSearchForward : !mSearchForward, n);
        if (!q)
            return std::nullopt;
        return Target{*q};
    }
    if (m == "*" || m == "#")
    {
        int ls = lineStart(p), le = lineEnd(p);
        int s = p;
        while (s < le && cls(mText[s], false) != 1)
            ++s;
        if (s >= le)
            return std::nullopt;
        int e = s;
        while (s > ls && cls(mText[s - 1], false) == 1)
            --s;
        while (e < le && cls(mText[e], false) == 1)
            ++e;
        setSearch("\\<" + mText.mid(s, e - s) + "\\>", m == "*");
        auto q = search(m == "*" ? p : s, m == "*", n);
        if (!q)
            return std::nullopt;
        return Target{*q};
    }
    if (m == "}" || m == "{")
    {
        int line = lineOf(p);
        const int last = lineCount() - 1;
        for (int k = 0; k < n; ++k)
        {
            if (m == "}")
            {
                while (line < last && isEmptyLine(line))
                    ++line;
                while (line < last && !isEmptyLine(line))
                    ++line;
            }
            else
            {
                while (line > 0 && isEmptyLine(line))
                    --line;
                while (line > 0 && !isEmptyLine(line))
                    --line;
            }
        }
        int ls = posOfLine(line);
        if (m == "}" && line == last && !isEmptyLine(line))
            return Target{size, false};
        return Target{ls};
    }
    return std::nullopt;
}

std::optional<Engine::Range> Engine::textObject(const QString& name, int count, int pos) const
{
    const QChar kind = name[0], o = name[1];
    const int ls = lineStart(pos), le = lineEnd(pos);

    if (o == 'w' || o == 'W')
    {
        if (pos >= le)
            return std::nullopt;
        const bool big = o == 'W';
        const int c = cls(mText[pos], big);
        int from = pos, to = pos + 1;
        while (from > ls && cls(mText[from - 1], big) == c)
            --from;
        while (to < le && cls(mText[to], big) == c)
            ++to;
        if (kind == 'a')
        {
            if (c != 0)
            {
                int t = to;
                while (t < le && isBlank(mText[t]))
                    ++t;
                if (t > to)
                    to = t;
                else
                    while (from > ls && isBlank(mText[from - 1]))
                        --from;
            }
            else if (to < le)
            {
                int c2 = cls(mText[to], big);
                while (to < le && cls(mText[to], big) == c2)
                    ++to;
            }
        }
        return Range{from, to, false};
    }

    if (o == '"' || o == '\'' || o == '`')
    {
        QList<int> quotes;
        for (int i = ls; i < le; ++i)
            if (mText[i] == o && (i == ls || mText[i - 1] != '\\'))
                quotes.push_back(i);
        int idx = -1;
        for (int j = 0; j < quotes.size() && idx < 0; ++j)
            if (quotes[j] == pos)
                idx = j % 2 == 0 ? j : j - 1;
        for (int j = 0; j + 1 < quotes.size() && idx < 0; j += 2)
            if (quotes[j] < pos && pos < quotes[j + 1])
                idx = j;
        for (int j = 0; j + 1 < quotes.size() && idx < 0; j += 2)
            if (quotes[j] > pos)
                idx = j;
        if (idx < 0 || idx + 1 >= quotes.size())
            return std::nullopt;
        int a = quotes[idx], b = quotes[idx + 1];
        if (kind == 'i')
            return Range{a + 1, b, false};
        int from = a, to = b + 1;
        int t = to;
        while (t < le && isBlank(mText[t]))
            ++t;
        if (t > to)
            to = t;
        else
            while (from > ls && isBlank(mText[from - 1]))
                --from;
        return Range{from, to, false};
    }

    if (o == 'p')
    {
        const int last = lineCount() - 1;
        int first = lineOf(pos), end = first;
        const bool blank = isEmptyLine(first);
        while (first > 0 && isEmptyLine(first - 1) == blank)
            --first;
        while (end < last && isEmptyLine(end + 1) == blank)
            ++end;
        if (kind == 'a')
        {
            if (!blank)
            {
                int e = end;
                while (e < last && isEmptyLine(e + 1))
                    ++e;
                if (e > end)
                    end = e;
                else
                    while (first > 0 && isEmptyLine(first - 1))
                        --first;
            }
            else
                while (end < last && !isEmptyLine(end + 1))
                    ++end;
        }
        return Range{posOfLine(first), lineEnd(posOfLine(end)), true};
    }

    QChar open, close;
    switch (o.unicode())
    {
    case '(':
    case ')':
    case 'b': open = '(', close = ')'; break;
    case '[':
    case ']': open = '[', close = ']'; break;
    case '{':
    case '}':
    case 'B': open = '{', close = '}'; break;
    case '<':
    case '>': open = '<', close = '>'; break;
    default: return std::nullopt;
    }
    auto enclosingOpen = [&](int from) {
        int depth = 0;
        for (int i = from; i >= 0; --i)
        {
            if (mText[i] == close)
                ++depth;
            else if (mText[i] == open && depth-- == 0)
                return i;
        }
        return -1;
    };
    auto matchForward = [&](int at) {
        int depth = 0;
        for (int i = at; i < mText.size(); ++i)
        {
            if (mText[i] == open)
                ++depth;
            else if (mText[i] == close && --depth == 0)
                return i;
        }
        return -1;
    };
    int a = -1;
    if (pos < mText.size() && mText[pos] == open)
        a = pos;
    else if (pos < mText.size() && mText[pos] == close)
    {
        int depth = 0;
        for (int i = pos; i >= 0 && a < 0; --i)
        {
            if (mText[i] == close)
                ++depth;
            else if (mText[i] == open && --depth == 0)
                a = i;
        }
    }
    else
        a = enclosingOpen(pos - 1);
    for (int level = 1; level < std::max(1, count) && a >= 0; ++level)
        a = enclosingOpen(a - 1);
    if (a < 0)
        return std::nullopt;
    int b = matchForward(a);
    if (b < 0)
        return std::nullopt;
    if (kind == 'a')
        return Range{a, b + 1, false};
    int from = a + 1, to = b;
    // A block spanning lines keeps its bracket lines: only what is between them goes.
    if (from < mText.size() && mText[from] == '\n')
    {
        ++from;
        int closeLine = lineStart(b);
        bool blankBefore = true;
        for (int i = closeLine; i < b && blankBefore; ++i)
            blankBefore = isBlank(mText[i]);
        if (blankBefore && closeLine >= from)
            to = closeLine;
    }
    return Range{from, std::max(from, to), false};
}

// ------------------------------------------------------------------- execution

void Engine::yank(QChar regName, const QString& text, bool linewise, bool deleting)
{
    Register r{text, linewise};
    if (regName == '_')
        return;
    if (regName.isLetter() && regName.isUpper())
    {
        Register& target = mRegisters[regName.toLower()];
        target.text += text;
        target.linewise = target.linewise || linewise;
        r = target;
    }
    else if (regName.isLetter())
        mRegisters[regName] = r;
    else if (regName == '+' || regName == '*')
        mBuf.setClipboard(text);
    mRegisters['"'] = r;
    if (!deleting)
        mRegisters['0'] = r;
}

Engine::Register Engine::reg(QChar name) const
{
    if (name.isNull())
        name = '"';
    if (name == '+' || name == '*')
    {
        QString t = mBuf.clipboard();
        return {t, t.endsWith('\n')};
    }
    return mRegisters.value(name.isLetter() ? name.toLower() : name);
}

void Engine::execute(const Cmd& cmd, const QString* replayInsert)
{
    const QString& a = cmd.name;
    const int count = cmd.count;
    const int n = std::max(1, count);

    if (a == ".")
    {
        if (!mHaveLastChange)
            return;
        Cmd again = mLastChange;
        if (count > 0)
            again.count = count;
        QString insert = mLastInsert;
        mReplaying = true;
        execute(again, &insert);
        mReplaying = false;
        return;
    }

    static const QStringList insertActions{"s", "S", "C", "i", "a", "I", "A", "o", "O", "<Insert>"};
    static const QStringList changeActions{"x", "X", "<Del>", "D", "p", "P", "J", "gJ", "~", "r"};
    const bool entersInsert = cmd.op == "c" || insertActions.contains(a);
    if (entersInsert)
    {
        mRecording = true;
        mPendingChange = cmd;
    }
    else if ((!cmd.op.isEmpty() && cmd.op != "y") || changeActions.contains(a))
    {
        mLastChange = cmd;
        mLastInsert.clear();
        mHaveLastChange = true;
    }

    if (!cmd.op.isEmpty())
    {
        const int size = int(mText.size());
        Range r;
        if (a == "line")
        {
            int l0 = lineOf(mCursor);
            int l1 = std::min(l0 + n - 1, lineCount() - 1);
            r = {posOfLine(l0), lineEnd(posOfLine(l1)), true};
        }
        else if (isTextObject(a))
        {
            auto o = textObject(a, count, mCursor);
            if (!o)
                return;
            r = *o;
        }
        else
        {
            const bool big = a == "W";
            // cw on a word is ce, and stays put on a word's last character.
            if (cmd.op == "c" && (a == "w" || a == "W") && mCursor < size && !mText[mCursor].isSpace())
            {
                int q = mCursor;
                for (int k = 0; k < n; ++k)
                {
                    bool atEnd = q + 1 >= size || cls(mText[q + 1], big) != cls(mText[q], big);
                    q = k == 0 && atEnd ? q : nextWordEnd(q, big);
                }
                r = {mCursor, std::min(q + 1, size), false};
            }
            else
            {
                auto t = motion(a, cmd.arg, count, true);
                if (!t)
                    return;
                int from = std::min(mCursor, t->pos), to = std::max(mCursor, t->pos);
                if (t->linewise)
                    r = {lineStart(from), lineEnd(to), true};
                else
                {
                    if (t->inclusive)
                        to = std::min(to + 1, size);
                    // The last word moved over ends the range, not the next line's indent.
                    if ((a == "w" || a == "W") && lineOf(to) > lineOf(from))
                    {
                        int ls = lineStart(to);
                        bool blank = true;
                        for (int i = ls; i < to && blank; ++i)
                            blank = isBlank(mText[i]);
                        if (blank && ls - 1 >= from)
                            to = ls - 1;
                    }
                    r = {from, to, false};
                }
            }
        }
        applyOperator(cmd.op, r, cmd, replayInsert);
        return;
    }

    if (isMotion(a))
    {
        auto t = motion(a, cmd.arg, count, false);
        if (!t)
            return;
        setCursor(clampNormal(t->pos), keepsColumn(a));
        if (a == "$" || a == "<End>")
            mWantCol = INT_MAX;
        return;
    }

    const int ls = lineStart(mCursor), le = lineEnd(mCursor);
    if (a == "x" || a == "<Del>")
    {
        if (le > ls)
            applyOperator("d", {mCursor, std::min(mCursor + n, le), false}, cmd, nullptr);
    }
    else if (a == "X")
    {
        if (mCursor > ls)
            applyOperator("d", {std::max(ls, mCursor - n), mCursor, false}, cmd, nullptr);
    }
    else if (a == "s")
        applyOperator("c", {mCursor, std::min(mCursor + n, le), false}, cmd, replayInsert);
    else if (a == "S" || a == "Y")
    {
        int l1 = std::min(lineOf(mCursor) + n - 1, lineCount() - 1);
        applyOperator(a == "S" ? "c" : "y", {ls, lineEnd(posOfLine(l1)), true}, cmd, replayInsert);
    }
    else if (a == "D" || a == "C")
    {
        auto t = motion("$", QChar(), count, true);
        int to = t && t->inclusive ? t->pos + 1 : (t ? t->pos : mCursor);
        applyOperator(a == "D" ? "d" : "c", {mCursor, std::max(mCursor, to), false}, cmd, replayInsert);
    }
    else if (a == "p" || a == "P")
        put(cmd, a == "P");
    else if (a == "u" || a == "<C-r>")
    {
        for (int k = 0; k < n; ++k)
            a == "u" ? mBuf.undo() : mBuf.redo();
        reload();
        setCursor(clampNormal(mCursor));
    }
    else if (a == "i" || a == "<Insert>")
        enterInsert(replayInsert, n);
    else if (a == "a")
    {
        setCursor(le > ls ? mCursor + 1 : mCursor);
        enterInsert(replayInsert, n);
    }
    else if (a == "I")
    {
        int i = ls;
        while (i < le && isBlank(mText[i]))
            ++i;
        setCursor(i);
        enterInsert(replayInsert, n);
    }
    else if (a == "A")
    {
        setCursor(le);
        enterInsert(replayInsert, n);
    }
    else if (a == "o" || a == "O")
    {
        mBuf.beginChange();
        mChangeOpen = true;
        QString line = mText.mid(ls, le - ls);
        if (a == "o")
        {
            QString indent(text::indentAfter(line), ' ');
            edit(le, 0, "\n" + indent);
            setCursor(le + 1 + int(indent.size()));
            mRepeatPrefix = "\n" + indent;
        }
        else
        {
            QString indent(text::leadingSpaces(line), ' ');
            edit(ls, 0, indent + "\n");
            setCursor(ls + int(indent.size()));
            mRepeatPrefix = "\n" + indent;
        }
        enterInsert(replayInsert, n);
    }
    else if (a == "J" || a == "gJ")
        joinLines(lineOf(mCursor), n, a == "J");
    else if (a == "~")
    {
        int to = std::min(mCursor + n, le);
        changeCase(mCursor, to, '~');
        setCursor(clampNormal(to));
    }
    else if (a == "r")
    {
        if (mCursor + n > le)
            return;
        mBuf.beginChange();
        if (cmd.arg == '\n')
        {
            edit(mCursor, n, "\n");
            mBuf.endChange();
            setCursor(mCursor + 1);
        }
        else
        {
            edit(mCursor, n, QString(n, cmd.arg));
            mBuf.endChange();
            setCursor(mCursor + n - 1);
        }
    }
    else if (a == "v")
        enterVisual(Mode::Visual);
    else if (a == "V")
        enterVisual(Mode::VisualLine);
    else if (a == ":" || a == "/" || a == "?")
    {
        mCmdLineFrom = Mode::Normal;
        mMode = Mode::CommandLine;
        mCmdLine = a;
    }
    else if (a == "<C-d>" || a == "<C-u>")
    {
        int lines = count > 0 ? count : mBuf.halfPage();
        int line = lineOf(mCursor) + (a == "<C-d>" ? lines : -lines);
        setCursor(firstNonBlank(posOfLine(std::clamp(line, 0, lineCount() - 1))));
    }
    else if (a == "ZZ")
    {
        mBuf.write();
        mBuf.close();
    }
    else if (a == "ZQ")
        mBuf.close();
}

void Engine::applyOperator(const QString& op, Range r, const Cmd& cmd, const QString* replayInsert)
{
    const int size = int(mText.size());
    if (r.linewise)
    {
        const QString lines = mText.mid(r.from, r.to - r.from) + "\n";
        if (op == "y")
        {
            yank(cmd.reg, lines, true, false);
            if (lineOf(mCursor) != lineOf(r.from))
                setCursor(clampNormal(std::min(r.from + mWantCol, lineEnd(r.from))), true);
        }
        else if (op == "d")
        {
            yank(cmd.reg, lines, true, true);
            int from = r.from, to = r.to + 1;
            if (to > size)
            {
                to = size;
                if (from > 0)
                    --from; // the last line takes the newline before it
            }
            mBuf.beginChange();
            edit(from, to - from, QString());
            mBuf.endChange();
            setCursor(firstNonBlank(from < r.from ? lineStart(from) : from));
        }
        else if (op == "c")
        {
            yank(cmd.reg, lines, true, true);
            QString indent(text::leadingSpaces(QStringView(mText).mid(r.from, r.to - r.from)), ' ');
            mBuf.beginChange();
            mChangeOpen = true;
            edit(r.from, r.to - r.from, indent);
            setCursor(r.from + int(indent.size()));
            mRepeatPrefix.clear();
            enterInsert(replayInsert, 1);
        }
        else if (op == ">" || op == "<")
            shiftLines(lineOf(r.from), lineOf(r.to), 1, op == ">");
        else if (op.startsWith('g'))
        {
            changeCase(r.from, r.to, op[1]);
            setCursor(firstNonBlank(r.from));
        }
        return;
    }

    const QString text = mText.mid(r.from, r.to - r.from);
    if (op == "y")
    {
        yank(cmd.reg, text, false, false);
        setCursor(clampNormal(r.from));
    }
    else if (op == "d")
    {
        yank(cmd.reg, text, false, true);
        mBuf.beginChange();
        edit(r.from, r.to - r.from, QString());
        mBuf.endChange();
        setCursor(clampNormal(r.from));
    }
    else if (op == "c")
    {
        yank(cmd.reg, text, false, true);
        mBuf.beginChange();
        mChangeOpen = true;
        edit(r.from, r.to - r.from, QString());
        setCursor(r.from);
        mRepeatPrefix.clear();
        enterInsert(replayInsert, 1);
    }
    else if (op == ">" || op == "<")
        shiftLines(lineOf(r.from), lineOf(std::max(r.from, r.to - 1)), 1, op == ">");
    else if (op.startsWith('g'))
    {
        changeCase(r.from, r.to, op[1]);
        setCursor(clampNormal(r.from));
    }
}

void Engine::put(const Cmd& cmd, bool before)
{
    const Register r = reg(cmd.reg);
    if (r.text.isEmpty())
        return;
    const int n = std::max(1, cmd.count);
    mBuf.beginChange();
    if (r.linewise)
    {
        QString block = r.text.endsWith('\n') ? r.text : r.text + "\n";
        block = block.repeated(n);
        int at;
        if (before)
        {
            at = lineStart(mCursor);
            edit(at, 0, block);
        }
        else
        {
            int le = lineEnd(mCursor);
            at = le + 1;
            if (le >= mText.size())
                edit(le, 0, "\n" + block.chopped(1));
            else
                edit(at, 0, block);
        }
        mBuf.endChange();
        setCursor(firstNonBlank(at));
        return;
    }
    const QString s = r.text.repeated(n);
    int at = !before && mCursor < lineEnd(mCursor) ? mCursor + 1 : mCursor;
    edit(at, 0, s);
    mBuf.endChange();
    setCursor(s.contains('\n') ? at : at + int(s.size()) - 1);
}

void Engine::joinLines(int line, int count, bool spaces)
{
    const int joins = std::max(1, count - 1);
    int at = -1;
    mBuf.beginChange();
    for (int k = 0; k < joins; ++k)
    {
        int le = lineEnd(posOfLine(line));
        if (le >= mText.size())
            break;
        int next = le + 1, ne = next;
        while (ne < mText.size() && isBlank(mText[ne]))
            ++ne;
        QString sep;
        if (spaces)
        {
            bool nextEmpty = ne >= mText.size() || mText[ne] == '\n';
            bool endsBlank = le > lineStart(le) && isBlank(mText[le - 1]);
            if (!nextEmpty && !endsBlank && mText[ne] != ')')
                sep = " ";
        }
        edit(le, (spaces ? ne : next) - le, sep);
        at = le;
    }
    mBuf.endChange();
    if (at >= 0)
        setCursor(clampNormal(at));
}

void Engine::shiftLines(int firstLine, int lastLine, int count, bool right)
{
    const int width = text::kIndent * std::max(1, count);
    mBuf.beginChange();
    for (int l = firstLine; l <= lastLine; ++l)
    {
        int ls = posOfLine(l), le = lineEnd(ls);
        if (right)
        {
            if (le > ls)
                edit(ls, 0, QString(width, ' '));
            continue;
        }
        int i = ls, col = 0;
        while (i < le && col < width && isBlank(mText[i]))
        {
            col += mText[i] == '\t' ? text::kIndent : 1;
            ++i;
        }
        if (i > ls)
            edit(ls, i - ls, QString());
    }
    mBuf.endChange();
    setCursor(firstNonBlank(posOfLine(firstLine)));
}

void Engine::changeCase(int from, int to, QChar how)
{
    QString s = mText.mid(from, to - from);
    QString out = s;
    for (QChar& c : out)
        c = how == 'u' ? c.toLower() : how == 'U' ? c.toUpper() : (c.isUpper() ? c.toLower() : c.toUpper());
    if (out == s)
        return;
    mBuf.beginChange();
    edit(from, to - from, out);
    mBuf.endChange();
}

// ------------------------------------------------------------------- insert

void Engine::enterInsert(const QString* replayInsert, int repeat)
{
    if (!mChangeOpen)
    {
        mBuf.beginChange();
        mChangeOpen = true;
        mRepeatPrefix.clear();
    }
    mMode = Mode::Insert;
    mInsertRepeat = std::max(1, repeat);
    mInsertStartText = mText;
    mBuf.setCursor(mCursor);
    if (replayInsert)
    {
        edit(mCursor, 0, *replayInsert);
        mBuf.setCursor(mCursor + int(replayInsert->size()));
        leaveInsert();
    }
}

void Engine::leaveInsert()
{
    reload();
    const QString inserted = insertedText(mInsertStartText, mText);
    if (mInsertRepeat > 1 && !inserted.isEmpty())
    {
        if (mRepeatPrefix.isEmpty())
        {
            QString more = inserted.repeated(mInsertRepeat - 1);
            edit(mCursor, 0, more);
            mCursor += int(more.size());
        }
        else
        {
            QString more = (mRepeatPrefix + inserted).repeated(mInsertRepeat - 1);
            int at = lineEnd(mCursor);
            edit(at, 0, more);
            mCursor = at + int(more.size());
        }
    }
    if (mChangeOpen)
    {
        mBuf.endChange();
        mChangeOpen = false;
    }
    if (mRecording)
    {
        mLastChange = mPendingChange;
        mLastInsert = inserted;
        mHaveLastChange = true;
        mRecording = false;
    }
    mInsertRepeat = 1;
    mRepeatPrefix.clear();
    mMode = Mode::Normal;
    int p = mCursor;
    if (p > lineStart(p))
        --p;
    setCursor(clampNormal(p));
}

// ------------------------------------------------------------------- visual

void Engine::enterVisual(Mode m)
{
    mAnchor = mCursor;
    mMode = m;
    updateSelection();
}

void Engine::exitVisual()
{
    mMode = Mode::Normal;
    mBuf.showSelection(-1, -1);
    setCursor(clampNormal(mCursor), true);
}

Engine::Range Engine::visualRange() const
{
    int a = std::min(mAnchor, mCursor), b = std::max(mAnchor, mCursor);
    if (mMode == Mode::VisualLine)
        return {lineStart(a), lineEnd(b), true};
    return {a, std::min(b + 1, int(mText.size())), false};
}

void Engine::updateSelection()
{
    mBuf.setCursor(mCursor);
    Range r = visualRange();
    mBuf.showSelection(r.from, r.linewise ? std::min(r.to + 1, int(mText.size())) : r.to);
}

void Engine::executeVisual(const Cmd& cmd)
{
    const QString& a = cmd.name;
    const int n = std::max(1, cmd.count);

    if (isMotion(a))
    {
        auto t = motion(a, cmd.arg, cmd.count, false);
        if (t)
        {
            setCursor(clampNormal(t->pos), keepsColumn(a));
            if (a == "$" || a == "<End>")
                mWantCol = INT_MAX;
        }
        updateSelection();
        return;
    }
    if (isTextObject(a))
    {
        auto r = textObject(a, cmd.count, mCursor);
        if (r && r->to > r->from)
        {
            if (r->linewise && mMode == Mode::Visual)
                mMode = Mode::VisualLine;
            mAnchor = std::min(mAnchor, r->from);
            setCursor(r->linewise ? r->to : r->to - 1);
        }
        updateSelection();
        return;
    }
    if (a == "o" || a == "O")
    {
        std::swap(mAnchor, mCursor);
        setCursor(mCursor);
        updateSelection();
        return;
    }
    if (a == "v" || a == "V")
    {
        Mode want = a == "v" ? Mode::Visual : Mode::VisualLine;
        if (mMode == want)
            exitVisual();
        else
        {
            mMode = want;
            updateSelection();
        }
        return;
    }
    if (a == "<C-d>" || a == "<C-u>")
    {
        int lines = cmd.count > 0 ? cmd.count : mBuf.halfPage();
        int line = lineOf(mCursor) + (a == "<C-d>" ? lines : -lines);
        setCursor(firstNonBlank(posOfLine(std::clamp(line, 0, lineCount() - 1))));
        updateSelection();
        return;
    }

    Range r = visualRange();
    Range lines{lineStart(r.from), lineEnd(r.linewise ? r.to : std::max(r.from, r.to - 1)), true};
    const int firstLine = lineOf(r.from), lastLine = lineOf(lines.to);
    mMode = Mode::Normal;
    mBuf.showSelection(-1, -1);
    mCursor = r.from;

    if (a == "d" || a == "x" || a == "<Del>")
        applyOperator("d", r, cmd, nullptr);
    else if (a == "X" || a == "D")
        applyOperator("d", lines, cmd, nullptr);
    else if (a == "c" || a == "s")
        applyOperator("c", r, cmd, nullptr);
    else if (a == "C" || a == "S" || a == "R")
        applyOperator("c", lines, cmd, nullptr);
    else if (a == "y")
        applyOperator("y", r, cmd, nullptr);
    else if (a == "Y")
        applyOperator("y", lines, cmd, nullptr);
    else if (a == ">" || a == "<")
        shiftLines(firstLine, lastLine, n, a == ">");
    else if (a == "~" || a == "u" || a == "U")
    {
        changeCase(r.from, r.to, a[0]);
        setCursor(clampNormal(r.from));
    }
    else if (a == "J")
        joinLines(firstLine, std::max(2, lastLine - firstLine + 1));
    else if (a == "r")
    {
        QString s = mText.mid(r.from, r.to - r.from);
        for (QChar& c : s)
            if (c != '\n')
                c = cmd.arg;
        mBuf.beginChange();
        edit(r.from, r.to - r.from, s);
        mBuf.endChange();
        setCursor(clampNormal(r.from));
    }
    else if (a == "p" || a == "P")
    {
        const Register saved = reg(cmd.reg);
        int from = r.from, to = r.to;
        if (r.linewise && to < mText.size())
            ++to;
        yank(QChar(), mText.mid(from, to - from), r.linewise, true);
        QString text = saved.text;
        if (r.linewise && !text.endsWith('\n'))
            text += '\n';
        else if (!r.linewise && saved.linewise)
            text = "\n" + text;
        if (r.linewise && to == mText.size() && !mText.endsWith('\n') && text.endsWith('\n'))
            text.chop(1);
        mBuf.beginChange();
        edit(from, to - from, text);
        mBuf.endChange();
        if (text.contains('\n'))
            setCursor(firstNonBlank(text.startsWith('\n') ? from + 1 : from));
        else
            setCursor(clampNormal(from + int(text.size()) - 1));
    }
    else
        setCursor(clampNormal(mCursor));
}

// ------------------------------------------------------------------- command line and search

void Engine::setSearch(const QString& pattern, bool forward)
{
    mSearch = pattern;
    mSearchForward = forward;
}

std::optional<int> Engine::search(int from, bool forward, int count)
{
    if (mSearch.isEmpty())
    {
        mBuf.message("No previous search pattern");
        return std::nullopt;
    }
    const QRegularExpression re = compilePattern(mSearch, false);
    int p = from;
    for (int k = 0; k < std::max(1, count); ++k)
    {
        if (forward)
        {
            auto m = re.match(mText, std::min(p + 1, int(mText.size())));
            if (!m.hasMatch())
                m = re.match(mText, 0);
            if (!m.hasMatch())
            {
                mBuf.message("Pattern not found: " + mSearch);
                return std::nullopt;
            }
            p = int(m.capturedStart());
        }
        else
        {
            int best = -1, lastStart = -1;
            for (auto it = re.globalMatch(mText); it.hasNext();)
            {
                int s = int(it.next().capturedStart());
                if (s < p)
                    best = s;
                lastStart = s;
            }
            if (best < 0)
                best = lastStart;
            if (best < 0)
            {
                mBuf.message("Pattern not found: " + mSearch);
                return std::nullopt;
            }
            p = best;
        }
    }
    return p;
}

void Engine::runCommandLine(const QString& line)
{
    if (line.isEmpty())
        return;
    const QChar kind = line[0];
    const QString body = line.mid(1);
    if (kind == '/' || kind == '?')
    {
        setSearch(body.isEmpty() ? mSearch : body, kind == '/');
        if (auto p = search(mCursor, kind == '/', 1))
            setCursor(*p);
        return;
    }
    runEx(body.trimmed());
}

void Engine::runEx(const QString& ex)
{
    if (ex.isEmpty())
        return;
    if (ex == "w" || ex == "w!" || ex == "write")
    {
        mBuf.write();
        return;
    }
    if (ex == "q" || ex == "q!" || ex == "quit")
    {
        mBuf.close();
        return;
    }
    if (ex == "wq" || ex == "wq!" || ex == "x" || ex == "x!")
    {
        mBuf.write();
        mBuf.close();
        return;
    }
    if (ex == "noh" || ex == "nohlsearch")
        return;
    bool isNumber = false;
    int number = ex.toInt(&isNumber);
    if (isNumber || ex == "$")
    {
        int line = ex == "$" ? lineCount() - 1 : std::clamp(number - 1, 0, lineCount() - 1);
        setCursor(firstNonBlank(posOfLine(line)));
        return;
    }

    int first = lineOf(mCursor), last = first;
    QString rest = ex;
    static const QRegularExpression range(R"(^(\d+),(\d+))");
    if (rest.startsWith('%'))
    {
        first = 0;
        last = lineCount() - 1;
        rest = rest.mid(1);
    }
    else if (auto m = range.match(rest); m.hasMatch())
    {
        first = std::clamp(m.captured(1).toInt() - 1, 0, lineCount() - 1);
        last = std::clamp(m.captured(2).toInt() - 1, first, lineCount() - 1);
        rest = rest.mid(m.capturedLength());
    }
    if ((rest.startsWith("s") || rest.startsWith("substitute")) && rest.size() > 1)
    {
        QString body = rest.startsWith("substitute") ? rest.mid(10) : rest.mid(1);
        if (!body.isEmpty() && !body[0].isLetterOrNumber() && !body[0].isSpace())
        {
            substitute(body, first, last);
            return;
        }
    }
    mBuf.message("Not an editor command: " + ex);
}

bool Engine::substitute(const QString& body, int firstLine, int lastLine)
{
    QStringList parts = splitSubstitute(body.mid(1), body[0]);
    QString pattern = parts.value(0);
    const QString rep = parts.value(1);
    const QString flags = parts.value(2);
    if (pattern.isEmpty())
        pattern = mSearch;
    if (pattern.isEmpty())
    {
        mBuf.message("No previous search pattern");
        return false;
    }
    mSearch = pattern;
    const QRegularExpression re = compilePattern(pattern, flags.contains('i'));
    const bool global = flags.contains('g');

    int changes = 0, lastChanged = -1;
    mBuf.beginChange();
    // Bottom up, so a replacement that adds lines leaves the lines still to do where they were.
    for (int l = lastLine; l >= firstLine; --l)
    {
        const int ls = posOfLine(l), le = lineEnd(ls);
        const QString line = mText.mid(ls, le - ls);
        QString out;
        int done = 0;
        bool changed = false;
        for (auto it = re.globalMatch(line); it.hasNext();)
        {
            auto m = it.next();
            out += line.mid(done, m.capturedStart() - done) + expandReplacement(rep, m);
            done = int(m.capturedEnd());
            changed = true;
            ++changes;
            if (!global)
                break;
        }
        if (!changed)
            continue;
        out += line.mid(done);
        edit(ls, le - ls, out);
        if (lastChanged < 0)
            lastChanged = l;
    }
    mBuf.endChange();
    if (changes == 0)
    {
        mBuf.message("Pattern not found: " + pattern);
        return false;
    }
    setCursor(firstNonBlank(posOfLine(lastChanged)));
    return true;
}
} // namespace lila::ui::vim
