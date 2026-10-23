#include "ui/editor/CodeEditor.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "ui/editor/CompletionPopup.h"
#include "ui/editor/EditorPalette.h"
#include "ui/editor/Interpreter.h"
#include "ui/editor/LspClient.h"
#include "ui/editor/PythonHighlighter.h"
#include "ui/editor/Ruff.h"
#include "ui/editor/TextUtil.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTextBlock>
#include <QToolTip>

#include <algorithm>

namespace lila::ui
{
using json = nlohmann::json;

namespace
{
/**
 * Hands key presses straight to a focused CodeEditor. The app-wide hotkey filter only lets plain typing through to
 * text widgets, which would swallow Ctrl+D, Alt+Up and the like; filters installed later run first.
 */
class KeyRouter : public QObject
{
public:
    static void ensure()
    {
        static KeyRouter* router = [] {
            auto* r = new KeyRouter;
            qApp->installEventFilter(r);
            return r;
        }();
        (void)router;
    }

    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress)
            return false;
        auto* editor = qobject_cast<CodeEditor*>(obj);
        if (!editor || obj != QApplication::focusWidget())
            return false;
        editor->routeKeyPress(static_cast<QKeyEvent*>(e));
        return true;
    }
};

class Gutter : public QWidget
{
public:
    explicit Gutter(CodeEditor* editor) : QWidget(editor), mEditor(editor) { setMouseTracking(true); }
    QSize sizeHint() const override { return {mEditor->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* e) override { mEditor->paintGutter(e); }
    bool event(QEvent* e) override { return mEditor->gutterEvent(e) || QWidget::event(e); }

private:
    CodeEditor* mEditor;
};

QString markupText(const json& v)
{
    if (v.is_string())
        return QString::fromStdString(v.get<std::string>());
    if (v.is_object())
        return QString::fromStdString(v.value("value", std::string()));
    if (v.is_array())
    {
        QStringList parts;
        for (const json& p : v)
            parts.append(markupText(p));
        return parts.join("\n\n");
    }
    return {};
}

QChar closerFor(QChar c)
{
    switch (c.unicode())
    {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    default: return {};
    }
}

bool isCloser(QChar c)
{
    return c == ')' || c == ']' || c == '}';
}

/** Escaped HTML for tooltips, with the leading signature block in the editor font. */
QString tooltipHtml(const QString& text, const QFont& mono)
{
    QString t = text.trimmed();
    if (t.size() > 4000)
        t = t.left(4000) + "\n...";
    qsizetype split = t.indexOf("\n\n");
    QString head = split >= 0 ? t.left(split) : t;
    QString body = split >= 0 ? t.mid(split + 2) : QString();
    QString html = QString("<pre style=\"font-family:'%1'; margin:0\">%2</pre>").arg(mono.family(), head.toHtmlEscaped());
    if (!body.isEmpty())
        html += "<p style=\"white-space:pre-wrap; margin-top:6px\">" + body.toHtmlEscaped() + "</p>";
    return "<div style=\"max-width:640px\">" + html + "</div>";
}
} // namespace

CodeEditor::CodeEditor(QWidget* parent) : QPlainTextEdit(parent)
{
    KeyRouter::ensure();

    mGutter = new Gutter(this);
    mHighlighter = new PythonHighlighter(document());
    mPopup = new CompletionPopup(this);
    mPopup->hide();
    mRuff = new ruff::Checker(this);

    mSignature = new QLabel(this, Qt::ToolTip | Qt::FramelessWindowHint);
    mSignature->setAttribute(Qt::WA_ShowWithoutActivating);
    mSignature->setTextFormat(Qt::RichText);
    mSignature->setStyleSheet("QLabel { background: #262626; color: #a0a0a0; border: 1px solid #5a5a5a; padding: 3px 6px; }");
    mSignature->hide();

    setLineWrapMode(QPlainTextEdit::NoWrap);
    setCursorWidth(2);
    setMouseTracking(true);

    QPalette p = palette();
    p.setColor(QPalette::Base, palette::background);
    p.setColor(QPalette::Text, palette::text);
    p.setColor(QPalette::Highlight, palette::selection);
    p.setColor(QPalette::HighlightedText, palette::gray(250));
    setPalette(p);

    QFont f("JetBrains Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(9.5);
    setEditorFont(f);

    for (QTimer* t : {&mSyncTimer, &mLintTimer, &mCompletionTimer, &mSignatureTimer, &mOccurrenceTimer})
        t->setSingleShot(true);
    mSyncTimer.setInterval(150);
    mLintTimer.setInterval(450);
    mCompletionTimer.setInterval(30);
    mSignatureTimer.setInterval(90);
    mOccurrenceTimer.setInterval(300);
    connect(&mSyncTimer, &QTimer::timeout, this, &CodeEditor::syncDocument);
    connect(&mLintTimer, &QTimer::timeout, this, [this] {
        if (!mRuffExe.isEmpty())
            mRuff->check(mRuffExe, toPlainText(), analysisPath());
    });
    connect(&mCompletionTimer, &QTimer::timeout, this, [this] { requestCompletion(mCompletionManual); });
    connect(&mSignatureTimer, &QTimer::timeout, this, &CodeEditor::requestSignatureHelp);
    connect(&mOccurrenceTimer, &QTimer::timeout, this, &CodeEditor::requestOccurrences);

    connect(mRuff, &ruff::Checker::finished, this, [this](const QList<Diagnostic>& d) { setDiagnostics("ruff", d); });
    connect(mPopup, &CompletionPopup::activated, this, &CodeEditor::acceptCompletion);
    connect(mPopup, &CompletionPopup::currentEntryChanged, this, &CodeEditor::resolveCompletion);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateGutterWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateGutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::onCursorMoved);
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        mDocDirty = true;
        mSyncTimer.start();
        scheduleLint();
    });

    updateGutterWidth();
    onCursorMoved();
}

CodeEditor::~CodeEditor()
{
    if (mLsp && !mOpenUri.isEmpty())
        mLsp->closeDocument(mOpenUri);
}

void CodeEditor::setEditorFont(const QFont& font)
{
    setFont(font);
    document()->setDefaultFont(font);
    mGutter->setFont(font);
    mPopup->setEditorFont(font);
    QFont sig = font;
    mSignature->setFont(sig);
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(' ') * text::kIndent);
    updateGutterWidth();
}

void CodeEditor::setFilePath(const QString& path)
{
    QString clean = path.isEmpty() ? QString() : QDir::cleanPath(path);
    if (clean == mPath)
        return;
    mPath = clean;
    if (mLsp && mLsp->isReady())
        openInLsp();
    scheduleLint();
}

void CodeEditor::setUntitledName(const QString& name)
{
    mUntitledName = name;
}

QString CodeEditor::displayName() const
{
    return mPath.isEmpty() ? mUntitledName : QFileInfo(mPath).fileName();
}

QString CodeEditor::analysisPath() const
{
    if (!mPath.isEmpty())
        return mPath;
    return QDir::cleanPath(QDir::tempPath() + "/lila-untitled/" + mUntitledName + ".py");
}

QString CodeEditor::uri() const
{
    return text::pathToUri(analysisPath());
}

QString CodeEditor::runFilename() const
{
    return mPath.isEmpty() ? "<" + mUntitledName + ">" : QDir::toNativeSeparators(mPath);
}

