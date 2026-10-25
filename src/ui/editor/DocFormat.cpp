#include "ui/editor/DocFormat.h"

#include "core/Profile.h"
#include "ui/editor/EditorPalette.h"

#include <QFont>
#include <QFontMetrics>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <optional>
#include <vector>

namespace lila::ui::doc
{
namespace
{
enum class Section
{
    Text,
    Entries,
    Code,
};

/** Google-style section headers, and how their bodies render. */
std::optional<Section> sectionFor(const QString& header)
{
    static const QSet<QString> entries{"Args", "Arguments", "Parameters", "Params", "Keyword Args", "Keyword Arguments",
                                       "Other Parameters", "Attributes", "Raises"};
    static const QSet<QString> code{"Example", "Examples"};
    static const QSet<QString> text{"Returns", "Return", "Yields", "Note", "Notes", "See Also", "Warning", "Warnings", "Todo"};
    if (entries.contains(header))
        return Section::Entries;
    if (code.contains(header))
        return Section::Code;
    if (text.contains(header))
        return Section::Text;
    return std::nullopt;
}

int indentOf(const QString& line)
{
    int n = 0;
    while (n < line.size() && line[n] == ' ')
        ++n;
    return n;
}

QColor blend(const QColor& a, const QColor& b, double t)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t));
}

QString monoStyle(const Theme& t)
{
    return QString("font-family:'%1'").arg(t.monoFamily);
}

QString dimSpan(const QString& escaped, const Theme& t)
{
    return QString("<span style=\"color:%1\">%2</span>").arg(t.dim.name(), escaped);
}

QString codeSpan(const QString& code, const Theme& t)
{
    return QString("<span style=\"%1; background-color:%2\">%3</span>").arg(monoStyle(t), t.codeBackground.name(), codeHtml(code, t));
}

/** Plain prose; with `meta`, the "Short form x." / "Default v." / "One of ..." parts a flag's doc ends with show their values as code. */
QString proseHtml(const QString& raw, const Theme& t, bool meta)
{
    if (!meta)
        return raw.toHtmlEscaped();
    static const QRegularExpression re(R"((Short form |Default |One of )(.+?)\.(?=\s|$))");
    QString out;
    qsizetype at = 0;
    for (auto it = re.globalMatch(raw); it.hasNext();)
    {
        QRegularExpressionMatch m = it.next();
        out += raw.mid(at, m.capturedStart() - at).toHtmlEscaped();
        out += dimSpan(m.captured(1).toHtmlEscaped(), t) + codeSpan(m.captured(2), t) + ".";
        at = m.capturedEnd();
    }
    return out + raw.mid(at).toHtmlEscaped();
}

/** Text with `backtick` spans as inline code. */
QString inlineHtml(const QString& raw, const Theme& t, bool meta = false)
{
    QStringList parts = raw.split('`');
    QString out;
    for (qsizetype i = 0; i < parts.size(); ++i)
    {
        // An unclosed backtick stays literal.
        bool code = i % 2 == 1 && i + 1 < parts.size();
        if (code)
            out += codeSpan(parts[i], t);
        else
            out += (i % 2 == 1 ? "`" : "") + proseHtml(parts[i], t, meta);
    }
    return out;
}

QString severityName(Severity s)
{
    switch (s)
    {
    case Severity::Error: return "Error";
    case Severity::Warning: return "Warning";
    case Severity::Info: return "Info";
    case Severity::Hint: return "Hint";
    }
    return {};
}

/** Builds the docstring's HTML block by block. */
class DocWriter
{
public:
    DocWriter(const Theme& t, const QHash<QString, QString>& annotations) : mTheme(t), mAnnotations(annotations) {}

