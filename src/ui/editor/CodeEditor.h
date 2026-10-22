#pragma once

#include "ui/editor/EditorTypes.h"

#include <nlohmann/json.hpp>

#include <QHash>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTimer>

#include <array>
#include <functional>

class QLabel;

namespace lila::ui
{
class CompletionPopup;
class LspClient;
class PythonHighlighter;

namespace ruff
{
class Checker;
}

/**
 * Python source editor: gutter, highlighting, PyCharm-style editing keys, and language features from an optional
 * LspClient plus the live interpreter. Knows nothing about tabs or running code; it asks through signals.
 */
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    enum class Layer
    {
        CurrentLine,
        Occurrences,
        Search,
        Brackets,
        Diagnostics,
        Count,
    };

    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    void setEditorFont(const QFont& font);

    /** Empty for an untitled buffer, which then goes by its untitled name. */
    void setFilePath(const QString& path);
    QString filePath() const { return mPath; }
    void setUntitledName(const QString& name);
    QString untitledName() const { return mUntitledName; }
    QString displayName() const;

    /** What the language server and ruff know this buffer as; untitled buffers get a path under the temp dir. */
    QString uri() const;
    QString analysisPath() const;
    /** The filename tracebacks will show: the path, or `<Untitled-1>`. */
    QString runFilename() const;

    void setLanguageClient(LspClient* lsp);
    /** Enables lint-as-you-type and formatting. */
    void setRuffExe(const QString& exe);

    /** 0-based line and column. */
    void goToLine(int line, int col = 0);
    void goToLineDialog();
    void formatDocument();
    /** Replaces the whole text as one undo step, keeping the cursor on the same line and column. */
    void replaceAllText(const QString& text);

    /** The code Ctrl+Enter or Shift+Enter would run, and the 0-based document line it starts on. */
    QString codeToRun(bool cell, int* firstLine) const;

    void setLayer(Layer layer, const QList<QTextEdit::ExtraSelection>& selections);
    void setDiagnostics(const QString& source, const QList<Diagnostic>& list);
    QList<Diagnostic> diagnostics() const;

    int lineNumberAreaWidth() const;
    void paintGutter(QPaintEvent* e);
    bool gutterEvent(QEvent* e);

    /** Tried before the editor's own keys, so the host can claim chords like Ctrl+S. */
    std::function<bool(QKeyEvent*)> hostKeyHandler;
    /** Tried before everything but the completion popup: a modal keymap such as VimBinding. */
    std::function<bool(QKeyEvent*)> modalKeyHandler;

    /** Key presses arrive here directly, ahead of the app's global hotkeys, while the editor has focus. */
    void routeKeyPress(QKeyEvent* e) { keyPressEvent(e); }

signals:
    void runRequested();
    void runCellRequested();
    void findRequested(bool replace);
    void showDocs(const QString& symbol);
    void openLocation(const QString& path, int line, int col);
    void statusMessage(const QString& text);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    bool viewportEvent(QEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void focusOutEvent(QFocusEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    using json = nlohmann::json;

    bool handleCompletionKey(QKeyEvent* e);
    bool handleEditingKey(QKeyEvent* e);
    void afterTyping(const QString& typed);

    void newlineAndIndent();
    bool smartBackspace();
    void indentSelection(bool dedent);
    void toggleComment();
    void duplicate();
    void moveLines(int direction);
    void smartHome(bool select);
    bool autoPair(QChar c);

    void updateGutterWidth();
    void updateGutter(const QRect& rect, int dy);
    void onCursorMoved();
    void matchBrackets();
    void rebuildExtraSelections();
    void rebuildDiagnosticSelections();

    QTextCursor cursorAt(int line, int col) const;
    json position(const QTextCursor& c) const;
    json textDocumentPosition(const QTextCursor& c) const;
    bool inStringOrComment(const QTextCursor& c) const;
    void applyTextEdits(const json& edits);

    void syncDocument();
    void openInLsp();
    void scheduleLint();

    void requestCompletion(bool manual);
    void showCompletions(const json& result, bool manual, int generation);
    void acceptCompletion();
    void resolveCompletion(int entryIndex);
    void hideCompletion();

    void requestSignatureHelp();
    void showSignature(const json& result);
    void hideSignature();

    void requestHover(const QPoint& viewportPos);
    void goToDefinition(const QTextCursor& at);
    void renameSymbol();
    void requestOccurrences();
    void emitShowDocs();

    QString diagnosticsTextAt(int line, int col) const;
    QList<Diagnostic> filterInterpreterKnown(QList<Diagnostic> list) const;

    QWidget* mGutter;
    PythonHighlighter* mHighlighter;
    CompletionPopup* mPopup;
    QLabel* mSignature;
    ruff::Checker* mRuff;
    QPointer<LspClient> mLsp;

    QString mPath;
    QString mUntitledName = "Untitled";
    QString mOpenUri;
    QString mRuffExe;

    std::array<QList<QTextEdit::ExtraSelection>, size_t(Layer::Count)> mLayers;
    QHash<QString, QList<Diagnostic>> mDiagnostics;

    QTimer mSyncTimer;
    QTimer mLintTimer;
    QTimer mCompletionTimer;
    QTimer mSignatureTimer;
    QTimer mOccurrenceTimer;

    bool mDocDirty = false;
    int mCompletionRequest = -1;
    int mCompletionGeneration = 0;
    int mCompletionStart = -1;
    bool mCompletionIncomplete = false;
    bool mCompletionManual = false;
    QString mCompletionBase;
    int mSignatureRequest = -1;
    int mHoverRequest = -1;
    QPoint mHoverPos;
};
} // namespace lila::ui
