#include "ui/editor/TextUtil.h"

#include <QDir>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

namespace lila::ui::text
{
int codepointToUtf16(QStringView line, int codepoint)
{
    int i = 0;
    for (int cp = 0; cp < codepoint && i < line.size(); ++cp)
        i += (line[i].isHighSurrogate() && i + 1 < line.size() && line[i + 1].isLowSurrogate()) ? 2 : 1;
    return i;
}

int utf16ToCodepoint(QStringView line, int utf16)
{
    int cp = 0;
    for (int i = 0; i < utf16 && i < line.size(); ++cp)
        i += (line[i].isHighSurrogate() && i + 1 < line.size() && line[i + 1].isLowSurrogate()) ? 2 : 1;
    return cp;
}

int leadingSpaces(QStringView line)
{
    int n = 0;
    for (QChar c : line)
    {
        if (c == ' ')
            ++n;
        else if (c == '\t')
            n += kIndent;
        else
            break;
    }
    return n;
}

namespace
{
/** The line without its comment and trailing space, plus the bracket balance of what is left. */
QStringView codePart(QStringView line, int& openBrackets)
{
    QChar quote;
    openBrackets = 0;
    int end = int(line.size());
    for (int i = 0; i < line.size(); ++i)
    {
        QChar c = line[i];
        if (!quote.isNull())
        {
            if (c == '\\')
                ++i;
            else if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '#')
        {
            end = i;
            break;
        }
        else if (c == '(' || c == '[' || c == '{')
            ++openBrackets;
        else if (c == ')' || c == ']' || c == '}')
            --openBrackets;
    }
    QStringView code = line.left(end);
    while (!code.isEmpty() && code.back().isSpace())
        code.chop(1);
    return code;
}
} // namespace

int indentAfter(QStringView line)
{
    int indent = leadingSpaces(line);
    int open = 0;
    QStringView code = codePart(line, open);
    if (code.endsWith(':') || open > 0)
        return indent + kIndent;
    QStringView stripped = code.trimmed();
    static const QStringList dedenters{"return", "pass", "break", "continue", "raise"};
    for (const QString& kw : dedenters)
        if (stripped == kw || (stripped.startsWith(kw) && stripped.size() > kw.size() && stripped[kw.size()] == ' '))
            return std::max(0, indent - kIndent);
    return indent;
}

QString dedent(const QString& code)
{
    QStringList lines = code.split('\n');
    int common = -1;
    for (const QString& l : lines)
    {
        if (l.trimmed().isEmpty())
            continue;
        int n = 0;
        while (n < l.size() && (l[n] == ' ' || l[n] == '\t'))
            ++n;
        common = common < 0 ? n : std::min(common, n);
    }
    if (common <= 0)
        return code;
    for (QString& l : lines)
        l = l.size() >= common ? l.mid(common) : l.trimmed();
    return lines.join('\n');
}

QStringList toggleComment(const QStringList& lines)
{
    bool allCommented = true;
    int minIndent = INT_MAX;
    for (const QString& l : lines)
    {
        if (l.trimmed().isEmpty())
            continue;
        minIndent = std::min(minIndent, leadingSpaces(l));
        if (!l.trimmed().startsWith('#'))
            allCommented = false;
    }
    if (minIndent == INT_MAX)
        return lines;

    QStringList out;
    out.reserve(lines.size());
    for (const QString& l : lines)
    {
        if (l.trimmed().isEmpty())
        {
            out.append(l);
            continue;
        }
        if (allCommented)
        {
            int hash = int(l.indexOf('#'));
            int cut = (hash + 1 < l.size() && l[hash + 1] == ' ') ? 2 : 1;
            out.append(l.left(hash) + l.mid(hash + cut));
        }
        else
            out.append(l.left(minIndent) + "# " + l.mid(minIndent));
    }
    return out;
}

bool isCellMarker(QStringView line)
{
    return line.trimmed().startsWith(u"# %%");
}

LineRange cellAt(const QStringList& lines, int line)
{
    int n = int(lines.size());
    line = std::clamp(line, 0, std::max(0, n - 1));
    int first = line;
    while (first > 0 && !isCellMarker(lines[first]))
        --first;
    int last = line + 1;
    while (last < n && !isCellMarker(lines[last]))
        ++last;
    return {first, std::max(first, last - 1)};
}

std::optional<TraceRef> findTraceRef(QStringView text)
{
    static const QRegularExpression re(R"re(File "([^"]+)", line (\d+))re");
    QRegularExpressionMatch m = re.matchView(text);
    if (!m.hasMatch())
        return std::nullopt;
    return TraceRef{m.captured(1), m.captured(2).toInt(), int(m.capturedStart(0)), int(m.capturedLength(0))};
}

bool isIdentChar(QChar c)
{
    return c.isLetterOrNumber() || c == '_';
}

int wordStart(QStringView line, int col)
{
    col = std::clamp(col, 0, int(line.size()));
    while (col > 0 && isIdentChar(line[col - 1]))
        --col;
    return col;
}

DottedPrefix dottedPrefixAt(QStringView line, int col)
{
    col = std::clamp(col, 0, int(line.size()));
    int ws = wordStart(line, col);
    DottedPrefix out{QString(), line.mid(ws, col - ws).toString()};
    int i = ws;
    while (i > 0 && line[i - 1] == '.')
    {
        int s = wordStart(line, i - 1);
        if (s == i - 1)
            break;
        i = s;
    }
    if (i < ws)
        out.base = line.mid(i, ws - 1 - i).toString();
    return out;
}

QString dottedSymbolAt(QStringView line, int col)
{
    col = std::clamp(col, 0, int(line.size()));
    int end = col;
    while (end < line.size() && isIdentChar(line[end]))
        ++end;
    DottedPrefix p = dottedPrefixAt(line, end);
    if (p.prefix.isEmpty())
        return {};
    return p.base.isEmpty() ? p.prefix : p.base + '.' + p.prefix;
}

bool isDottedName(QStringView s)
{
    static const QRegularExpression re(R"(^[A-Za-z_]\w*(\.[A-Za-z_]\w*)*$)");
    return re.matchView(s).hasMatch();
}

int matchScore(QStringView candidate, QStringView typed)
{
    if (typed.isEmpty())
        return candidate.startsWith('_') ? 3000 : 0;
    if (candidate.startsWith(typed))
        return 0 + int(candidate.size());
    if (candidate.startsWith(typed, Qt::CaseInsensitive))
        return 1000 + int(candidate.size());
    // Subsequence, case-insensitive; gaps cost.
    int gaps = 0;
    int ci = 0;
    for (QChar t : typed)
    {
        QChar lt = t.toLower();
        int start = ci;
        while (ci < candidate.size() && candidate[ci].toLower() != lt)
            ++ci;
        if (ci >= candidate.size())
            return -1;
        gaps += ci - start;
        ++ci;
    }
    if (candidate[0].toLower() != typed[0].toLower())
        gaps += 50;
    return 2000 + gaps * 10 + int(candidate.size());
}

QString pathToUri(const QString& path)
{
    return QUrl::fromLocalFile(QDir::cleanPath(path)).toString(QUrl::FullyEncoded);
}

QString uriToPath(const QString& uri)
{
    return QDir::cleanPath(QUrl(uri).toLocalFile());
}

bool sameUri(const QString& a, const QString& b)
{
    if (a == b)
        return true;
#ifdef _WIN32
    return uriToPath(a).compare(uriToPath(b), Qt::CaseInsensitive) == 0;
#else
    return uriToPath(a) == uriToPath(b);
#endif
}
} // namespace lila::ui::text