    QString run(const QString& doc)
    {
        const QStringList lines = doc.split('\n');
        for (const QString& rawLine : lines)
        {
            QString line = rawLine;
            while (!line.isEmpty() && line.back().isSpace())
                line.chop(1);
            feed(line);
        }
        flushAll();
        return mOut;
    }

private:
    void feed(const QString& line)
    {
        if (line.isEmpty())
        {
            flushParagraph();
            flushEntry();
            if (mSection == Section::Code && !mCode.isEmpty())
                mCode.append(QString());
            return;
        }
        const int indent = indentOf(line);
        const QString body = line.mid(indent);
        if (indent == 0 && body.endsWith(':'))
            if (std::optional<Section> s = sectionFor(body.chopped(1)))
            {
                flushAll();
                mSection = *s;
                mSectionIndent = -1;
                mOut += QString("<p style=\"margin-top:8px; margin-bottom:3px; color:%1; font-weight:bold\">%2</p>")
                            .arg(mTheme.dim.name(), body.chopped(1).toHtmlEscaped());
                return;
            }
        // A line back at the left margin ends a section's indented body.
        if (mSection != Section::Text && indent == 0)
        {
            flushAll();
            mSection = Section::Text;
        }
        if (mSectionIndent < 0)
            mSectionIndent = indent;

        if (mSection == Section::Code || body.startsWith(">>>") || body.startsWith("..."))
        {
            flushParagraph();
            mCode.append(line.mid(std::min(indent, std::max(mSectionIndent, 0))));
            return;
        }
        flushCode();
        if (mSection == Section::Entries)
        {
            static const QRegularExpression entry(R"(^(\*{0,2}[A-Za-z_]\w*)\s*(?:\(([^)]*)\))?\s*:\s*(.*)$)");
            QRegularExpressionMatch m = entry.match(body);
            if (indent <= mSectionIndent && m.hasMatch())
            {
                flushEntry();
                mEntryName = m.captured(1);
                mEntryType = m.captured(2);
                mEntryText = m.captured(3);
                mInEntry = true;
                return;
            }
            if (mInEntry)
            {
                mEntryText += (mEntryText.isEmpty() ? "" : " ") + body;
                return;
            }
        }
        bool bullet = body.startsWith("- ") || body.startsWith("* ");
        if (!mParagraph.isEmpty() && (indent != mParagraphIndent || bullet))
            flushParagraph();
        if (mParagraph.isEmpty())
            mParagraphIndent = indent;
        mParagraph += (mParagraph.isEmpty() ? "" : " ") + body;
    }

    void flushParagraph()
    {
        if (mParagraph.isEmpty())
            return;
        mOut += QString("<p style=\"margin-top:0px; margin-bottom:6px; margin-left:%1px\">%2</p>")
                    .arg(mParagraphIndent * mTheme.charWidth / 2)
                    .arg(inlineHtml(mParagraph, mTheme));
        mParagraph.clear();
    }

    void flushEntry()
    {
        if (!mInEntry)
            return;
        mInEntry = false;
        QString type = mEntryType.isEmpty() ? mAnnotations.value(mEntryName.mid(mEntryName.lastIndexOf('*') + 1)) : mEntryType;
        QString row = QString("<tr><td nowrap valign=\"top\" style=\"padding-right:10px\"><span style=\"%1; font-weight:bold\">%2</span></td>"
                              "<td valign=\"top\">")
                          .arg(monoStyle(mTheme), mEntryName.toHtmlEscaped());
        if (!type.isEmpty())
            row += QString("<span style=\"%1; color:%2\">%3</span>&nbsp;&nbsp;").arg(monoStyle(mTheme), mTheme.dim.name(), type.toHtmlEscaped());
        row += inlineHtml(mEntryText, mTheme, true) + "</td></tr>";
        mEntries += row;
    }

    void flushEntries()
    {
        flushEntry();
        if (mEntries.isEmpty())
            return;
        mOut += QString("<table cellspacing=\"0\" cellpadding=\"2\" style=\"margin-left:%1px; margin-bottom:4px\">%2</table>")
                    .arg(std::max(mSectionIndent, 0) * mTheme.charWidth / 2)
                    .arg(mEntries);
        mEntries.clear();
    }

    void flushCode()
    {
        while (!mCode.isEmpty() && mCode.back().isEmpty())
            mCode.removeLast();
        if (mCode.isEmpty())
            return;
        mOut += codeBlockHtml(mCode.join('\n'), mTheme);
        mCode.clear();
    }

    void flushAll()
    {
        flushParagraph();
        flushEntries();
        flushCode();
    }

