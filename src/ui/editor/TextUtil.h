#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>

#include <optional>

namespace lila::ui::text
{
constexpr int kIndent = 4;

/** Ruff counts columns in code points; Qt and LSP count UTF-16 units. */
int codepointToUtf16(QStringView line, int codepoint);
int utf16ToCodepoint(QStringView line, int utf16);

int leadingSpaces(QStringView line);

/** Indentation for the line that follows `line` when Enter is pressed at its end. */
int indentAfter(QStringView line);

/** Strips the common leading whitespace, ignoring blank lines. */
QString dedent(const QString& code);

/** Comments every line at the shallowest indent, or uncomments if all non-blank lines are comments already. */
QStringList toggleComment(const QStringList& lines);

/** Lines starting with `# %%` open a cell. Returns the [first, last] line of the cell around `line`. */
struct LineRange
{
    int first = 0;
    int last = 0;
};
bool isCellMarker(QStringView line);
LineRange cellAt(const QStringList& lines, int line);

/** A traceback frame reference: `File "<path>", line <n>`. */
struct TraceRef
{
    QString file;
    int line = 0;
    int start = 0; // span of the reference within the text
    int length = 0;
};
std::optional<TraceRef> findTraceRef(QStringView text);

bool isIdentChar(QChar c);

/** Start column of the identifier that ends at `col`. */
int wordStart(QStringView line, int col);

/** For "x = lila.cmds.po" at the end returns {"lila.cmds", "po"}; base is empty for a bare name. */
struct DottedPrefix
{
    QString base;
    QString prefix;
};
DottedPrefix dottedPrefixAt(QStringView line, int col);

/** Full dotted symbol under the cursor, extended right to the end of the identifier: "lila.cmds.polyCube". */
QString dottedSymbolAt(QStringView line, int col);

/** True if `s` is identifier(.identifier)*, which is all the interpreter is ever asked to evaluate. */
bool isDottedName(QStringView s);

/** Ranks `candidate` against a typed prefix: lower is better, -1 rejects. Prefix matches beat subsequences. */
int matchScore(QStringView candidate, QStringView typed);

/** file:/// URIs, percent-encoded the way LSP servers emit them. */
QString pathToUri(const QString& path);
QString uriToPath(const QString& uri);
bool sameUri(const QString& a, const QString& b);
} // namespace lila::ui::text
