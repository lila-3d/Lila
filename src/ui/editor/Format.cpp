#include "ui/editor/Format.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "ui/editor/TextUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lila::ui::format
{
namespace
{
constexpr const char* kPrefs = "scriptEditor";
constexpr const char* kPlaceholderBody = "pass  # lila:format-placeholder";

/** The line without its comment and trailing space. Quotes are tracked within the line only. */
QStringView withoutComment(QStringView line)
{
    QChar quote;
    for (int i = 0; i < line.size(); ++i)
    {
        QChar c = line[i];
        if (!quote.isNull())
        {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = QChar();
        }
        else if (c == '"' || c == '\'')
            quote = c;
        else if (c == '#')
        {
            line = line.left(i);
            break;
        }
    }
    while (!line.isEmpty() && line.back().isSpace())
        line.chop(1);
    return line;
}

QString firstWord(QStringView s)
{
    int n = 0;
    while (n < s.size() && text::isIdentChar(s[n]))
        ++n;
    return s.left(n).toString();
}

/** Lines with their newline kept, so joining them gives back the text exactly. */
QStringList splitKeepingNewlines(const QString& s)
{
    QStringList out;
    int start = 0;
    for (int i = 0; i < s.size(); ++i)
        if (s[i] == '\n')
        {
            out.append(s.mid(start, i + 1 - start));
            start = i + 1;
        }
    if (start < s.size())
        out.append(s.mid(start));
    return out;
}

struct Hunk
{
    int aStart, aEnd, bStart, bEnd;
};

/** Changed runs of lines between a[0, n) and b[0, m) by longest common subsequence; one hunk when that would cost too much. */
std::vector<Hunk> lineHunks(const QStringList& a, const QStringList& b, int lo, int aHi, int bHi)
{
    const int n = aHi - lo, m = bHi - lo;
    if (n == 0 && m == 0)
        return {};
    if (n == 0 || m == 0 || int64_t(n + 1) * (m + 1) > 4'000'000 || std::min(n, m) >= 65535)
        return {{lo, aHi, lo, bHi}};

    std::vector<size_t> ha(n), hb(m);
    for (int i = 0; i < n; ++i)
        ha[i] = qHash(a[lo + i]);
    for (int j = 0; j < m; ++j)
        hb[j] = qHash(b[lo + j]);
    auto same = [&](int i, int j) { return ha[i] == hb[j] && a[lo + i] == b[lo + j]; };

    // table[i][j]: common subsequence length of a[i..] and b[j..].
    const int w = m + 1;
    std::vector<uint16_t> table(size_t(n + 1) * w, 0);
    for (int i = n - 1; i >= 0; --i)
        for (int j = m - 1; j >= 0; --j)
            table[size_t(i) * w + j] = same(i, j) ? uint16_t(table[size_t(i + 1) * w + j + 1] + 1)
                                                  : std::max(table[size_t(i + 1) * w + j], table[size_t(i) * w + j + 1]);

    std::vector<Hunk> out;
    int i = 0, j = 0;
    std::optional<Hunk> open;
    auto close = [&] {
        if (open)
        {
            open->aEnd = lo + i;
            open->bEnd = lo + j;
            out.push_back(*open);
            open.reset();
        }
    };
    while (i < n || j < m)
    {
        if (i < n && j < m && same(i, j))
        {
            close();
            ++i;
            ++j;
            continue;
        }
        if (!open)
            open = Hunk{lo + i, 0, lo + j, 0};
        if (j >= m || (i < n && table[size_t(i + 1) * w + j] >= table[size_t(i) * w + j + 1]))
            ++i;
        else
            ++j;
    }
    close();
    return out;
}
} // namespace

Settings Settings::load()
{
    const PrefStore& p = PrefStore::get();
    const Settings d;
    Settings s;
    s.onSave = p.value<bool>(kPrefs, key::onSave, d.onSave);
    s.organizeImportsOnSave = p.value<bool>(kPrefs, key::organizeImportsOnSave, d.organizeImportsOnSave);
    s.fixAllOnSave = p.value<bool>(kPrefs, key::fixAllOnSave, d.fixAllOnSave);
    s.onPaste = p.value<bool>(kPrefs, key::onPaste, d.onPaste);
    s.onType = p.value<bool>(kPrefs, key::onType, d.onType);
    s.onFocusLoss = p.value<bool>(kPrefs, key::onFocusLoss, d.onFocusLoss);
    s.trimTrailingWhitespace = p.value<bool>(kPrefs, key::trimTrailingWhitespace, d.trimTrailingWhitespace);
    s.finalNewline = p.value<bool>(kPrefs, key::finalNewline, d.finalNewline);
    s.useProjectConfig = p.value<bool>(kPrefs, key::useProjectConfig, d.useProjectConfig);
    s.lineLength = std::clamp(int(std::lround(p.value<double>(kPrefs, key::lineLength, d.lineLength))), 1, 320);
    s.indentWidth = std::clamp(int(std::lround(p.value<double>(kPrefs, key::indentWidth, d.indentWidth))), 1, 16);
    s.quoteStyle = QString::fromStdString(p.value<std::string>(kPrefs, key::quoteStyle, d.quoteStyle.toStdString()));
    if (s.quoteStyle != "single" && s.quoteStyle != "preserve")
        s.quoteStyle = "double";
    return s;
}

QString findProjectConfig(const QString& filePath)
{
    LILA_ZONE();

    QDir dir = QFileInfo(filePath).absoluteDir();
    while (true)
    {
        for (const char* name : {".ruff.toml", "ruff.toml"})
            if (QFileInfo f(dir.filePath(name)); f.isFile())
                return f.absoluteFilePath();
        QFile pyproject(dir.filePath("pyproject.toml"));
        if (pyproject.open(QIODevice::ReadOnly) && pyproject.readAll().contains("[tool.ruff"))
            return QFileInfo(pyproject).absoluteFilePath();
        if (!dir.cdUp())
            return {};
    }
}

QStringList styleArgs(const Settings& s, bool hasProjectConfig, bool forCheck)
{
    if (hasProjectConfig && s.useProjectConfig)
        return {};
    QStringList args{"--line-length", QString::number(s.lineLength)};
    if (!forCheck)
        args << "--config" << QString("format.quote-style = '%1'").arg(s.quoteStyle) << "--config" << QString("indent-width = %1").arg(s.indentWidth);
    return args;
}

QString rangeArg(const QStringList& lines, int line, int col, int endLine, int endCol)
{
    auto cp = [&](int l, int c) { return l >= 0 && l < lines.size() ? text::utf16ToCodepoint(lines[l], c) : c; };
    return QString("--range=%1:%2-%3:%4").arg(line + 1).arg(cp(line, col) + 1).arg(endLine + 1).arg(cp(endLine, endCol) + 1);
}

std::vector<Edit> diff(const QString& before, const QString& after)
{
    LILA_ZONE();

    if (before == after)
        return {};
    const QStringList a = splitKeepingNewlines(before), b = splitKeepingNewlines(after);
    int lo = 0;
    while (lo < a.size() && lo < b.size() && a[lo] == b[lo])
        ++lo;
    int aHi = int(a.size()), bHi = int(b.size());
    while (aHi > lo && bHi > lo && a[aHi - 1] == b[bHi - 1])
    {
        --aHi;
        --bHi;
    }
    std::vector<int> offsets(a.size() + 1, 0);
    for (int i = 0; i < a.size(); ++i)
        offsets[i + 1] = offsets[i] + int(a[i].size());

    std::vector<Edit> out;
    for (const Hunk& h : lineHunks(a, b, lo, aHi, bHi))
    {
        int start = offsets[h.aStart], end = offsets[h.aEnd];
        QString text;
        for (int j = h.bStart; j < h.bEnd; ++j)
            text += b[j];
        // Down to the characters that differ, so a caret beside the change keeps its place.
        int pre = 0;
        while (start + pre < end && pre < text.size() && before[start + pre] == text[pre])
            ++pre;
        int post = 0;
        while (end - post > start + pre && text.size() - post > pre && before[end - post - 1] == text[text.size() - post - 1])
            ++post;
        Edit e{start + pre, end - post, text.mid(pre, text.size() - pre - post)};
        if (e.start != e.end || !e.text.isEmpty())
            out.push_back(std::move(e));
    }
    return out;
}

int mapPosition(int pos, const QString& before, const std::vector<Edit>& edits)
{
    int delta = 0;
    for (const Edit& e : edits)
    {
        if (pos <= e.start)
            break;
        if (pos >= e.end)
        {
            delta += int(e.text.size()) - (e.end - e.start);
            continue;
        }
        int seen = 0;
        for (int i = e.start; i < pos && i < before.size(); ++i)
            seen += before[i].isSpace() ? 0 : 1;
        int k = 0;
        while (k < e.text.size() && seen > 0)
            seen -= e.text[k++].isSpace() ? 0 : 1;
        return e.start + delta + k;
    }
    return pos + delta;
}

std::vector<LineState> scan(const QStringList& lines)
{
    std::vector<LineState> out;
    out.reserve(lines.size() + 1);
    LineState st;
    QChar quote;
    bool triple = false;
    for (const QString& line : lines)
    {
        out.push_back(st);
        const int n = int(line.size());
        bool backslash = false;
        for (int i = 0; i < n; ++i)
        {
            const QChar c = line[i];
            if (!quote.isNull())
            {
                if (c == '\\')
                {
                    backslash = i + 1 == n;
                    ++i;
                }
                else if (c == quote && (!triple || (i + 2 < n && line[i + 1] == c && line[i + 2] == c)))
                {
                    i += triple ? 2 : 0;
                    quote = QChar();
                    triple = false;
                }
                continue;
            }
            if (c == '#')
                break;
            if (c == '"' || c == '\'')
            {
                quote = c;
                triple = i + 2 < n && line[i + 1] == c && line[i + 2] == c;
                i += triple ? 2 : 0;
            }
            else if (c == '\\' && i + 1 == n)
                backslash = true;
            else if (c == '(' || c == '[' || c == '{')
                ++st.depth;
            else if (c == ')' || c == ']' || c == '}')
                st.depth = std::max(0, st.depth - 1);
        }
        // A single-quoted string only carries on past a backslash.
        if (!quote.isNull() && !triple && !backslash)
            quote = QChar();
        st.inString = !quote.isNull();
        st.continued = backslash && quote.isNull();
    }
    out.push_back(st);
    return out;
}

std::optional<Statement> statementAt(const QStringList& lines, int line)
{
    if (line < 0 || line >= lines.size())
        return std::nullopt;
    const QString trimmed = lines[line].trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith('#'))
        return std::nullopt;
    const std::vector<LineState> st = scan(lines);
    const LineState& end = st[line + 1];
    if (end.depth > 0 || end.inString || end.continued)
        return std::nullopt;
    int first = line;
    while (first > 0 && (st[first].depth > 0 || st[first].inString || st[first].continued))
        --first;
    return Statement{first, line};
}