void CodeEditor::setLanguageClient(LspClient* lsp)
{
    if (mLsp == lsp)
        return;
    if (mLsp)
    {
        if (!mOpenUri.isEmpty())
            mLsp->closeDocument(mOpenUri);
        mLsp->disconnect(this);
    }
    mOpenUri.clear();
    mLsp = lsp;
    if (!mLsp)
        return;
    connect(mLsp, &LspClient::ready, this, &CodeEditor::openInLsp);
    connect(mLsp, &LspClient::stopped, this, [this] {
        mOpenUri.clear();
        setDiagnostics("lsp", {});
    });
    connect(mLsp, &LspClient::diagnosticsPublished, this, [this](const QString& u, const QList<Diagnostic>& d) {
        if (!mOpenUri.isEmpty() && text::sameUri(u, mOpenUri))
            setDiagnostics("lsp", filterInterpreterKnown(d));
    });
    if (mLsp->isReady())
        openInLsp();
}

void CodeEditor::setRuffExe(const QString& exe)
{
    mRuffExe = exe;
    scheduleLint();
}

void CodeEditor::openInLsp()
{
    if (!mLsp || !mLsp->isReady())
        return;
    if (!mOpenUri.isEmpty())
        mLsp->closeDocument(mOpenUri);
    mOpenUri = uri();
    mLsp->openDocument(mOpenUri, toPlainText());
    mDocDirty = false;
}

void CodeEditor::syncDocument()
{
    if (!mDocDirty || !mLsp || mOpenUri.isEmpty())
        return;
    mDocDirty = false;
    mLsp->changeDocument(mOpenUri, toPlainText());
}

void CodeEditor::scheduleLint()
{
    if (!mRuffExe.isEmpty())
        mLintTimer.start();
}

// --- Geometry and painting ---------------------------------------------------------------------------------------

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = std::max(3, int(QString::number(std::max(1, blockCount())).size()));
    return 14 + fontMetrics().horizontalAdvance('9') * digits + 10;
}

