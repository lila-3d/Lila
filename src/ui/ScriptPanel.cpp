#include "ui/ScriptPanel.h"

#include "core/Log.h"
#include "python/Python.h"
#include "ui/Theme.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QVBoxLayout>

namespace lila::ui
{
ScriptPanel::ScriptPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    QFont mono = monoFont();
    mOutput = new QPlainTextEdit(this);
    mOutput->setReadOnly(true);
    mOutput->setFont(mono);
    mOutput->setMaximumBlockCount(20000);
    mOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
    mOutput->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mOutput, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu* menu = mOutput->createStandardContextMenu(pos);
        menu->addSeparator();
        menu->addAction("Clear", mOutput, &QPlainTextEdit::clear);
        menu->exec(mOutput->viewport()->mapToGlobal(pos));
        delete menu;
    });
    layout->addWidget(mOutput, 1);

    mInput = new QLineEdit(this);
    mInput->setFont(mono);
    mInput->setPlaceholderText("Python");
    mInput->installEventFilter(this);
    layout->addWidget(mInput);
    connect(mInput, &QLineEdit::returnPressed, this, &ScriptPanel::runInput);

    // Sinks fire on whatever thread logged; hop to the UI thread.
    connect(this, &ScriptPanel::appendRequested, this, &ScriptPanel::append, Qt::QueuedConnection);
    for (const LogEntry& e : log::since(0))
        append(int(e.level), QString::fromStdString(e.text));
    mSink = log::addSink([this](const LogEntry& e) { emit appendRequested(int(e.level), QString::fromStdString(e.text)); });
}

ScriptPanel::~ScriptPanel()
{
    log::removeSink(mSink);
}

void ScriptPanel::append(int level, const QString& text)
{
    auto lvl = LogLevel(level);
    if (lvl == LogLevel::Debug)
        return;
    // Grayscale: severity reads by prefix and brightness, not hue.
    QString line = text;
    if (lvl == LogLevel::Warning)
        line = "# Warning: " + text;
    else if (lvl == LogLevel::Error && !text.startsWith("Traceback") && !text.startsWith("  "))
        line = "# Error: " + text;
    else if (lvl == LogLevel::Info)
        line = "// " + text;

    QTextCursor c(mOutput->document());
    c.movePosition(QTextCursor::End);
    QTextCharFormat fmt;
    fmt.setForeground(lvl == LogLevel::Error || lvl == LogLevel::Warning ? themeShade(250)
                      : lvl == LogLevel::Info                             ? themeShade(140)
                                                                          : themeShade(205));
    if (lvl == LogLevel::Error)
        fmt.setFontWeight(QFont::Bold);
    if (lvl == LogLevel::Output || lvl == LogLevel::Echo)
        c.insertText(line, fmt);
    else
        c.insertText(line + (line.endsWith('\n') ? "" : "\n"), fmt);

    auto* bar = mOutput->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void ScriptPanel::runInput()
{
    QString code = mInput->text();
    if (code.trimmed().isEmpty())
        return;
    mHistory.append(code);
    mHistoryPos = int(mHistory.size());
    mInput->clear();
    log::write(LogLevel::Echo, code.toStdString() + "\n");
    python::run(code.toStdString(), "<prompt>");
}

bool ScriptPanel::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mInput && e->type() == QEvent::KeyPress && !mHistory.isEmpty())
    {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Up || k->key() == Qt::Key_Down)
        {
            mHistoryPos = std::clamp(mHistoryPos + (k->key() == Qt::Key_Up ? -1 : 1), 0, int(mHistory.size()));
            mInput->setText(mHistoryPos < mHistory.size() ? mHistory[mHistoryPos] : QString());
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}
} // namespace lila::ui
