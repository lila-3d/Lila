#include "ui/editor/OutputView.h"

#include "core/Log.h"
#include "ui/editor/EditorPalette.h"
#include "ui/editor/TextUtil.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QScrollBar>
#include <QTextBlock>
#include <QUrl>

namespace lila::ui
{
namespace
{
constexpr const char* kScheme = "lila-trace";
}

OutputView::OutputView(QWidget* parent) : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setMaximumBlockCount(20000);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMouseTracking(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QPalette p = palette();
    p.setColor(QPalette::Base, palette::gray(26));
    p.setColor(QPalette::Text, palette::gray(205));
    setPalette(p);

    QFont f("JetBrains Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(9.5);
    setEditorFont(f);

    // Sinks fire on whatever thread logged; hop to the UI thread.
    connect(this, &OutputView::appendRequested, this, &OutputView::append, Qt::QueuedConnection);
    for (const LogEntry& e : log::since(0))
        append(int(e.level), QString::fromStdString(e.text));
    mSink = log::addSink([this](const LogEntry& e) { emit appendRequested(int(e.level), QString::fromStdString(e.text)); });
}

OutputView::~OutputView()
{
    log::removeSink(mSink);
}

void OutputView::setEditorFont(const QFont& font)
{
    setFont(font);
    document()->setDefaultFont(font);
}

void OutputView::append(int level, const QString& text)
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
    if (lvl != LogLevel::Output && lvl != LogLevel::Echo && !line.endsWith('\n'))
        line += '\n';

    QTextCharFormat fmt;
    fmt.setForeground(lvl == LogLevel::Error || lvl == LogLevel::Warning ? palette::gray(250)
                      : lvl == LogLevel::Info                             ? palette::gray(135)
                      : lvl == LogLevel::Echo                             ? palette::gray(160)
                                                                          : palette::gray(208));
    if (lvl == LogLevel::Error)
        fmt.setFontWeight(QFont::Bold);

    auto* bar = verticalScrollBar();
    bool atBottom = bar->value() >= bar->maximum() - 2;

    QTextCursor c(document());
    c.movePosition(QTextCursor::End);
    QStringView rest = line;
    while (!rest.isEmpty())
    {
        auto ref = text::findTraceRef(rest);
        if (!ref)
        {
            c.insertText(rest.toString(), fmt);
            break;
        }
        c.insertText(rest.left(ref->start).toString(), fmt);
        QTextCharFormat link = fmt;
        link.setAnchor(true);
        link.setAnchorHref(QString("%1:%2:%3").arg(kScheme).arg(ref->line).arg(QString::fromUtf8(QUrl::toPercentEncoding(ref->file))));
        link.setFontUnderline(true);
        link.setForeground(palette::gray(240));
        c.insertText(rest.mid(ref->start, ref->length).toString(), link);
        rest = rest.mid(ref->start + ref->length);
    }
    if (atBottom)
        bar->setValue(bar->maximum());
}

void OutputView::mouseMoveEvent(QMouseEvent* e)
{
    QPlainTextEdit::mouseMoveEvent(e);
    viewport()->setCursor(anchorAt(e->position().toPoint()).isEmpty() ? Qt::IBeamCursor : Qt::PointingHandCursor);
}

void OutputView::mouseReleaseEvent(QMouseEvent* e)
{
    QPlainTextEdit::mouseReleaseEvent(e);
    if (e->button() != Qt::LeftButton || textCursor().hasSelection())
        return;
    QString href = anchorAt(e->position().toPoint());
    if (!href.startsWith(kScheme))
        return;
    QStringList parts = href.split(':');
    if (parts.size() < 3)
        return;
    int line = parts[1].toInt();
    QString file = QUrl::fromPercentEncoding(parts.mid(2).join(':').toUtf8());
    emit traceClicked(file, line);
}

void OutputView::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu* menu = createStandardContextMenu();
    menu->addSeparator();
    menu->addAction("Clear", this, &QPlainTextEdit::clear);
    menu->exec(e->globalPos());
    delete menu;
}
} // namespace lila::ui