void CodeEditor::updateGutterWidth()
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateGutter(const QRect& rect, int dy)
{
    if (dy)
        mGutter->scroll(0, dy);
    else
        mGutter->update(0, rect.y(), mGutter->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        updateGutterWidth();
}

void CodeEditor::resizeEvent(QResizeEvent* e)
{
    QPlainTextEdit::resizeEvent(e);
    QRect cr = contentsRect();
    mGutter->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::paintGutter(QPaintEvent* e)
{
    LILA_ZONE();

    QPainter p(mGutter);
    p.fillRect(e->rect(), palette::gutterBg);

    // Worst severity per line for the marks.
    QHash<int, Severity> marks;
    for (const auto& list : mDiagnostics)
        for (const Diagnostic& d : list)
        {
            if (d.severity == Severity::Hint)
                continue;
            auto it = marks.find(d.line);
            if (it == marks.end() || int(d.severity) < int(it.value()))
                marks[d.line] = d.severity;
        }

    int current = textCursor().blockNumber();
    bool relative = PrefStore::get().value<bool>("scriptEditor", "/relativeLineNumbers", false);
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    int h = fontMetrics().height();
    int w = mGutter->width();
    QFont bold = mGutter->font();
    bold.setBold(true);
    while (block.isValid() && top <= e->rect().bottom())
    {
        if (block.isVisible() && bottom >= e->rect().top())
        {
            int n = block.blockNumber();
            bool cur = n == current;
            p.setFont(cur ? bold : mGutter->font());
            p.setPen(cur ? palette::gutterCurrent : palette::gutterText);
            // Relative mode (vim's hybrid): the current line keeps its number, others show their distance from it.
            int shown = relative && !cur ? std::abs(n - current) : n + 1;
            p.drawText(0, top, w - 10, h, Qt::AlignRight | Qt::AlignVCenter, QString::number(shown));
            if (auto it = marks.find(n); it != marks.end())
            {
                p.setRenderHint(QPainter::Antialiasing, true);
                QRectF dot(4, top + h / 2.0 - 3.5, 7, 7);
                if (it.value() == Severity::Error)
                {
                    p.setPen(Qt::NoPen);
                    p.setBrush(palette::squiggleError);
                }
                else if (it.value() == Severity::Warning)
                {
                    p.setPen(Qt::NoPen);
                    p.setBrush(palette::gray(140));
                }
                else
                {
                    p.setPen(QPen(palette::gray(130), 1.2));
                    p.setBrush(Qt::NoBrush);
                }
                p.drawEllipse(dot);
                p.setRenderHint(QPainter::Antialiasing, false);
            }
            if (text::isCellMarker(block.text()))
            {
                p.setPen(palette::cellRule);
                p.drawLine(0, top, w, top);
            }
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }
    p.setPen(palette::gray(44));
    p.drawLine(w - 1, e->rect().top(), w - 1, e->rect().bottom());
}

bool CodeEditor::gutterEvent(QEvent* e)
{
    if (e->type() == QEvent::ToolTip)
    {
        auto* he = static_cast<QHelpEvent*>(e);
        QTextCursor c = cursorForPosition(QPoint(0, he->pos().y()));
        QStringList lines;
        for (const auto& list : mDiagnostics)
            for (const Diagnostic& d : list)
                if (d.line == c.blockNumber() && d.severity != Severity::Hint)
                    lines.append(d.message + (d.code.isEmpty() ? "" : "  [" + d.source + " " + d.code + "]"));
        if (lines.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(he->globalPos(), lines.join('\n'), mGutter);
        return true;
    }
    if (e->type() == QEvent::MouseButtonPress)
    {
        auto* me = static_cast<QMouseEvent*>(e);
        QTextCursor c = cursorForPosition(QPoint(0, int(me->position().y())));
        c.movePosition(QTextCursor::StartOfBlock);
        c.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
        setTextCursor(c);
        setFocus();
        return true;
    }
    return false;
}

void CodeEditor::paintEvent(QPaintEvent* e)
{
    QPlainTextEdit::paintEvent(e);

    // A rule above each `# %%` cell marker.
    QPainter p(viewport());
    p.setPen(palette::cellRule);
    QTextBlock block = firstVisibleBlock();
    QPointF offset = contentOffset();
    while (block.isValid())
    {
        QRectF r = blockBoundingGeometry(block).translated(offset);
        if (r.top() > e->rect().bottom())
            break;
        if (block.isVisible() && text::isCellMarker(block.text()))
            p.drawLine(QPointF(0, r.top() + 0.5), QPointF(viewport()->width(), r.top() + 0.5));
        block = block.next();
    }
}

void CodeEditor::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier)
    {
        QFont f = font();
        double size = std::clamp(f.pointSizeF() + (e->angleDelta().y() > 0 ? 0.5 : -0.5), 6.0, 32.0);
        f.setPointSizeF(size);
        setEditorFont(f);
        e->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(e);
}

// --- Extra selections -----------------------------------------------------------------------------------------------

void CodeEditor::setLayer(Layer layer, const QList<QTextEdit::ExtraSelection>& selections)
{
    mLayers[size_t(layer)] = selections;
    rebuildExtraSelections();
}

void CodeEditor::rebuildExtraSelections()
{
    QList<QTextEdit::ExtraSelection> all;
    for (const auto& l : mLayers)
        all += l;
    setExtraSelections(all);
}

void CodeEditor::onCursorMoved()
{
    QTextEdit::ExtraSelection line;
    line.format.setBackground(palette::currentLine);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    mLayers[size_t(Layer::CurrentLine)] = {line};
    matchBrackets();
    if (PrefStore::get().value<bool>("scriptEditor", "/relativeLineNumbers", false))
        mGutter->update();

    if (mPopup->isVisible())
    {
        int pos = textCursor().position();
        QTextBlock b = textCursor().block();
        if (pos < mCompletionStart || mCompletionStart < b.position())
            hideCompletion();
    }
    if (mSignature->isVisible())
        mSignatureTimer.start();
    mLayers[size_t(Layer::Occurrences)].clear();
    mOccurrenceTimer.start();
    rebuildExtraSelections();
    mGutter->update();
}

bool CodeEditor::inStringOrComment(const QTextCursor& c) const
{
    auto* tokens = static_cast<PythonBlockData*>(c.block().userData());
    return tokens && tokens->inStringOrComment(c.positionInBlock());
}

void CodeEditor::matchBrackets()
{
    auto& layer = mLayers[size_t(Layer::Brackets)];
    layer.clear();
    QTextCursor c = textCursor();
    QTextDocument* doc = document();
    int pos = c.position();

    auto codeAt = [&](int p, QChar& ch) {
        ch = doc->characterAt(p);
        QTextBlock b = doc->findBlock(p);
        auto* tokens = static_cast<PythonBlockData*>(b.userData());
        int col = p - b.position();
        return !(tokens && tokens->inStringOrComment(col + 1));
    };

    int at = -1;
    QChar ch;
    for (int candidate : {pos - 1, pos})
    {
        if (candidate < 0)
            continue;
        QChar x;
        if (codeAt(candidate, x) && QStringView(u"()[]{}").contains(x))
        {
            at = candidate;
            ch = x;
            break;
        }
    }
    if (at < 0)
        return;

    static const QString open = "([{", close = ")]}";
    bool forward = open.contains(ch);
    QChar mine = ch;
    QChar other = forward ? close[open.indexOf(ch)] : open[close.indexOf(ch)];
    int depth = 0;
    int step = forward ? 1 : -1;
    int limit = 200000;
    int match = -1;
    for (int p = at; p >= 0 && p < doc->characterCount() && limit-- > 0; p += step)
    {
        QChar x;
        if (!codeAt(p, x))
            continue;
        if (x == mine)
            ++depth;
        else if (x == other && --depth == 0)
        {
            match = p;
            break;
        }
    }

    auto mark = [&](int p) {
        QTextEdit::ExtraSelection s;
        s.format.setBackground(palette::bracketMatch);
        s.format.setFontWeight(QFont::Bold);
        s.cursor = QTextCursor(doc);
        s.cursor.setPosition(p);
        s.cursor.setPosition(p + 1, QTextCursor::KeepAnchor);
        layer.append(s);
    };
    if (match >= 0)
    {
        mark(at);
        mark(match);
    }
}

// --- Diagnostics ------------------------------------------------------------------------------------------------------

QTextCursor CodeEditor::cursorAt(int line, int col) const
{
    QTextBlock b = document()->findBlockByNumber(std::clamp(line, 0, std::max(0, blockCount() - 1)));
    QTextCursor c(b);
    c.setPosition(b.position() + std::clamp(col, 0, std::max(0, b.length() - 1)));
    return c;
}

void CodeEditor::setDiagnostics(const QString& source, const QList<Diagnostic>& list)
{
    mDiagnostics[source] = list;
    rebuildDiagnosticSelections();
    mGutter->update();
}

QList<Diagnostic> CodeEditor::diagnostics() const
{
    QList<Diagnostic> all;
    for (const auto& l : mDiagnostics)
        all += l;
    return all;
}

QList<Diagnostic> CodeEditor::filterInterpreterKnown(QList<Diagnostic> list) const
{
    // Script tabs run in __main__, where Lila injects names (ops, lila, every command) the analyzer cannot see.
    static const QRegularExpression undefinedRe(R"re("(\w+)" is not defined)re");
    list.removeIf([this](const Diagnostic& d) {
        if (d.code == "reportUndefinedVariable")
        {
            QRegularExpressionMatch m = undefinedRe.match(d.message);
            return m.hasMatch() && interp::hasGlobal(m.captured(1));
        }
        if (d.code == "reportAttributeAccessIssue" && d.line == d.endLine)
        {
            QString line = document()->findBlockByNumber(d.line).text();
            QString symbol = text::dottedSymbolAt(line, d.col);
            return !symbol.isEmpty() && !interp::qualifiedName(symbol).isEmpty();
        }
        return false;
    });
    return list;
}

void CodeEditor::rebuildDiagnosticSelections()
{
    auto& layer = mLayers[size_t(Layer::Diagnostics)];
    layer.clear();
    for (const auto& list : mDiagnostics)
        for (const Diagnostic& d : list)
        {
            QTextEdit::ExtraSelection s;
            s.cursor = cursorAt(d.line, d.col);
            QTextCursor end = cursorAt(d.endLine, d.endCol);
            if (end.position() <= s.cursor.position())
            {
                // Zero-width ranges mark the word (or character) at that spot.
                end = s.cursor;
                end.movePosition(QTextCursor::EndOfWord);
                if (end.position() <= s.cursor.position())
                    end.movePosition(QTextCursor::NextCharacter);
            }
            s.cursor.setPosition(end.position(), QTextCursor::KeepAnchor);
            if (d.unnecessary)
                s.format.setForeground(palette::unnecessaryText);
            if (d.severity == Severity::Hint)
            {
                if (!d.unnecessary)
                    continue;
            }
            else
            {
                s.format.setUnderlineStyle(d.severity == Severity::Error ? QTextCharFormat::WaveUnderline
                                                                         : QTextCharFormat::SpellCheckUnderline);
                s.format.setUnderlineColor(d.severity == Severity::Error     ? palette::squiggleError
                                           : d.severity == Severity::Warning ? palette::squiggleWarning
                                                                             : palette::squiggleInfo);
                if (d.severity != Severity::Error)
                    s.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
            }
            layer.append(s);
        }
    rebuildExtraSelections();
}

QString CodeEditor::diagnosticsTextAt(int line, int col) const
{
    QStringList out;
    for (const auto& list : mDiagnostics)
        for (const Diagnostic& d : list)
        {
            bool after = line > d.line || (line == d.line && col >= d.col);
            bool before = line < d.endLine || (line == d.endLine && col <= std::max(d.endCol, d.col + 1));
            if (after && before && !(d.severity == Severity::Hint && !d.unnecessary))
                out.append(d.message + (d.code.isEmpty() ? "" : "  [" + d.source + " " + d.code + "]"));
        }
    return out.join('\n');
}

// --- Keys --------------------------------------------------------------------------------------------------------------

bool CodeEditor::event(QEvent* e)
{
    // Claim chords so window-level shortcuts don't steal them while typing here.
    if (e->type() == QEvent::ShortcutOverride)
    {
        auto* k = static_cast<QKeyEvent*>(e);
        Qt::KeyboardModifiers mods = k->modifiers() & ~Qt::KeypadModifier;
        bool fkey = k->key() >= Qt::Key_F1 && k->key() <= Qt::Key_F35;
        if (mods & (Qt::ControlModifier | Qt::AltModifier) || fkey || k->key() == Qt::Key_Escape)
        {
            e->accept();
            return true;
        }
    }
    return QPlainTextEdit::event(e);
}

void CodeEditor::keyPressEvent(QKeyEvent* e)
{
    if (mPopup->isVisible() && handleCompletionKey(e))
        return;
    if (modalKeyHandler && modalKeyHandler(e))
    {
        if (e->key() == Qt::Key_Escape)
            hideSignature();
        return;
    }
    if (e->key() == Qt::Key_Escape && mSignature->isVisible())
    {
        hideSignature();
        return;
    }
    if (hostKeyHandler && hostKeyHandler(e))
        return;
    if (handleEditingKey(e))
        return;

    Qt::KeyboardModifiers mods = e->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier);
    QString typed = mods == Qt::NoModifier ? e->text() : QString();
    QPlainTextEdit::keyPressEvent(e);
    if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete)
        afterTyping(QString());
    else if (!typed.isEmpty() && typed[0].isPrint())
        afterTyping(typed);
}

bool CodeEditor::handleCompletionKey(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Up: mPopup->moveSelection(-1); return true;
    case Qt::Key_Down: mPopup->moveSelection(1); return true;
    case Qt::Key_PageUp: mPopup->moveSelection(-10); return true;
    case Qt::Key_PageDown: mPopup->moveSelection(10); return true;
    case Qt::Key_Escape: hideCompletion(); return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
        if (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            return false;
        acceptCompletion();
        return true;
    default: return false;
    }
}

bool CodeEditor::handleEditingKey(QKeyEvent* e)
{
    Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    bool ctrl = mods == Qt::ControlModifier;
    int key = e->key();

    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && ctrl)
    {
        emit runRequested();
        return true;
    }
    // The numeric keypad's Enter runs, as in Maya.
    if (key == Qt::Key_Enter && (e->modifiers() & Qt::KeypadModifier) && mods == Qt::NoModifier)
    {
        emit runRequested();
        return true;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && mods == Qt::ShiftModifier)
    {
        emit runCellRequested();
        return true;
    }
    if ((key == Qt::Key_Return || key == Qt::Key_Enter) && mods == Qt::NoModifier)
    {
        hideSignature();
        newlineAndIndent();
        return true;
    }
    if (ctrl && key == Qt::Key_Space)
    {
        mCompletionManual = true;
        requestCompletion(true);
        return true;
    }
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_Space)
    {
        requestSignatureHelp();
        return true;
    }
    if (ctrl && (key == Qt::Key_Slash || key == Qt::Key_Question))
    {
        toggleComment();
        return true;
    }
    if (ctrl && key == Qt::Key_D)
    {
        duplicate();
        return true;
    }
    if (mods == Qt::AltModifier && (key == Qt::Key_Up || key == Qt::Key_Down))
    {
        moveLines(key == Qt::Key_Up ? -1 : 1);
        return true;
    }
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && (key == Qt::Key_Up || key == Qt::Key_Down))
    {
        moveLines(key == Qt::Key_Up ? -1 : 1);
        return true;
    }
    if (ctrl && (key == Qt::Key_F || key == Qt::Key_H || key == Qt::Key_R))
    {
        emit findRequested(key != Qt::Key_F);
        return true;
    }
    if (ctrl && key == Qt::Key_G)
    {
        goToLineDialog();
        return true;
    }
    if (mods == (Qt::ControlModifier | Qt::AltModifier) && key == Qt::Key_L)
    {
        formatDocument();
        return true;
    }
    if ((key == Qt::Key_F12 && mods == Qt::NoModifier) || (ctrl && key == Qt::Key_B))
    {
        goToDefinition(textCursor());
        return true;
    }
    if (key == Qt::Key_F2 && mods == Qt::NoModifier)
    {
        renameSymbol();
        return true;
    }
    if ((key == Qt::Key_F1 && mods == Qt::NoModifier) || (ctrl && key == Qt::Key_Q))
    {
        emitShowDocs();
        return true;
    }
    if (key == Qt::Key_Tab && mods == Qt::NoModifier)
    {
        QTextCursor c = textCursor();
        if (c.hasSelection() && document()->findBlock(c.selectionStart()) != document()->findBlock(c.selectionEnd()))
            indentSelection(false);
        else
        {
            int col = c.positionInBlock();
            c.insertText(QString(text::kIndent - col % text::kIndent, ' '));
        }
        return true;
    }
    if (key == Qt::Key_Backtab || (key == Qt::Key_Tab && mods == Qt::ShiftModifier))
    {
        indentSelection(true);
        return true;
    }
    if (key == Qt::Key_Backspace && mods == Qt::NoModifier && smartBackspace())
    {
        afterTyping(QString());
        return true;
    }
    if (key == Qt::Key_Home && (mods == Qt::NoModifier || mods == Qt::ShiftModifier))
    {
        smartHome(mods == Qt::ShiftModifier);
        return true;
    }
    if (mods == Qt::NoModifier || mods == Qt::ShiftModifier)
    {
        QString t = e->text();
        if (t.size() == 1 && autoPair(t[0]))
        {
            afterTyping(t);
            return true;
        }
    }
    return false;
}

