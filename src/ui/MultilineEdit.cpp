#include "ui/MultilineEdit.h"

#include <QKeyEvent>
#include <QTimer>

namespace lila::ui
{
MultilineEdit::MultilineEdit(QWidget* parent) : QPlainTextEdit(parent)
{
    setTabChangesFocus(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    mPause = new QTimer(this);
    mPause->setSingleShot(true);
    mPause->setInterval(600);
    connect(mPause, &QTimer::timeout, this, &MultilineEdit::commit);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        if (hasFocus())
            mPause->start();
    });
}

void MultilineEdit::showText(const std::string& text)
{
    QString t = QString::fromStdString(text);
    if (hasFocus() || t == toPlainText())
        return;
    mCommitted = t;
    QSignalBlocker block(this);
    setPlainText(t);
}

QSize MultilineEdit::sizeHint() const
{
    QSize s = QPlainTextEdit::sizeHint();
    s.setHeight(fontMetrics().lineSpacing() * 4 + 12);
    return s;
}

void MultilineEdit::keyPressEvent(QKeyEvent* e)
{
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier))
    {
        commit();
        return;
    }
    if (e->key() == Qt::Key_Escape)
    {
        mPause->stop();
        QSignalBlocker block(this);
        setPlainText(mCommitted);
        clearFocus();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

void MultilineEdit::focusOutEvent(QFocusEvent* e)
{
    commit();
    QPlainTextEdit::focusOutEvent(e);
}

void MultilineEdit::commit()
{
    mPause->stop();
    QString t = toPlainText();
    if (t == mCommitted || isReadOnly())
        return;
    mCommitted = t;
    Q_EMIT committed(t);
}
} // namespace lila::ui
