#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>

namespace lila::ui::vim
{
enum class Mode
{
    Normal,
    Insert,
    Visual,
    VisualLine,
    CommandLine, // typing after ':', '/' or '?'
};

/** What the engine edits. Positions are character offsets into text(), lines separated by '\n'. */
class Buffer
{
public:
    virtual ~Buffer() = default;

    virtual QString text() const = 0;
    virtual int cursor() const = 0;
    virtual void setCursor(int pos) = 0;
    virtual void replace(int pos, int length, const QString& with) = 0;

    /** Every edit between the outermost begin and end, insert-mode typing included, undoes as one step. */
    virtual void beginChange() = 0;
    virtual void endChange() = 0;
    virtual void undo() = 0;
    virtual void redo() = 0;

    /** The visual-mode highlight as [from, to); from < 0 clears it. */
    virtual void showSelection(int from, int to) { (void)from, (void)to; }
    /** The "+ and "* registers. */
    virtual QString clipboard() const { return {}; }
    virtual void setClipboard(const QString& text) { (void)text; }
    /** :w and :q. */
    virtual void write() {}
    virtual void close() {}
    virtual void message(const QString& text) { (void)text; }
    /** Lines a Ctrl+D / Ctrl+U scroll moves. */
    virtual int halfPage() const { return 15; }
};

/**
 * A vim state machine over a Buffer. Keys arrive one at a time as tokens: a single character for printable keys,
 * otherwise <Esc> <CR> <BS> <Del> <Tab> <Left> <Right> <Up> <Down> <Home> <End>, and <C-x> for Ctrl+letter.
 * In insert mode only <Esc> (and <C-[>) is consumed; the caller types everything else into the buffer itself.
 */
class Engine
{
public:
    explicit Engine(Buffer& buffer);

    /** False when the key was not consumed and should go to the editor as normal. */
    bool key(const QString& token);

    Mode mode() const { return mMode; }
    /** NORMAL, INSERT, VISUAL, V-LINE, or the command line being typed, plus any pending keys. */
    QString status() const;
    /** Back to normal mode, as on focus loss or a mouse click. */
    void reset();

    /** A register's text and whether it holds whole lines; '"' is the unnamed one. */
    QString registerText(QChar name) const { return mRegisters.value(name).text; }

private:
    struct Cmd
    {
        QChar reg;
        int count = 0; // 0: none given
        QString op;    // d c y > < g~ gu gU
        QString name;  // motion, text object (iw), action, or "line" for dd / cc / yy
        QChar arg;     // f t F T r
    };
    struct Target
    {
        int pos = 0;
        bool linewise = false;
        bool inclusive = false;
    };
    struct Range
    {
        int from = 0;
        int to = 0;
        bool linewise = false;
    };
    struct Register
    {
        QString text;
        bool linewise = false;
    };
    enum class Parse
    {
        Incomplete,
        Invalid,
        Done,
    };

    bool normalKey(const QString& k);
    bool insertKey(const QString& k);
    bool visualKey(const QString& k);
    bool commandLineKey(const QString& k);

    Parse parseNormal(const QStringList& keys, Cmd& cmd) const;
    Parse parseVisual(const QStringList& keys, Cmd& cmd) const;
    Parse parseMotion(const QStringList& keys, int& i, Cmd& cmd) const;

    void execute(const Cmd& cmd, const QString* replayInsert = nullptr);
    void executeVisual(const Cmd& cmd);
    std::optional<Target> motion(const QString& name, QChar arg, int count, bool forOperator);
    std::optional<Range> textObject(const QString& name, int count, int pos) const;
    void applyOperator(const QString& op, Range r, const Cmd& cmd, const QString* replayInsert);
    void put(const Cmd& cmd, bool before);
    void joinLines(int line, int count, bool spaces = true);
    void shiftLines(int firstLine, int lastLine, int count, bool right);
    void changeCase(int from, int to, QChar how);

    void enterInsert(const QString* replayInsert, int repeat);
    void leaveInsert();
    void enterVisual(Mode m);
    void exitVisual();
    void updateSelection();
    Range visualRange() const;
    void runCommandLine(const QString& line);
    void runEx(const QString& ex);
    bool substitute(const QString& ex, int firstLine, int lastLine);
    std::optional<int> search(int from, bool forward, int count);
    void setSearch(const QString& pattern, bool forward);

    void yank(QChar reg, const QString& text, bool linewise, bool deleting);
    Register reg(QChar name) const;

    // Text helpers over mText.
    void reload();
    void edit(int pos, int length, const QString& with);
    void setCursor(int pos, bool keepWantCol = false);
    int lineStart(int pos) const;
    int lineEnd(int pos) const;
    int lineOf(int pos) const;
    int lineCount() const;
    int posOfLine(int line) const;
    int firstNonBlank(int pos) const;
    bool isEmptyLine(int line) const;
    int clampNormal(int pos) const;
    int nextWordStart(int p, bool big) const;
    int nextWordEnd(int p, bool big) const;
    int prevWordStart(int p, bool big) const;
    int prevWordEnd(int p, bool big) const;
    int matchBracket(int p) const;

    Buffer& mBuf;
    QString mText;
    int mCursor = 0;
    int mKnownCursor = -1; // where the engine last put the cursor; anything else moved it
    Mode mMode = Mode::Normal;
    Mode mCmdLineFrom = Mode::Normal;
    QStringList mPending;
    QString mCmdLine; // prefix char included: ":w", "/foo"
    int mWantCol = 0;
    int mAnchor = 0;

    QHash<QChar, Register> mRegisters;
    QChar mLastFindKind;
    QChar mLastFindChar;
    QString mSearch;
    bool mSearchForward = true;

    bool mChangeOpen = false;
    int mInsertRepeat = 1;
    QString mInsertStartText;
    bool mRecording = false; // the insert session belongs to a repeatable change
    QString mRepeatPrefix;   // o and O repeat whole lines
    Cmd mPendingChange;
    Cmd mLastChange;
    QString mLastInsert;
    bool mHaveLastChange = false;
    bool mReplaying = false;
};
} // namespace lila::ui::vim