void CodeEditor::newlineAndIndent()
{
    QTextCursor c = textCursor();
    c.beginEditBlock();
    if (c.hasSelection())
        c.removeSelectedText();
    QString line = c.block().text();
    int col = c.positionInBlock();
    QString left = line.left(col);
    QString right = line.mid(col);
    int indent = text::indentAfter(left);
    int base = text::leadingSpaces(left);
    // Drop spaces that would otherwise trail the cursor onto the new line.
    int strip = 0;
    while (strip < right.size() && right[strip] == ' ')
        ++strip;
    if (strip)
    {
        c.setPosition(c.position() + strip, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        right = right.mid(strip);
    }
    QString trimmedLeft = left.trimmed();
    QChar lastOpen = trimmedLeft.isEmpty() ? QChar() : trimmedLeft.back();
    if (!right.isEmpty() && closerFor(lastOpen) == right[0] && isCloser(right[0]))
    {
        // Between a bracket pair: open a line for the contents and put the closer back at the base indent.
        c.insertText("\n" + QString(base + text::kIndent, ' '));
        int keep = c.position();
        c.insertText("\n" + QString(base, ' '));
        c.setPosition(keep);
    }
    else
        c.insertText("\n" + QString(indent, ' '));
    c.endEditBlock();
    setTextCursor(c);
    ensureCursorVisible();
}

bool CodeEditor::smartBackspace()
{
    QTextCursor c = textCursor();
    if (c.hasSelection())
        return false;
    int col = c.positionInBlock();
    if (col == 0)
        return false;
    QString line = c.block().text();
    QChar prev = line[col - 1];
    if (col < line.size() && !closerFor(prev).isNull() && line[col] == closerFor(prev))
    {
        c.movePosition(QTextCursor::PreviousCharacter);
        c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, 2);
        c.removeSelectedText();
        setTextCursor(c);
        return true;
    }
    QStringView before = QStringView(line).left(col);
    if (!before.trimmed().isEmpty() || prev != ' ')
        return false;
    int target = ((col - 1) / text::kIndent) * text::kIndent;
    c.setPosition(c.block().position() + target, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    setTextCursor(c);
    return true;
}

bool CodeEditor::autoPair(QChar ch)
{
    QTextCursor c = textCursor();
    QString line = c.block().text();
    int col = c.positionInBlock();
    QChar next = col < line.size() ? line[col] : QChar();
    QChar prev = col > 0 ? line[col - 1] : QChar();

    bool quote = ch == '"' || ch == '\'';
    bool opener = ch == '(' || ch == '[' || ch == '{';

    // Typing a closer the editor already put there steps over it.
    if ((isCloser(ch) || quote) && next == ch && !c.hasSelection())
    {
        bool closesString = quote && inStringOrComment(c);
        if (isCloser(ch) || closesString)
        {
            c.movePosition(QTextCursor::NextCharacter);
            setTextCursor(c);
            return true;
        }
    }
    if (!opener && !quote)
        return false;

    if (c.hasSelection())
    {
        QString sel = c.selectedText();
        if (sel.contains(QChar::ParagraphSeparator))
            return false;
        int start = c.selectionStart();
        c.insertText(QString(ch) + sel + closerFor(ch));
        c.setPosition(start + 1);
        c.setPosition(start + 1 + int(sel.size()), QTextCursor::KeepAnchor);
        setTextCursor(c);
        return true;
    }

    bool nextOk = next.isNull() || next.isSpace() || isCloser(next) || next == ',' || next == ':' || next == ';';
    if (opener)
    {
        if (!nextOk || inStringOrComment(c))
            return false;
        c.insertText(QString(ch) + closerFor(ch));
        c.movePosition(QTextCursor::PreviousCharacter);
        setTextCursor(c);
        return true;
    }

    // Quotes. A third quote after an empty pair opens a triple-quoted string.
    if (col >= 2 && line[col - 1] == ch && line[col - 2] == ch && (col < 3 || line[col - 3] != ch))
    {
        c.insertText(QString(ch) + QString(3, ch));
        c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor, 3);
        setTextCursor(c);
        return true;
    }
    if (inStringOrComment(c))
        return false;
    // Only pair after a string prefix (f, rb, ...) or a non-word character.
    if (!prev.isNull() && text::isIdentChar(prev))
    {
        int ws = text::wordStart(line, col);
        QString word = line.mid(ws, col - ws).toLower();
        static const QStringList prefixes{"r", "b", "f", "u", "t", "rb", "br", "fr", "rf"};
        if (!prefixes.contains(word))
            return false;
    }
    if (!nextOk)
        return false;
    c.insertText(QString(ch) + ch);
    c.movePosition(QTextCursor::PreviousCharacter);
    setTextCursor(c);
    return true;
}