    const Theme& mTheme;
    const QHash<QString, QString>& mAnnotations;
    QString mOut;
    Section mSection = Section::Text;
    int mSectionIndent = -1;
    QString mParagraph;
    int mParagraphIndent = 0;
    QStringList mCode;
    bool mInEntry = false;
    QString mEntryName, mEntryType, mEntryText;
    QString mEntries;
};

/** Splits a parameter list at its top-level commas. */
QStringList splitParameters(const QString& params)
{
    QStringList out;
    int depth = 0;
    QChar quote;
    qsizetype start = 0;
    for (qsizetype i = 0; i < params.size(); ++i)
    {
        QChar c = params[i];
        if (!quote.isNull())
        {
            if (c == quote)
                quote = QChar();
            continue;
        }
        if (c == '\'' || c == '"')
            quote = c;
        else if (c == '(' || c == '[' || c == '{')
            ++depth;
        else if (c == ')' || c == ']' || c == '}')
            --depth;
        else if (c == ',' && depth == 0)
        {
            out.append(params.mid(start, i - start).trimmed());
            start = i + 1;
        }
    }
    out.append(params.mid(start).trimmed());
    return out;
}
} // namespace

Theme currentTheme(const QFont& mono)
{
    using palette::gray;
    const palette::Scheme& s = palette::scheme();
    Theme t;
    t.background = gray(40);
    t.text = s.text;
    t.dim = gray(140);
    t.border = gray(74);
    t.codeBackground = s.background;
    t.error = s.squiggleError;
    t.warning = s.squiggleWarning;
    t.info = s.squiggleInfo;
    t.monoFamily = mono.family();
    t.charWidth = std::max(1, QFontMetrics(mono).horizontalAdvance(' '));
    t.syntax = syntaxFormats();
    return t;
}

QString codeHtml(const QString& code, const Theme& theme, int hlStart, int hlEnd)
{
    std::vector<const QTextCharFormat*> formats(size_t(code.size()), nullptr);
    std::vector<Token> tokens;
    int state = 0;
    qsizetype offset = 0;
    for (const QString& line : code.split('\n'))
    {
        tokens.clear();
        state = tokenizePython(line, state, tokens);
        for (const Token& tok : tokens)
            for (int i = tok.start; i < tok.start + tok.length && offset + i < code.size(); ++i)
                formats[size_t(offset + i)] = &theme.syntax.forToken(line, tok);
        offset += line.size() + 1;
    }

    auto styleAt = [&](qsizetype i) {
        const QTextCharFormat* f = formats[size_t(i)];
        const bool highlighted = i >= hlStart && i < hlEnd;
        QColor color = f && f->foreground().style() != Qt::NoBrush ? f->foreground().color() : theme.text;
        QString style = "color:" + color.name();
        if ((f && f->fontWeight() >= QFont::Bold) || highlighted)
            style += "; font-weight:bold";
        if (f && f->fontItalic())
            style += "; font-style:italic";
        if (highlighted)
            style += "; text-decoration:underline";
        return style;
    };
    QString out;
    for (qsizetype i = 0; i < code.size();)
    {
        if (code[i] == '\n')
        {
            out += "<br>";
            ++i;
            continue;
        }
        const QString style = styleAt(i);
        qsizetype j = i + 1;
        while (j < code.size() && code[j] != '\n' && styleAt(j) == style)
            ++j;
        out += QString("<span style=\"%1\">%2</span>").arg(style, code.mid(i, j - i).toHtmlEscaped());
        i = j;
    }
    return out;
}

QString codeBlockHtml(const QString& code, const Theme& theme, int hlStart, int hlEnd)
{
    return QString("<table%1 cellspacing=\"0\" cellpadding=\"5\" style=\"margin-top:2px; margin-bottom:6px\"><tr>"
                   "<td bgcolor=\"%2\"><p style=\"white-space:pre-wrap; %3; margin:0px\">%4</p></td></tr></table>")
        .arg(QString::fromLatin1(kFullWidth), theme.codeBackground.name(), monoStyle(theme), codeHtml(code, theme, hlStart, hlEnd));
}

