#include "ui/StatusOutput.h"

#include "ui/Icons.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace lila::ui
{
namespace
{
constexpr int kIconSize = 14;
constexpr int kTooltipLines = 8;

const char* prefix(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Warning: return "# Warning: ";
    case LogLevel::Error: return "# Error: ";
    case LogLevel::Info: return "// ";
    default: return "";
    }
}
} // namespace

StatusOutput::StatusOutput(std::function<void()> openOutput, std::function<bool()> outputShowing, QWidget* parent)
    : QWidget(parent), mTail(kTooltipLines), mOpen(std::move(openOutput)), mOutputShowing(std::move(outputShowing))
{
    // Sinks fire on whatever thread logged; hop to the UI thread.
    connect(this, &StatusOutput::appendRequested, this, &StatusOutput::append, Qt::QueuedConnection);
    for (const LogEntry& e : log::since(0))
        mTail.add(e.level, e.text);
    mSink = log::addSink([this](const LogEntry& e) { emit appendRequested(int(e.level), QString::fromStdString(e.text)); });
    refresh();
}

StatusOutput::~StatusOutput()
{
    log::removeSink(mSink);
}

QSize StatusOutput::sizeHint() const
{
    return {320, fontMetrics().height() + 4};
}

QSize StatusOutput::minimumSizeHint() const
{
    return {40, fontMetrics().height() + 4};
}

bool StatusOutput::shown() const
{
    return !mTail.lines().empty() && !(mOutputShowing && mOutputShowing());
}

void StatusOutput::append(int level, const QString& text)
{
    if (mTail.add(LogLevel(level), text.toStdString()))
        refresh();
}

void StatusOutput::refresh()
{
    QString tip = "<qt>Script Output: click to open<br><pre style='margin: 0'>";
    for (const OutputLine& l : mTail.lines())
    {
        QString line = (QString(prefix(l.level)) + QString::fromStdString(l.text)).toHtmlEscaped();
        tip += (l.level == LogLevel::Error ? "<b>" + line + "</b>" : line) + "<br>";
    }
    setToolTip(tip + "</pre></qt>");
    update();
}

void StatusOutput::paintEvent(QPaintEvent*)
{
    bool visible = shown();
    setCursor(visible ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (!visible)
        return;
    const OutputLine& last = mTail.lines().back();
    bool error = last.level == LogLevel::Error;
    bool warning = last.level == LogLevel::Warning;

    QPainter p(this);
    QFont f = font();
    f.setBold(error);
    p.setFont(f);
    QFontMetrics fm(f);
    int iconW = error || warning ? kIconSize + 5 : 0;
    QString text = fm.elidedText(QString::fromStdString(last.text), Qt::ElideRight, std::max(0, width() - iconW - 8));
    int total = iconW + fm.horizontalAdvance(text);
    int x = std::max(4, (width() - total) / 2);
    if (iconW)
    {
        // Grayscale UI: severity reads by the icon and weight, not hue.
        QIcon ic = icon(error ? "alert-octagon" : "alert-triangle");
        ic.paint(&p, QRect(x, (height() - kIconSize) / 2, kIconSize, kIconSize), Qt::AlignCenter, QIcon::Normal, QIcon::On);
        x += iconW;
    }
    p.setPen(error || warning ? QColor(242, 242, 242) : QColor(160, 160, 160));
    p.drawText(QRect(x, 0, width() - x, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
}

bool StatusOutput::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip && !shown())
        return true;
    return QWidget::event(e);
}

void StatusOutput::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && shown() && mOpen)
        mOpen();
    else
        QWidget::mousePressEvent(e);
}
} // namespace lila::ui