void CodeEditor::smartHome(bool select)
{
    QTextCursor c = textCursor();
    QString line = c.block().text();
    int first = text::leadingSpaces(line);
    first = std::min(first, int(line.size()));
    int target = c.positionInBlock() == first ? 0 : first;
    c.setPosition(c.block().position() + target, select ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor);
    setTextCursor(c);
}

namespace
{
/** First and last block touched by the selection, ignoring a selection that ends at column 0. */
std::pair<QTextBlock, QTextBlock> selectedBlocks(const QTextCursor& c, QTextDocument* doc)
{
    QTextBlock first = doc->findBlock(c.selectionStart());
    QTextBlock last = doc->findBlock(c.selectionEnd());
    if (c.hasSelection() && last != first && c.selectionEnd() == last.position())
        last = last.previous();
    return {first, last};
}
} // namespace

void CodeEditor::indentSelection(bool dedent)
{
    QTextCursor c = textCursor();
    auto [first, last] = selectedBlocks(c, document());
    QTextCursor e(document());
    e.beginEditBlock();
    for (QTextBlock b = first; b.isValid(); b = b.next())
    {
        e.setPosition(b.position());
        if (dedent)
        {
            QString t = b.text();
            int n = 0;
            while (n < text::kIndent && n < t.size() && t[n] == ' ')
                ++n;
            e.setPosition(b.position() + n, QTextCursor::KeepAnchor);
            e.removeSelectedText();
        }
        else if (!b.text().isEmpty())
            e.insertText(QString(text::kIndent, ' '));
        if (b == last)
            break;
    }
    e.endEditBlock();
    if (c.hasSelection())
    {
        // Reselect whole lines so repeated Tab keeps working on the same block.
        QTextCursor s(document());
        s.setPosition(first.position());
        s.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
        setTextCursor(s);
    }
}

void CodeEditor::toggleComment()
{
    QTextCursor c = textCursor();
    auto [first, last] = selectedBlocks(c, document());
    QStringList lines;
    for (QTextBlock b = first; b.isValid(); b = b.next())
    {
        lines.append(b.text());
        if (b == last)
            break;
    }
    QStringList out = text::toggleComment(lines);
    if (out == lines)
        return;
    int col = c.positionInBlock();
    int delta = int(out.first().size()) - int(lines.first().size());
    bool single = first == last && !c.hasSelection();

    QTextCursor e(document());
    e.beginEditBlock();
    e.setPosition(first.position());
    e.setPosition(last.position() + last.length() - 1, QTextCursor::KeepAnchor);
    e.insertText(out.join('\n'));
    e.endEditBlock();

    if (single)
    {
        // Like PyCharm: comment and step down a line, so repeated presses walk through a block.
        QTextCursor n(document()->findBlockByNumber(first.blockNumber()));
        n.setPosition(n.block().position() + std::clamp(col + delta, 0, n.block().length() - 1));
        n.movePosition(QTextCursor::NextBlock);
        setTextCursor(n);
    }
    else
    {
        QTextBlock f = document()->findBlockByNumber(first.blockNumber());
        QTextBlock l = document()->findBlockByNumber(first.blockNumber() + int(out.size()) - 1);
        QTextCursor s(document());
        s.setPosition(f.position());
        s.setPosition(l.position() + l.length() - 1, QTextCursor::KeepAnchor);
        setTextCursor(s);
    }
}

void CodeEditor::duplicate()
{
    QTextCursor c = textCursor();
    if (c.hasSelection())
    {
        int start = c.selectionStart(), end = c.selectionEnd();
        QString sel = toPlainText().mid(start, end - start);
        QTextCursor e(document());
        e.setPosition(end);
        e.insertText(sel);
        e.setPosition(end);
        e.setPosition(end + int(sel.size()), QTextCursor::KeepAnchor);
        setTextCursor(e);
        return;
    }
    QTextBlock b = c.block();
    int col = c.positionInBlock();
    QTextCursor e(document());
    e.beginEditBlock();
    e.setPosition(b.position() + b.length() - 1);
    e.insertText("\n" + b.text());
    e.endEditBlock();
    QTextCursor n(b.next());
    n.setPosition(b.next().position() + col);
    setTextCursor(n);
}

void CodeEditor::moveLines(int direction)
{
    QTextCursor c = textCursor();
    auto [first, last] = selectedBlocks(c, document());
    QTextBlock neighbor = direction < 0 ? first.previous() : last.next();
    if (!neighbor.isValid())
        return;
    int anchorOff = c.anchor() - first.position();
    int posOff = c.position() - first.position();
    int movingEnd = last.position() + last.length() - 1;
    QString all = toPlainText();
    QString moving = all.mid(first.position(), movingEnd - first.position());
    QString other = neighbor.text();

    int rangeStart, rangeEnd, newFirst;
    QString replacement;
    if (direction < 0)
    {
        rangeStart = neighbor.position();
        rangeEnd = movingEnd;
        replacement = moving + "\n" + other;
        newFirst = neighbor.position();
    }
    else
    {
        rangeStart = first.position();
        rangeEnd = neighbor.position() + neighbor.length() - 1;
        replacement = other + "\n" + moving;
        newFirst = first.position() + int(other.size()) + 1;
    }
    QTextCursor e(document());
    e.beginEditBlock();
    e.setPosition(rangeStart);
    e.setPosition(rangeEnd, QTextCursor::KeepAnchor);
    e.insertText(replacement);
    e.endEditBlock();

    QTextCursor s(document());
    s.setPosition(newFirst + anchorOff);
    s.setPosition(newFirst + posOff, QTextCursor::KeepAnchor);
    setTextCursor(s);
}

void CodeEditor::goToLine(int line, int col)
{
    QTextCursor c = cursorAt(line, col);
    setTextCursor(c);
    centerCursor();
    setFocus();
}