bool needsPlaceholder(const QStringList& lines, Statement statement)
{
    if (statement.last >= lines.size() || !withoutComment(lines[statement.last]).endsWith(':'))
        return false;
    const int indent = text::leadingSpaces(lines[statement.first]);
    for (int i = statement.last + 1; i < lines.size(); ++i)
    {
        const QString t = lines[i].trimmed();
        if (t.isEmpty() || t.startsWith('#'))
            continue;
        return text::leadingSpaces(lines[i]) <= indent;
    }
    return true;
}

QString placeholderLine(const QString& headerLine)
{
    return QString(text::leadingSpaces(headerLine) + text::kIndent, ' ') + kPlaceholderBody;
}

QString removePlaceholder(const QString& text)
{
    QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i)
        if (lines[i].trimmed() == kPlaceholderBody)
        {
            lines.removeAt(i);
            break;
        }
    return lines.join('\n');
}

QString cleanupForSave(const QString& text, bool trim, bool finalNewline, int keepLine)
{
    LILA_ZONE();

    QString out = text;
    if (trim)
    {
        QStringList lines = text.split('\n');
        const std::vector<LineState> st = scan(lines);
        for (int i = 0; i < lines.size(); ++i)
        {
            // Trailing space inside a string is part of its value.
            if (i == keepLine || st[i + 1].inString)
                continue;
            QString& l = lines[i];
            int n = int(l.size());
            while (n > 0 && (l[n - 1] == ' ' || l[n - 1] == '\t' || l[n - 1] == '\f'))
                --n;
            l.truncate(n);
        }
        out = lines.join('\n');
    }
    if (finalNewline && !out.isEmpty() && !out.endsWith('\n'))
        out += '\n';
    return out;
}

