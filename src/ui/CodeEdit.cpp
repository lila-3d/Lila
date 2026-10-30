#include "ui/CodeEdit.h"

#include "ui/Theme.h"

#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace lila::ui
{
CodeEdit::CodeEdit(QWidget* parent, int lines) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    mEdit = new QPlainTextEdit(this);
    mEdit->setFont(monoFont());
    mEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    mEdit->setTabStopDistance(4 * mEdit->fontMetrics().horizontalAdvance(' '));
    mEdit->setMinimumHeight(mEdit->fontMetrics().lineSpacing() * lines + 8);
    mEdit->installEventFilter(this);
    mApply = new QPushButton("Apply", this);
    mApply->setToolTip("Recompile with this code (Ctrl+Enter)");
    mApply->setEnabled(false);
    connect(mEdit, &QPlainTextEdit::textChanged, this, [this] { mApply->setEnabled(!mEdit->isReadOnly() && mEdit->toPlainText() != mApplied); });
    connect(mApply, &QPushButton::clicked, this, &CodeEdit::apply);
    v->addWidget(mEdit);
    v->addWidget(mApply, 0, Qt::AlignRight);
}

void CodeEdit::showText(const std::string& text)
{
    QString t = QString::fromStdString(text);
    if (mApply->isEnabled() || t == mEdit->toPlainText())
        return;
    mApplied = t;
    QSignalBlocker block(mEdit);
    mEdit->setPlainText(t);
}

std::string CodeEdit::text() const { return mEdit->toPlainText().toStdString(); }

void CodeEdit::setReadOnly(bool readOnly)
{
    mEdit->setReadOnly(readOnly);
    mApply->setVisible(!readOnly);
}

bool CodeEdit::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == mEdit && e->type() == QEvent::KeyPress)
    {
        auto* k = static_cast<QKeyEvent*>(e);
        if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier))
        {
            apply();
            return true;
        }
        if (k->key() == Qt::Key_Escape && mApply->isEnabled())
        {
            mEdit->setPlainText(mApplied);
            return true;
        }
    }
    return QWidget::eventFilter(watched, e);
}

void CodeEdit::apply()
{
    QString t = mEdit->toPlainText();
    mApply->setEnabled(false);
    if (t == mApplied || mEdit->isReadOnly())
        return;
    mApplied = t;
    Q_EMIT applied(t);
}
} // namespace lila::ui