void CodeEditor::goToLineDialog()
{
    QTextCursor c = textCursor();
    bool ok = false;
    QString current = QString("%1:%2").arg(c.blockNumber() + 1).arg(c.positionInBlock() + 1);
    QString s = QInputDialog::getText(this, "Go to Line", QString("Line [:column] (1 - %1)").arg(blockCount()), QLineEdit::Normal,
                                      current, &ok);
    if (!ok)
        return;
    QStringList parts = s.split(':');
    int line = parts.value(0).trimmed().toInt();
    int col = parts.size() > 1 ? parts[1].trimmed().toInt() : 1;
    if (line > 0)
        goToLine(line - 1, std::max(0, col - 1));
}

void CodeEditor::replaceAllText(const QString& text)
{
    QTextCursor c = textCursor();
    int line = c.blockNumber(), col = c.positionInBlock();
    int scroll = verticalScrollBar()->value();
    QTextCursor e(document());
    e.beginEditBlock();
    e.select(QTextCursor::Document);
    e.insertText(text);
    e.endEditBlock();
    setTextCursor(cursorAt(line, col));
    verticalScrollBar()->setValue(scroll);
}

void CodeEditor::formatDocument()
{
    if (mRuffExe.isEmpty())
    {
        emit statusMessage("Formatting needs ruff, which is not installed yet");
        return;
    }
    QString source = toPlainText();
    ruff::FormatResult r = ruff::format(mRuffExe, source, analysisPath());
    if (!r.ok)
    {
        emit statusMessage("ruff format: " + r.error.section('\n', 0, 0));
        return;
    }
    // ruff always ends with a newline; keep a buffer that had none from gaining one each time.
    QString out = r.text;
    out.replace("\r\n", "\n");
    if (out != source)
    {
        replaceAllText(out);
        emit statusMessage("Formatted with ruff");
    }
}

QString CodeEditor::codeToRun(bool cell, int* firstLine) const
{
    QTextCursor c = textCursor();
    QString all = toPlainText();
    if (c.hasSelection() && !cell)
    {
        if (firstLine)
            *firstLine = document()->findBlock(c.selectionStart()).blockNumber();
        // From the start of the first line, so a partially selected line keeps its indentation for dedent().
        QTextBlock b = document()->findBlock(c.selectionStart());
        QString lead = all.mid(b.position(), c.selectionStart() - b.position());
        QString sel = all.mid(c.selectionStart(), c.selectionEnd() - c.selectionStart());
        return lead.trimmed().isEmpty() ? lead + sel : sel;
    }
    if (cell)
    {
        QStringList lines = all.split('\n');
        text::LineRange r = text::cellAt(lines, c.blockNumber());
        if (firstLine)
            *firstLine = r.first;
        return lines.mid(r.first, r.last - r.first + 1).join('\n');
    }
    if (firstLine)
        *firstLine = 0;
    return all;
}

void CodeEditor::focusOutEvent(QFocusEvent* e)
{
    hideCompletion();
    hideSignature();
    QPlainTextEdit::focusOutEvent(e);
}

void CodeEditor::mouseReleaseEvent(QMouseEvent* e)
{
    QPlainTextEdit::mouseReleaseEvent(e);
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier) && !textCursor().hasSelection())
        goToDefinition(cursorForPosition(e->position().toPoint()));
}

// --- Language features -------------------------------------------------------------------------------------------------

json CodeEditor::position(const QTextCursor& c) const
{
    return {{"line", c.blockNumber()}, {"character", c.positionInBlock()}};
}

json CodeEditor::textDocumentPosition(const QTextCursor& c) const
{
    return {{"textDocument", {{"uri", mOpenUri.toStdString()}}}, {"position", position(c)}};
}

void CodeEditor::applyTextEdits(const json& edits)
{
    if (!edits.is_array() || edits.empty())
        return;
    struct Edit
    {
        int start, end;
        QString text;
    };
    std::vector<Edit> list;
    for (const json& e : edits)
    {
        const json& r = e["range"];
        QTextCursor a = cursorAt(r["start"]["line"], r["start"]["character"]);
        QTextCursor b = cursorAt(r["end"]["line"], r["end"]["character"]);
        list.push_back({a.position(), b.position(), QString::fromStdString(e.value("newText", std::string()))});
    }
    std::sort(list.begin(), list.end(), [](const Edit& a, const Edit& b) { return a.start > b.start; });
    QTextCursor c(document());
    c.beginEditBlock();
    for (const Edit& e : list)
    {
        c.setPosition(e.start);
        c.setPosition(e.end, QTextCursor::KeepAnchor);
        c.insertText(e.text);
    }
    c.endEditBlock();
}

void CodeEditor::afterTyping(const QString& typed)
{
    QTextCursor c = textCursor();
    QChar ch = typed.isEmpty() ? QChar() : typed.back();

    if (ch == '(' || ch == ',')
        mSignatureTimer.start();
    else if (ch == ')' && mSignature->isVisible())
        mSignatureTimer.start();

    if (mPopup->isVisible())
    {
        int pos = c.position();
        QString word = toPlainText().mid(mCompletionStart, pos - mCompletionStart);
        bool stillWord = pos >= mCompletionStart && std::all_of(word.begin(), word.end(), text::isIdentChar);
        if (!stillWord || (typed.isEmpty() && pos < mCompletionStart))
        {
            hideCompletion();
            if (ch != '.')
                return;
        }
        else
        {
            if (mCompletionIncomplete && !ch.isNull())
            {
                mCompletionManual = false;
                mCompletionTimer.start();
            }
            else if (mPopup->filter(word) == 0)
                hideCompletion();
            return;
        }
    }
    if (ch.isNull())
        return;

    if (inStringOrComment(c))
        return;
    QString line = c.block().text();
    int col = c.positionInBlock();
    if (ch == '.')
    {
        // Not after a number: `1.` starts a float.
        if (col >= 2 && line[col - 2].isDigit() && text::wordStart(line, col - 1) < col - 1 &&
            line[text::wordStart(line, col - 1)].isDigit())
            return;
        mCompletionManual = false;
        mCompletionTimer.start();
        return;
    }
    if (text::isIdentChar(ch))
    {
        int ws = text::wordStart(line, col);
        if (line[ws].isDigit())
            return;
        // No suggestions while naming a new function or class.
        QString before = line.left(ws).trimmed();
        if (before.endsWith("def") || before.endsWith("class") || before.endsWith(" as") || before == "as")
            return;
        mCompletionManual = false;
        mCompletionTimer.start();
    }
}

void CodeEditor::requestCompletion(bool manual)
{
    LILA_ZONE();

    syncDocument();
    QTextCursor c = textCursor();
    QString line = c.block().text();
    int col = c.positionInBlock();
    int ws = text::wordStart(line, col);
    mCompletionStart = c.block().position() + ws;
    mCompletionBase = text::dottedPrefixAt(line, col).base;
    int generation = ++mCompletionGeneration;

    if (mLsp && mLsp->isReady() && !mOpenUri.isEmpty())
    {
        if (mCompletionRequest >= 0)
            mLsp->cancel(mCompletionRequest);
        json params = textDocumentPosition(c);
        QChar prev = col > 0 ? line[col - 1] : QChar();
        if (prev == '.' && ws == col)
            params["context"] = {{"triggerKind", 2}, {"triggerCharacter", "."}};
        else
            params["context"] = {{"triggerKind", manual ? 1 : 3}};
        mCompletionRequest = mLsp->request("textDocument/completion", params, [this, manual, generation](const json& r, const json&) {
            mCompletionRequest = -1;
            showCompletions(r, manual, generation);
        });
        if (mCompletionRequest >= 0)
            return;
    }
    showCompletions(json(), manual, generation);
}

