#include "ui/editor/OutputView.h"

#include "core/Log.h"
#include "ui/editor/EditorPalette.h"
#include "ui/editor/TextUtil.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QScrollBar>
#include <QTextBlock>
#include <QUrl>

#include <vector>

namespace lila::ui
{
namespace
{
constexpr const char* kScheme = "lila-trace";
constexpr int kLevelProperty = QTextFormat::UserProperty + 1;

QColor levelColor(LogLevel lvl, bool link)
{
    const palette::Scheme& s = palette::scheme();
    if (link)
        return s.outputLink;
    return lvl == LogLevel::Error     ? s.outputError
           : lvl == LogLevel::Warning ? s.outputWarning
           : lvl == LogLevel::Info    ? s.outputInfo
           : lvl == LogLevel::Echo    ? s.outputEcho
                                      : s.outputText;
}
} // namespace

QTextCharFormat logFormat(int level)
{
    QTextCharFormat fmt;
    fmt.setForeground(levelColor(LogLevel(level), false));
    fmt.setProperty(kLevelProperty, level);
    return fmt;
}

void recolorLog(QTextDocument* doc)
{
    struct Run
    {
        int pos, length;
        QTextCharFormat format;
    };
    std::vector<Run> runs;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it)
            if (QTextFragment frag = it.fragment(); frag.charFormat().hasProperty(kLevelProperty))
                runs.push_back({frag.position(), frag.length(), frag.charFormat()});
    QTextCursor c(doc);
    for (Run& r : runs)
    {
        r.format.setForeground(levelColor(LogLevel(r.format.intProperty(kLevelProperty)), r.format.isAnchor()));
        c.setPosition(r.pos);
        c.setPosition(r.pos + r.length, QTextCursor::KeepAnchor);
        c.setCharFormat(r.format);
    }
}

OutputView::OutputView(QWidget* parent) : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setMaximumBlockCount(20000);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setMouseTracking(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    palette::onLookChanged(this, [this] { applyLook(); });
    applyLook();

    QFont f("JetBrains Mono");
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(9.5);
    setEditorFont(f);

    // Sinks fire on whatever thread logged; hop to the UI thread.
    connect(this, &OutputView::appendRequested, this, &OutputView::append, Qt::QueuedConnection);
    for (const LogEntry& e : log::since(0))
        append(int(e.level), QString::fromStdString(e.text));
    mSink = log::addSink([this](const LogEntry& e) { Q_EMIT appendRequested(int(e.level), QString::fromStdString(e.text)); });
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

void OutputView::applyLook()
{
    QPalette p = palette();
    p.setColor(QPalette::Base, palette::scheme().outputBg);
    p.setColor(QPalette::Text, palette::scheme().outputText);
    setPalette(p);

    recolorLog(document());
}

void OutputView::append(int level, const QString& text)
{
    auto lvl = LogLevel(level);
    if (lvl == LogLevel::Debug)
        return;
    // Severity reads by prefix as well as colour, so it survives the monotone look.
    QString line = text;
    if (lvl == LogLevel::Warning)
        line = "# Warning: " + text;
    else if (lvl == LogLevel::Error && !text.startsWith("Traceback") && !text.startsWith("  "))
        line = "# Error: " + text;
    else if (lvl == LogLevel::Info)
        line = "// " + text;
    if (lvl != LogLevel::Output && lvl != LogLevel::Echo && !line.endsWith('\n'))
        line += '\n';

    QTextCharFormat fmt = logFormat(level);
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
        link.setForeground(levelColor(lvl, true));
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
    Q_EMIT traceClicked(file, line);
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
