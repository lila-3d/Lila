#include "ui/editor/FindBar.h"

#include "core/Profile.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/EditorPalette.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
constexpr int kMaxMatches = 5000;

QToolButton* toggle(QWidget* parent, const QString& text, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

QToolButton* button(QWidget* parent, const QString& text, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}
} // namespace

FindBar::FindBar(QWidget* parent) : QWidget(parent)
{
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(4, 3, 4, 3);
    grid->setHorizontalSpacing(4);
    grid->setVerticalSpacing(2);

    mFind = new QLineEdit(this);
    mFind->setPlaceholderText("Find");
    mFind->setClearButtonEnabled(true);
    mFind->installEventFilter(this);
    mCase = toggle(this, "Aa", "Match case");
    mWord = toggle(this, "W", "Whole words");
    mRegex = toggle(this, ".*", "Regular expression");
    mCount = new QLabel(this);
    mCount->setMinimumWidth(80);
    auto* prev = button(this, "Prev", "Previous match (Shift+Enter)");
    auto* next = button(this, "Next", "Next match (Enter)");
    auto* closeBtn = button(this, "x", "Close (Esc)");

    auto* findRow = new QHBoxLayout;
    findRow->setSpacing(2);
    findRow->addWidget(mFind, 1);
    findRow->addWidget(mCase);
    findRow->addWidget(mWord);
    findRow->addWidget(mRegex);
    findRow->addWidget(mCount);
    findRow->addWidget(prev);
    findRow->addWidget(next);
    findRow->addStretch();
    findRow->addWidget(closeBtn);
    grid->addLayout(findRow, 0, 0);

    mReplaceRow = new QWidget(this);
    auto* replaceRow = new QHBoxLayout(mReplaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(2);
    mReplace = new QLineEdit(mReplaceRow);
    mReplace->setPlaceholderText("Replace");
    mReplace->installEventFilter(this);
    auto* one = button(mReplaceRow, "Replace", "Replace this match");
    auto* all = button(mReplaceRow, "All", "Replace all matches");
    replaceRow->addWidget(mReplace, 1);
    replaceRow->addWidget(one);
    replaceRow->addWidget(all);
    replaceRow->addStretch();
    grid->addWidget(mReplaceRow, 1, 0);

    mRefresh.setSingleShot(true);
    mRefresh.setInterval(60);
    connect(&mRefresh, &QTimer::timeout, this, &FindBar::refresh);
    connect(mFind, &QLineEdit::textChanged, this, [this] {
        refresh();
        // Jump as you type, starting from where the current selection begins.
        if (mEditor && !mFind->text().isEmpty())
        {
            QTextCursor c = mEditor->textCursor();
            c.setPosition(c.selectionStart());
            mEditor->setTextCursor(c);
            findNext();
        }
    });
    for (QToolButton* t : {mCase, mWord, mRegex})
        connect(t, &QToolButton::toggled, this, &FindBar::refresh);
    connect(prev, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(next, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(closeBtn, &QToolButton::clicked, this, &FindBar::close);
    connect(one, &QToolButton::clicked, this, &FindBar::replaceOne);
    connect(all, &QToolButton::clicked, this, &FindBar::replaceAll);

    hide();
}

void FindBar::setEditor(CodeEditor* editor)
{
    if (mEditor == editor)
        return;
    if (mEditor)
    {
        mEditor->setLayer(CodeEditor::Layer::Search, {});
        mEditor->document()->disconnect(this);
        mEditor->disconnect(this);
    }
    mEditor = editor;
    if (mEditor)
    {
        connect(mEditor->document(), &QTextDocument::contentsChanged, this, [this] {
            if (isVisible())
                mRefresh.start();
        });
        connect(mEditor, &QPlainTextEdit::cursorPositionChanged, this, [this] {
            if (isVisible())
                mRefresh.start();
        });
        if (isVisible())
            refresh();
    }
}

void FindBar::activate(bool replace)
{
    mReplaceRow->setVisible(replace);
    show();
    if (mEditor)
    {
        QTextCursor c = mEditor->textCursor();
        QString sel = c.selectedText();
        if (!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator))
        {
            QSignalBlocker block(mFind);
            mFind->setText(mRegex->isChecked() ? QRegularExpression::escape(sel) : sel);
        }
    }
    mFind->setFocus();
    mFind->selectAll();
    refresh();
}

void FindBar::close()
{
    hide();
    if (mEditor)
    {
        mEditor->setLayer(CodeEditor::Layer::Search, {});
        mEditor->setFocus();
    }
}

QRegularExpression FindBar::pattern() const
{
    QString p = mFind->text();
    if (!mRegex->isChecked())
        p = QRegularExpression::escape(p);
    if (mWord->isChecked())
        p = "\\b" + p + "\\b";
    QRegularExpression re(p, mCase->isChecked() ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    return re;
}

QList<QTextCursor> FindBar::matches() const
{
    LILA_ZONE();

    QList<QTextCursor> out;
    if (!mEditor || mFind->text().isEmpty())
        return out;
    QRegularExpression re = pattern();
    if (!re.isValid())
        return out;
    QTextDocument* doc = mEditor->document();
    QTextCursor c(doc);
    while (out.size() < kMaxMatches)
    {
        c = doc->find(re, c);
        if (c.isNull())
            break;
        if (c.selectionStart() == c.selectionEnd())
        {
            // Empty match (like `^`): step past it or we never advance.
            if (!c.movePosition(QTextCursor::NextCharacter))
                break;
            continue;
        }
        out.append(c);
    }
    return out;
}

void FindBar::refresh()
{
    if (!mEditor)
        return;
    QRegularExpression re = pattern();
    bool invalid = !mFind->text().isEmpty() && !re.isValid();
    mFind->setStyleSheet(invalid ? "QLineEdit { border: 1px solid #d0d0d0; }" : QString());

    QList<QTextCursor> found = matches();
    QTextCursor cur = mEditor->textCursor();
    QList<QTextEdit::ExtraSelection> sels;
    int currentIndex = -1;
    for (int i = 0; i < found.size(); ++i)
    {
        QTextEdit::ExtraSelection s;
        s.cursor = found[i];
        bool current = found[i].selectionStart() == cur.selectionStart() && found[i].selectionEnd() == cur.selectionEnd();
        if (current)
            currentIndex = i;
        s.format.setBackground(current ? palette::searchCurrent : palette::searchMatch);
        s.format.setForeground(palette::gray(250));
        sels.append(s);
    }
    mEditor->setLayer(CodeEditor::Layer::Search, sels);

    if (mFind->text().isEmpty())
        mCount->clear();
    else if (invalid)
        mCount->setText("bad pattern");
    else if (found.isEmpty())
        mCount->setText("no matches");
    else if (currentIndex >= 0)
        mCount->setText(QString("%1 of %2").arg(currentIndex + 1).arg(found.size()));
    else
        mCount->setText(QString("%1 matches").arg(found.size()));
}

void FindBar::findNext(bool backwards)
{
    if (!mEditor || mFind->text().isEmpty())
        return;
    QRegularExpression re = pattern();
    if (!re.isValid())
        return;
    QTextDocument* doc = mEditor->document();
    QTextCursor from = mEditor->textCursor();
    QTextDocument::FindFlags flags;
    if (backwards)
    {
        flags |= QTextDocument::FindBackward;
        from.setPosition(from.selectionStart());
    }
    else
        from.setPosition(from.selectionEnd());
    QTextCursor hit = doc->find(re, from, flags);
    if (hit.isNull())
    {
        // Wrap around.
        QTextCursor edge(doc);
        if (backwards)
            edge.movePosition(QTextCursor::End);
        hit = doc->find(re, edge, flags);
    }
    if (!hit.isNull())
    {
        mEditor->setTextCursor(hit);
        mEditor->centerCursor();
    }
    refresh();
}

void FindBar::replaceOne()
{
    if (!mEditor)
        return;
    QRegularExpression re = pattern();
    QTextCursor c = mEditor->textCursor();
    QString sel = mEditor->document()->toPlainText().mid(c.selectionStart(), c.selectionEnd() - c.selectionStart());
    QRegularExpressionMatch m = re.match(sel);
    if (c.hasSelection() && m.hasMatch() && m.capturedLength(0) == sel.size())
    {
        QString replacement = mReplace->text();
        if (mRegex->isChecked())
            replacement = sel.replace(re, replacement);
        c.insertText(replacement);
        mEditor->setTextCursor(c);
    }
    findNext();
}

void FindBar::replaceAll()
{
    if (!mEditor)
        return;
    QList<QTextCursor> found = matches();
    if (found.isEmpty())
        return;
    QRegularExpression re = pattern();
    QTextCursor edit(mEditor->document());
    edit.beginEditBlock();
    for (auto it = found.rbegin(); it != found.rend(); ++it)
    {
        QString sel = it->selectedText();
        QString replacement = mRegex->isChecked() ? sel.replace(re, mReplace->text()) : mReplace->text();
        it->insertText(replacement);
    }
    edit.endEditBlock();
    mCount->setText(QString("replaced %1").arg(found.size()));
    mEditor->setLayer(CodeEditor::Layer::Search, {});
}

bool FindBar::eventFilter(QObject* obj, QEvent* e)
{
    if (e->type() == QEvent::KeyPress && (obj == mFind || obj == mReplace))
    {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape)
        {
            close();
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)
        {
            if (obj == mReplace)
                replaceOne();
            else
                findNext(k->modifiers() & Qt::ShiftModifier);
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}
} // namespace lila::ui