std::optional<int> keywordDedent(const QStringList& lines, int line)
{
    if (line <= 0 || line >= lines.size())
        return std::nullopt;
    const QString& current = lines[line];
    const QString word = firstWord(current.trimmed());
    QStringList openers;
    if (word == "else")
        openers = {"if", "elif", "for", "while", "try", "except"};
    else if (word == "elif")
        openers = {"if", "elif"};
    else if (word == "except")
        openers = {"try", "except"};
    else if (word == "finally")
        openers = {"try", "except", "else"};
    if (openers.isEmpty() || !withoutComment(current).endsWith(':'))
        return std::nullopt;

    const int indent = text::leadingSpaces(current);
    const std::vector<LineState> st = scan(lines);
    // Another statement at this indent since the opener means the opener's block is closed.
    bool blocked = false;
    for (int i = line - 1; i >= 0; --i)
    {
        const QString t = lines[i].trimmed();
        if (t.isEmpty() || t.startsWith('#') || st[i].depth > 0 || st[i].inString || st[i].continued)
            continue;
        const int ind = text::leadingSpaces(lines[i]);
        if (ind > indent)
            continue;
        const bool opens = openers.contains(firstWord(t));
        if (ind == indent)
        {
            if (opens && !blocked)
                return std::nullopt;
            blocked = true;
            continue;
        }
        return opens ? std::optional<int>(ind) : std::nullopt;
    }
    return std::nullopt;
}
} // namespace lila::ui::format
