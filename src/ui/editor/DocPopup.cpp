#include "ui/editor/DocPopup.h"

#include "core/Profile.h"

#include <QAbstractTextDocumentLayout>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>

#include <cmath>

namespace lila::ui
{
DocPopup::DocPopup(QWidget* owner) : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    mDoc.setDocumentMargin(0);
    hide();
}

void DocPopup::setHtml(const QString& html, const doc::Theme& theme, int maxWidth)
{
    LILA_ZONE();

    mBackground = theme.background;
    mBorder = theme.border;
    mText = theme.text;
    mDoc.setDefaultFont(font());

    // Full-width strips would make the ideal width the maximum, so measure without them.
    QTextDocument measure;
    measure.setDocumentMargin(0);
    measure.setDefaultFont(font());
    measure.setHtml(QString(html).remove(QString::fromLatin1(doc::kFullWidth)));
    measure.setTextWidth(maxWidth);
    int width = std::min(maxWidth, int(std::ceil(measure.idealWidth())));

    mDoc.setHtml(html);
    mDoc.setTextWidth(width);
    resize(width + 2, int(std::ceil(mDoc.size().height())) + 2);
    update();
}

void DocPopup::showAt(const QPoint& global, int flipBottom)
{
    QPoint pos = global;
    if (QScreen* screen = QGuiApplication::screenAt(global))
    {
        QRect avail = screen->availableGeometry();
        if (pos.y() + height() > avail.bottom())
            pos.setY(std::max(avail.top(), flipBottom - height()));
        pos.setX(std::clamp(pos.x(), avail.left(), std::max(avail.left(), avail.right() - width())));
    }
    move(pos);
    show();
    raise();
}

void DocPopup::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), mBackground);
    p.setPen(mBorder);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.translate(1, 1);
    QAbstractTextDocumentLayout::PaintContext ctx;
    ctx.palette.setColor(QPalette::Text, mText);
    ctx.clip = QRectF(0, 0, width() - 2, height() - 2);
    mDoc.documentLayout()->draw(&p, ctx);
}
} // namespace lila::ui