QHash<QString, QString> parameterAnnotations(const QString& signature)
{
    QHash<QString, QString> out;
    static const QRegularExpression tag(R"(^\([\w ]+\)\s*)");
    QString sig = signature;
    sig.remove(tag);
    qsizetype open = sig.indexOf('(');
    if (open < 0)
        return out;
    int depth = 0;
    qsizetype close = -1;
    for (qsizetype i = open; i < sig.size() && close < 0; ++i)
    {
        if (sig[i] == '(')
            ++depth;
        else if (sig[i] == ')' && --depth == 0)
            close = i;
    }
    if (close < 0)
        close = sig.size();
    for (QString p : splitParameters(sig.mid(open + 1, close - open - 1)))
    {
        while (p.startsWith('*'))
            p.remove(0, 1);
        qsizetype colon = p.indexOf(':');
        if (colon <= 0)
            continue;
        QString name = p.left(colon).trimmed();
        QString annotation = p.mid(colon + 1).trimmed();
        qsizetype eq = annotation.indexOf(" = ");
        if (eq >= 0)
        {
            bool noneDefault = annotation.mid(eq + 3).trimmed() == "None";
            annotation = annotation.left(eq).trimmed();
            if (noneDefault && annotation.endsWith(" | None"))
                annotation.chop(7);
        }
        if (!name.isEmpty() && annotation != "Any")
            out.insert(name, annotation);
    }
    return out;
}

QString docstringHtml(const QString& docstring, const Theme& theme, const QHash<QString, QString>& annotations)
{
    LILA_ZONE();

    return DocWriter(theme, annotations).run(docstring);
}

QString diagnosticsHtml(const QList<Diagnostic>& diagnostics, const Theme& theme)
{
    QString out;
    for (const Diagnostic& d : diagnostics)
    {
        QColor c = d.severity == Severity::Error ? theme.error : d.severity == Severity::Warning ? theme.warning : theme.info;
        QString message;
        for (const QString& line : d.message.split('\n'))
        {
            int indent = indentOf(line);
            message += (message.isEmpty() ? "" : "<br>") + QString("&nbsp;").repeated(indent) + inlineHtml(line.mid(indent), theme);
        }
        QString origin = (d.source + " " + d.code).trimmed();
        out += QString("<table%1 cellspacing=\"0\" cellpadding=\"6\"><tr><td bgcolor=\"%2\">"
                       "<span style=\"color:%3; font-weight:bold\">%4</span>&nbsp;&nbsp;%5%6</td></tr></table>")
                   .arg(QString::fromLatin1(kFullWidth), blend(theme.background, c, 0.2).name(), c.name(), severityName(d.severity), message,
                        origin.isEmpty() ? QString() : "&nbsp;&nbsp;" + dimSpan(origin.toHtmlEscaped(), theme));
    }
    return out;
}

QString hoverHtml(const QList<Diagnostic>& diagnostics, const QString& info, const Theme& theme)
{
    LILA_ZONE();

    QString out = diagnosticsHtml(diagnostics, theme);
    QString text = info.trimmed();
    if (text.isEmpty())
        return out;
    qsizetype split = text.indexOf("\n\n");
    QString head = split >= 0 ? text.left(split) : text;
    QString body = split >= 0 ? text.mid(split + 2) : QString();

    QString inner;
    static const QRegularExpression tag(R"(^\(([\w ]+)\)\s*)");
    QRegularExpressionMatch m = tag.match(head);
    if (m.hasMatch())
    {
        inner += QString("<p style=\"margin-top:0px; margin-bottom:2px; color:%1\">%2</p>").arg(theme.dim.name(), m.captured(1).toHtmlEscaped());
        head = head.mid(m.capturedEnd());
    }
    inner += codeBlockHtml(head, theme);
    if (!body.trimmed().isEmpty())
        inner += docstringHtml(body, theme, parameterAnnotations(head));
    return out + QString("<table%1 cellspacing=\"0\" cellpadding=\"8\"><tr><td>%2</td></tr></table>").arg(QString::fromLatin1(kFullWidth), inner);
}
} // namespace lila::ui::doc
