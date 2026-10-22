#include "ui/editor/VimBinding.h"

#include "ui/editor/CodeEditor.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

namespace lila::ui
{
VimBinding::VimBinding(CodeEditor* editor) : QObject(editor), mEditor(editor), mEngine(*this)
{
    mEditor->modalKeyHandler = [this](QKeyEvent* e) { return handleKey(e); };
    mEditor->viewport()->installEventFilter(this);
    refresh();
}

VimBinding::~VimBinding()
{
    // Null when the editor itself is being destroyed.
    if (!mEditor)
        return;
    mEditor->modalKeyHandler = nullptr;
    mEditor->setOverwriteMode(false);
    mEditor->setCursorWidth(2);
    QTextCursor c = mEditor->textCursor();
    c.clearSelection();
    mEditor->setTextCursor(c);
}

bool VimBinding::eventFilter(QObject* watched, QEvent* e)
{
    // A click ends visual or command-line mode, as in gvim; insert mode carries on where it lands.
    if (e->type() == QEvent::MouseButtonPress && mEngine.mode() != vim::Mode::Normal && mEngine.mode() != vim::Mode::Insert)
        reset();
    return QObject::eventFilter(watched, e);
}

void VimBinding::reset()
{
    mEngine.reset();
    refresh();
}

QString VimBinding::token(const QKeyEvent* e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    const int key = e->key();
    if (mods & (Qt::AltModifier | Qt::MetaModifier))
        return {};
    if (mods & Qt::ControlModifier)
    {
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            return QString("<C-%1>").arg(QChar('a' + (key - Qt::Key_A)));
        if (key == Qt::Key_BracketLeft)
            return "<C-[>";
        return {};
    }
    switch (key)
    {
    case Qt::Key_Escape: return "<Esc>";
    case Qt::Key_Return:
        // Shift+Enter runs the cell.
        return mods & Qt::ShiftModifier ? QString() : QStringLiteral("<CR>");
    case Qt::Key_Enter:
        // The numeric keypad's Enter runs the script.
        return QString();
    case Qt::Key_Backspace: return "<BS>";
    case Qt::Key_Delete: return "<Del>";
    case Qt::Key_Tab: return "<Tab>";
    case Qt::Key_Backtab: return "<Tab>";
    case Qt::Key_Left: return "<Left>";
    case Qt::Key_Right: return "<Right>";
    case Qt::Key_Up: return "<Up>";
    case Qt::Key_Down: return "<Down>";
    case Qt::Key_Home: return "<Home>";
    case Qt::Key_End: return "<End>";
    case Qt::Key_Insert: return "<Insert>";
    default: break;
    }
    const QString text = e->text();
    if (!text.isEmpty() && (text[0].isPrint() || text[0].isSurrogate()))
        return text;
    return {};
}

template <class Fn> void VimBinding::inGroup(Fn&& fn)
{
    QTextDocument* doc = mEditor->document();
    QTextCursor c(doc);
    const int before = doc->availableUndoSteps();
    if (mChangeDepth > 0 && mChangeHasEdits)
        c.joinPreviousEditBlock();
    else
        c.beginEditBlock();
    fn();
    c.endEditBlock();
    if (mChangeDepth > 0 && doc->availableUndoSteps() != before)
        mChangeHasEdits = true;
}

bool VimBinding::handleKey(QKeyEvent* e)
{
    if (mPassing || !mEditor)
        return false;
    const QString t = token(e);
    if (t.isEmpty())
        return false;
    if (mEngine.mode() == vim::Mode::Insert && t != "<Esc>" && t != "<C-[>")
    {
        // The editor types it, inside the change's undo group.
        mPassing = true;
        inGroup([&] { mEditor->routeKeyPress(e); });
        mPassing = false;
        // Layout waits for the edit block to close, so scrolling to the cursor does too.
        mEditor->ensureCursorVisible();
        return true;
    }
    const bool used = mEngine.key(t);
    refresh();
    return used;
}

void VimBinding::refresh()
{
    if (!mEditor)
        return;
    const vim::Mode m = mEngine.mode();
    const bool block = m == vim::Mode::Normal || m == vim::Mode::CommandLine;
    mEditor->setOverwriteMode(block);
    mEditor->setCursorWidth(block ? std::max(2, mEditor->fontMetrics().horizontalAdvance(' ')) : 2);
    mEditor->ensureCursorVisible();
    mEditor->viewport()->update();
    emit statusChanged(mEngine.status());
}

QString VimBinding::text() const
{
    return mEditor->toPlainText();
}

int VimBinding::cursor() const
{
    return mSelFrom >= 0 ? mCursor : mEditor->textCursor().position();
}

void VimBinding::setCursor(int pos)
{
    mCursor = pos;
    if (mSelFrom >= 0)
        return;
    QTextCursor c = mEditor->textCursor();
    c.setPosition(pos);
    mEditor->setTextCursor(c);
}

void VimBinding::replace(int pos, int length, const QString& with)
{
    inGroup([&] {
        QTextCursor c(mEditor->document());
        c.setPosition(pos);
        c.setPosition(pos + length, QTextCursor::KeepAnchor);
        c.insertText(with);
    });
}

void VimBinding::beginChange()
{
    if (mChangeDepth++ == 0)
        mChangeHasEdits = false;
}

void VimBinding::endChange()
{
    if (mChangeDepth > 0)
        --mChangeDepth;
}

void VimBinding::undo()
{
    const QString before = text();
    mEditor->undo();
    moveToChange(before);
}

void VimBinding::redo()
{
    const QString before = text();
    mEditor->redo();
    moveToChange(before);
}

void VimBinding::moveToChange(const QString& before)
{
    // Vim puts the cursor where the change starts; the editor leaves it after the re-inserted text.
    const QString after = text();
    const int n = int(std::min(before.size(), after.size()));
    int p = 0;
    while (p < n && before[p] == after[p])
        ++p;
    if (p < n || before.size() != after.size())
        setCursor(p);
}

void VimBinding::showSelection(int from, int to)
{
    mSelFrom = from;
    mSelTo = to;
    QTextCursor c = mEditor->textCursor();
    if (from < 0)
    {
        c.clearSelection();
        mEditor->setTextCursor(c);
        return;
    }
    // The caret sits on the end the vim cursor is on.
    if (mCursor <= from)
    {
        c.setPosition(to);
        c.setPosition(from, QTextCursor::KeepAnchor);
    }
    else
    {
        c.setPosition(from);
        c.setPosition(to, QTextCursor::KeepAnchor);
    }
    mEditor->setTextCursor(c);
}

QString VimBinding::clipboard() const
{
    return QGuiApplication::clipboard()->text();
}

void VimBinding::setClipboard(const QString& text)
{
    QGuiApplication::clipboard()->setText(text);
}

int VimBinding::halfPage() const
{
    const int line = std::max(1, mEditor->fontMetrics().lineSpacing());
    return std::max(1, mEditor->viewport()->height() / line / 2);
}
} // namespace lila::ui