void CodeEditor::showCompletions(const json& result, bool manual, int generation)
{
    LILA_ZONE();

    if (generation != mCompletionGeneration || !hasFocus())
        return;
    QTextCursor c = textCursor();
    int pos = c.position();
    if (pos < mCompletionStart || c.block().position() > mCompletionStart)
        return;
    QString typed = toPlainText().mid(mCompletionStart, pos - mCompletionStart);
    if (!std::all_of(typed.begin(), typed.end(), text::isIdentChar))
        return;

    QList<CompletionEntry> entries;
    QSet<QString> seen;
    const json* items = nullptr;
    mCompletionIncomplete = false;
    if (result.is_array())
        items = &result;
    else if (result.is_object())
    {
        mCompletionIncomplete = result.value("isIncomplete", false);
        if (auto it = result.find("items"); it != result.end())
            items = &*it;
    }
    if (items)
        for (const json& it : *items)
        {
            CompletionEntry e;
            e.label = QString::fromStdString(it.value("label", std::string()));
            e.kind = CompletionKind(it.value("kind", 1));
            e.detail = QString::fromStdString(it.value("detail", std::string()));
            e.documentation = markupText(it.value("documentation", json()));
            e.insertText = QString::fromStdString(it.value("insertText", it.value("label", std::string())));
            e.sortText = QString::fromStdString(it.value("sortText", it.value("label", std::string())));
            e.lspItem = it;
            seen.insert(e.label);
            entries.append(std::move(e));
        }

    // The live interpreter knows what analysis can't: commands generated at runtime, objects built by earlier runs.
    for (CompletionEntry& e : interp::complete(mCompletionBase))
        if (!seen.contains(e.label))
            entries.append(std::move(e));

    if (entries.isEmpty())
    {
        hideCompletion();
        return;
    }
    mPopup->setEntries(std::move(entries));
    int visible = mPopup->filter(typed);
    if (visible == 0 || (!manual && visible == 1 && mPopup->entries()[mPopup->currentEntry()].label == typed))
    {
        hideCompletion();
        return;
    }
    QRect r = cursorRect();
    r.moveTopLeft(viewport()->mapToGlobal(r.topLeft()));
    mPopup->showBelow(r);
    resolveCompletion(mPopup->currentEntry());
}

void CodeEditor::resolveCompletion(int index)
{
    if (index < 0 || index >= mPopup->entries().size())
        return;
    CompletionEntry& e = mPopup->entries()[index];
    if (e.resolved)
        return;
    e.resolved = true;
    if (e.fromInterpreter)
    {
        QString name = mCompletionBase.isEmpty() ? e.label : mCompletionBase + "." + e.label;
        e.documentation = interp::describe(name).section("\n\n", 1);
        mPopup->refreshDocumentation();
        return;
    }
    if (!mLsp || !mLsp->isReady() || !mLsp->capabilities().value("/completionProvider/resolveProvider"_json_pointer, false))
        return;
    int generation = mCompletionGeneration;
    QString label = e.label;
    mLsp->request("completionItem/resolve", e.lspItem, [this, generation, index, label](const json& r, const json&) {
        if (generation != mCompletionGeneration || !r.is_object() || index >= mPopup->entries().size())
            return;
        CompletionEntry& entry = mPopup->entries()[index];
        if (entry.label != label)
            return;
        entry.lspItem = r;
        if (r.contains("detail"))
            entry.detail = QString::fromStdString(r.value("detail", std::string()));
        if (r.contains("documentation"))
            entry.documentation = markupText(r["documentation"]);
        if (mPopup->currentEntry() == index)
            mPopup->refreshDocumentation();
        mPopup->update();
    });
}

void CodeEditor::acceptCompletion()
{
    int index = mPopup->currentEntry();
    if (index < 0)
    {
        hideCompletion();
        return;
    }
    CompletionEntry e = mPopup->entries()[index];
    hideCompletion();

    QTextCursor c = textCursor();
    int end = c.position();
    int start = mCompletionStart;
    QString insert = e.insertText.isEmpty() ? e.label : e.insertText;
    if (auto te = e.lspItem.find("textEdit"); te != e.lspItem.end() && te->is_object())
    {
        insert = QString::fromStdString(te->value("newText", insert.toStdString()));
        const json& range = te->contains("range") ? (*te)["range"] : te->value("replace", json::object());
        if (range.is_object() && range.contains("start"))
            start = cursorAt(range["start"]["line"], range["start"]["character"]).position();
    }
    // Ends of words already in the document after the cursor get replaced too, like PyCharm's Tab.
    QString line = c.block().text();
    int col = c.positionInBlock();
    int wordEnd = col;
    while (wordEnd < line.size() && text::isIdentChar(line[wordEnd]))
        ++wordEnd;
    QString suffix = line.mid(col, wordEnd - col);
    if (!suffix.isEmpty() && insert.endsWith(suffix))
        end += int(suffix.size());

    c.beginEditBlock();
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.insertText(insert);
    bool callable = e.kind == CompletionKind::Function || e.kind == CompletionKind::Method;
    int after = c.position();
    QChar next = document()->characterAt(after);
    if (callable && next != '(')
    {
        c.insertText("()");
        c.movePosition(QTextCursor::PreviousCharacter);
    }
    c.endEditBlock();
    setTextCursor(c);

    if (auto extra = e.lspItem.find("additionalTextEdits"); extra != e.lspItem.end())
        applyTextEdits(*extra);
    if (callable)
        mSignatureTimer.start();
}

void CodeEditor::hideCompletion()
{
    mCompletionTimer.stop();
    ++mCompletionGeneration;
    if (mLsp && mCompletionRequest >= 0)
        mLsp->cancel(mCompletionRequest);
    mCompletionRequest = -1;
    mPopup->hide();
}

void CodeEditor::requestSignatureHelp()
{
    if (!mLsp || !mLsp->isReady() || mOpenUri.isEmpty() || !hasFocus())
    {
        hideSignature();
        return;
    }
    syncDocument();
    if (mSignatureRequest >= 0)
        mLsp->cancel(mSignatureRequest);
    mSignatureRequest = mLsp->request("textDocument/signatureHelp", textDocumentPosition(textCursor()), [this](const json& r, const json&) {
        mSignatureRequest = -1;
        showSignature(r);
    });
}

void CodeEditor::showSignature(const json& r)
{
    if (!r.is_object() || !r.contains("signatures") || r["signatures"].empty() || !hasFocus())
    {
        hideSignature();
        return;
    }
    const json& sigs = r["signatures"];
    int active = std::clamp(r.value("activeSignature", 0), 0, int(sigs.size()) - 1);
    const json& sig = sigs[size_t(active)];
    QString label = QString::fromStdString(sig.value("label", std::string()));
    int activeParam = sig.value("activeParameter", r.value("activeParameter", 0));

    int hlStart = -1, hlEnd = -1;
    const json& params = sig.value("parameters", json::array());
    if (activeParam >= 0 && activeParam < int(params.size()))
    {
        const json& pl = params[size_t(activeParam)]["label"];
        if (pl.is_array() && pl.size() == 2)
        {
            hlStart = pl[0];
            hlEnd = pl[1];
        }
        else if (pl.is_string())
        {
            QString s = QString::fromStdString(pl.get<std::string>());
            hlStart = int(label.indexOf(s));
            hlEnd = hlStart >= 0 ? hlStart + int(s.size()) : -1;
        }
    }
    QString html;
    if (hlStart >= 0 && hlEnd <= label.size())
        html = label.left(hlStart).toHtmlEscaped() + "<b style=\"color:#f5f5f5\">" + label.mid(hlStart, hlEnd - hlStart).toHtmlEscaped() +
               "</b>" + label.mid(hlEnd).toHtmlEscaped();
    else
        html = label.toHtmlEscaped();
    html = "<span style=\"white-space:pre\">" + html + "</span>";
    QString doc = markupText(sig.value("documentation", json())).section("\n\n", 0, 0).simplified();
    if (!doc.isEmpty())
        html += "<br><span style=\"color:#8a8a8a\">" + (doc.size() > 160 ? doc.left(157) + "..." : doc).toHtmlEscaped() + "</span>";
    mSignature->setText(html);
    mSignature->adjustSize();

    QRect cr = cursorRect();
    QPoint pos = viewport()->mapToGlobal(QPoint(cr.left(), cr.top() - mSignature->height() - 2));
    mSignature->move(pos);
    mSignature->show();
}

