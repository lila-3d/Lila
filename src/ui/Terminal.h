#pragma once

#include "core/Export.h"

#include "ui/terminal/VtScreen.h"

#include <QAbstractScrollArea>
#include <QProcessEnvironment>
#include <QWidget>

#include <memory>

class QLineEdit;
class QTabWidget;
class QTimer;

namespace lila::ui
{
/**
 * A child process on a pseudo terminal: ConPTY on Windows, forkpty elsewhere. Output is read on a background
 * thread and delivered on the owning thread through output().
 */
class LILA_UI_API PtyProcess : public QObject
{
    Q_OBJECT

public:
    explicit PtyProcess(QObject* parent = nullptr);
    ~PtyProcess() override;

    /** commandLine is passed to the OS as is (CreateProcess on Windows, /bin/sh -c elsewhere). */
    bool start(const QString& commandLine, const QString& workingDir, const QProcessEnvironment& env, int cols, int rows,
               QString* error = nullptr);
    void write(const QByteArray& data);
    void resize(int cols, int rows);
    bool running() const;

Q_SIGNALS:
    void output(const QByteArray& data);
    void finished(int exitCode);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

/** One terminal: a VtScreen painted in the mono font, fed by a PtyProcess running the user's shell. */
class LILA_UI_API TerminalView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;

    /** Starts the shell from prefs, in the workspace root (or home). Safe to call again after it exits. */
    bool startShell();
    bool running() const;
    QString shellName() const { return mShellName; }

    void copySelection();
    void paste();
    void clearScrollback();
    void sendText(const QString& text);

    const term::VtScreen& screen() const { return mScreen; }

    /** The command line startShell() runs: pref general /terminal/shell, else pwsh, powershell or cmd. */
    static QString shellCommand();
    /**
     * The shell's environment: Lila's Python and lilabatch on PATH, its package dir on PYTHONPATH, and LILA, LILA_MCP_URL,
     * LILA_MCP_TOKEN, LILA_WORKSPACE and LILA_BIN so agents started here find Lila's MCP server.
     */
    static QProcessEnvironment shellEnvironment();

Q_SIGNALS:
    void exited(int exitCode);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* e) override;
    void focusInEvent(QFocusEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    bool focusNextPrevChild(bool) override { return false; }
    void scrollContentsBy(int, int) override;

private:
    struct Pos
    {
        int64_t line = 0; // absolute: survives lines scrolling into and out of history
        int col = 0;
        bool operator==(const Pos&) const = default;
        auto operator<=>(const Pos&) const = default;
    };

    void onOutput(const QByteArray& bytes);
    void onFinished(int exitCode);
    void updateFontMetrics();
    void updateGrid();
    void updateScrollBar();
    void send(const QByteArray& bytes);
    QByteArray keyToBytes(QKeyEvent* e) const;
    Pos cellAt(const QPoint& p) const;
    int64_t absLine(int historyIndex) const;
    int historyIndex(int64_t absLine) const;
    int firstVisibleLine() const;
    bool hasSelection() const { return mSelAnchor != mSelEnd; }
    bool selected(int64_t line, int col) const;
    void selectWord(const Pos& at);
    void scrollLines(int lines);
    void scrollToBottom();
    void repaintScreen();
    /** True when the program tracks the mouse and Shift isn't held to select locally instead. */
    bool forwardsMouse(Qt::KeyboardModifiers mods) const;
    /** Sends the event to the program if its tracking mode reports it; false when the view should handle it. */
    bool reportMouse(QMouseEvent* e, int type);
    void showFind();
    void findNext(bool backward);

    term::VtScreen mScreen;
    PtyProcess* mPty = nullptr;
    QString mShellName;
    QFont mFont, mBold, mItalic, mBoldItalic;
    qreal mCellW = 8;
    int mCellH = 16;
    int mAscent = 12;
    int mPad = 4;
    int mViewOffset = 0; // lines scrolled up from the bottom
    Pos mSelAnchor, mSelEnd;
    bool mSelecting = false;
    bool mFocused = false;
    bool mExited = false;
    bool mUpdatingBar = false;
    int mWheelAccum = 0;
    int mHeldButton = -1; // term::MouseButton reported as pressed, -1 for none
    QPoint mLastMouseCell{-1, -1};
    QTimer* mSyncTimeout = nullptr;
    QLineEdit* mFindBar = nullptr;
};

/** Dock contents: tabs of TerminalView with a "+" for another shell. The first one starts when first shown. */
class LILA_UI_API TerminalPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalPanel(QWidget* parent = nullptr);

    TerminalView* newTerminal();
    TerminalView* currentTerminal() const;

protected:
    void showEvent(QShowEvent* e) override;

private:
    void closeTab(int index);

    QTabWidget* mTabs;
};
} // namespace lila::ui