void CodeEditor::hideSignature()
{
    mSignatureTimer.stop();
    mSignature->hide();
}

bool CodeEditor::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::ToolTip)
    {
        auto* he = static_cast<QHelpEvent*>(e);
        requestHover(he->pos());
        return true;
    }
    return QPlainTextEdit::viewportEvent(e);
}

void CodeEditor::requestHover(const QPoint& viewportPos)
{
    QTextCursor c = cursorForPosition(viewportPos);
    // cursorForPosition snaps to the nearest character; ignore hovering over empty space past the line end.
    QRect cr = cursorRect(c);
    if (std::abs(cr.center().x() - viewportPos.x()) > fontMetrics().horizontalAdvance('M') * 2 && c.atBlockEnd())
    {
        QToolTip::hideText();
        return;
    }
    mHoverPos = viewport()->mapToGlobal(viewportPos);
    QString diag = diagnosticsTextAt(c.blockNumber(), c.positionInBlock());
    QPoint global = mHoverPos;

    auto show = [this, diag, global](const QString& info) {
        QString combined = diag;
        if (!info.isEmpty())
            combined = combined.isEmpty() ? info : diag + "\n\n" + info;
        if (combined.isEmpty() || (QCursor::pos() - global).manhattanLength() > 12)
        {
            QToolTip::hideText();
            return;
        }
        QToolTip::showText(global, tooltipHtml(combined, font()), viewport());
    };

    if (inStringOrComment(c) || !mLsp || !mLsp->isReady() || mOpenUri.isEmpty())
    {
        QString symbol = text::dottedSymbolAt(c.block().text(), c.positionInBlock());
        show(inStringOrComment(c) ? QString() : interp::describe(symbol));
        return;
    }
    syncDocument();
    if (mHoverRequest >= 0)
        mLsp->cancel(mHoverRequest);
    mHoverRequest = mLsp->request("textDocument/hover", textDocumentPosition(c), [this, show](const json& r, const json&) {
        mHoverRequest = -1;
        show(r.is_object() ? markupText(r.value("contents", json())) : QString());
    });
}

void CodeEditor::goToDefinition(const QTextCursor& at)
{
    if (!mLsp || !mLsp->isReady() || mOpenUri.isEmpty())
    {
        emit statusMessage("Go to definition needs the language server");
        return;
    }
    syncDocument();
    mLsp->request("textDocument/definition", textDocumentPosition(at), [this](const json& r, const json&) {
        const json* loc = nullptr;
        if (r.is_array() && !r.empty())
            loc = &r[0];
        else if (r.is_object())
            loc = &r;
        if (!loc)
        {
            emit statusMessage("No definition found");
            return;
        }
        QString uri = QString::fromStdString(loc->value("uri", loc->value("targetUri", std::string())));
        const json& range = loc->contains("targetSelectionRange") ? (*loc)["targetSelectionRange"] : loc->value("range", json::object());
        int line = range.value("/start/line"_json_pointer, 0);
        int col = range.value("/start/character"_json_pointer, 0);
        if (text::sameUri(uri, mOpenUri))
            goToLine(line, col);
        else
            emit openLocation(text::uriToPath(uri), line, col);
    });
}

void CodeEditor::renameSymbol()
{
    if (!mLsp || !mLsp->isReady() || mOpenUri.isEmpty())
    {
        emit statusMessage("Rename needs the language server");
        return;
    }
    QTextCursor c = textCursor();
    QString line = c.block().text();
    int col = c.positionInBlock();
    int ws = text::wordStart(line, col);
    int we = col;
    while (we < line.size() && text::isIdentChar(line[we]))
        ++we;
    QString current = line.mid(ws, we - ws);
    if (current.isEmpty())
        return;
    bool ok = false;
    QString name = QInputDialog::getText(this, "Rename", "New name for '" + current + "':", QLineEdit::Normal, current, &ok);
    if (!ok || name.isEmpty() || name == current)
        return;
    syncDocument();
    json params = textDocumentPosition(c);
    params["newName"] = name.toStdString();
    mLsp->request("textDocument/rename", params, [this](const json& r, const json& err) {
        if (!err.is_null())
        {
            emit statusMessage("Rename failed: " + QString::fromStdString(err.value("message", std::string())));
            return;
        }
        if (!r.is_object())
            return;
        if (auto ch = r.find("changes"); ch != r.end() && ch->is_object())
            for (auto it = ch->begin(); it != ch->end(); ++it)
                if (text::sameUri(QString::fromStdString(it.key()), mOpenUri))
                    applyTextEdits(it.value());
        if (auto dc = r.find("documentChanges"); dc != r.end() && dc->is_array())
            for (const json& d : *dc)
                if (text::sameUri(QString::fromStdString(d.value("/textDocument/uri"_json_pointer, std::string())), mOpenUri))
                    applyTextEdits(d.value("edits", json::array()));
    });
}

void CodeEditor::requestOccurrences()
{
    if (!mLsp || !mLsp->isReady() || mOpenUri.isEmpty() || textCursor().hasSelection())
        return;
    QTextCursor c = textCursor();
    QString line = c.block().text();
    int col = c.positionInBlock();
    bool onWord = (col < line.size() && text::isIdentChar(line[col])) || (col > 0 && text::isIdentChar(line[col - 1]));
    if (!onWord || inStringOrComment(c))
        return;
    syncDocument();
    int pos = c.position();
    mLsp->request("textDocument/documentHighlight", textDocumentPosition(c), [this, pos](const json& r, const json&) {
        if (textCursor().position() != pos || !r.is_array() || r.size() < 2)
            return;
        QList<QTextEdit::ExtraSelection> list;
        for (const json& h : r)
        {
            QTextEdit::ExtraSelection s;
            s.format.setBackground(palette::occurrence);
            s.cursor = cursorAt(h["range"]["start"]["line"], h["range"]["start"]["character"]);
            QTextCursor end = cursorAt(h["range"]["end"]["line"], h["range"]["end"]["character"]);
            s.cursor.setPosition(end.position(), QTextCursor::KeepAnchor);
            list.append(s);
        }
        setLayer(Layer::Occurrences, list);
    });
}

void CodeEditor::emitShowDocs()
{
    QTextCursor c = textCursor();
    QString symbol = text::dottedSymbolAt(c.block().text(), c.positionInBlock());
    if (symbol.isEmpty())
        return;
    QString qualified = interp::qualifiedName(symbol);
    emit showDocs(qualified.isEmpty() ? symbol : qualified);
}
} // namespace lila::ui
