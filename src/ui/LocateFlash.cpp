#include "ui/LocateFlash.h"

#include "core/PrefStore.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPolygonF>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::ui
{
namespace
{
struct FlashStyle
{
    int pulses = 1;
    int pulseMs = 400;
    double strength = 0.7;
    double reach = 0.05;
    QColor color;
};

class LocateOverlay : public QWidget
{
public:
    LocateOverlay(QWidget* target, const FlashStyle& style) : QWidget(target), mTarget(target), mStyle(style)
    {
        setObjectName("lilaLocateFlash");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        target->installEventFilter(this);
        auto* tick = new QTimer(this);
        tick->setInterval(16);
        connect(tick, &QTimer::timeout, this, [this] {
            if (mClock.elapsed() >= mStyle.pulses * mStyle.pulseMs)
                deleteLater();
            else
                update();
        });
        tick->start();
        restart();
    }

    void restart()
    {
        mClock.start();
        setGeometry(mTarget->rect());
        show();
        raise();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        // Content added or laid out after the flash starts would otherwise cover it.
        if (o == mTarget && (e->type() == QEvent::Resize || e->type() == QEvent::ChildAdded || e->type() == QEvent::Show))
        {
            setGeometry(mTarget->rect());
            raise();
        }
        return false;
    }

    void paintEvent(QPaintEvent*) override
    {
        double phase = std::fmod(double(mClock.elapsed()), mStyle.pulseMs) / mStyle.pulseMs;
        // The square root holds each pulse near full strength for most of its length.
        double pulse = std::sqrt(std::sin(std::numbers::pi * phase));
        QColor c = mStyle.color;
        c.setAlphaF(float(std::clamp(pulse * mStyle.strength, 0.0, 1.0)));
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        constexpr int kWidth = 4;
        p.setPen(QPen(c, kWidth));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(kWidth / 2.0, kWidth / 2.0, -kWidth / 2.0, -kWidth / 2.0), 4, 4);
        drawGlow(p, c);
    }

    /** A band inside each edge, fading to nothing towards the centre; the edges meet on mitred diagonals. */
    void drawGlow(QPainter& p, QColor c) const
    {
        QRectF r = QRectF(rect()).adjusted(4, 4, -4, -4);
        double depth = mStyle.reach * std::min(r.width(), r.height()) / 2.0;
        if (depth < 1.0)
            return;
        QRectF in = r.adjusted(depth, depth, -depth, -depth);
        QColor clear = c;
        c.setAlphaF(c.alphaF() * 0.35f);
        clear.setAlphaF(0.0f);
        auto band = [&](QPointF a, QPointF b, QPointF ib, QPointF ia, QPointF from, QPointF to) {
            QLinearGradient g(from, to);
            g.setColorAt(0.0, c);
            g.setColorAt(1.0, clear);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawPolygon(QPolygonF({a, b, ib, ia}));
        };
        band(r.topLeft(), r.topRight(), in.topRight(), in.topLeft(), {0, r.top()}, {0, in.top()});
        band(r.bottomLeft(), r.bottomRight(), in.bottomRight(), in.bottomLeft(), {0, r.bottom()}, {0, in.bottom()});
        band(r.topLeft(), r.bottomLeft(), in.bottomLeft(), in.topLeft(), {r.left(), 0}, {in.left(), 0});
        band(r.topRight(), r.bottomRight(), in.bottomRight(), in.topRight(), {r.right(), 0}, {in.right(), 0});
    }

private:
    QWidget* mTarget;
    FlashStyle mStyle;
    QElapsedTimer mClock;
};

QColor flashColor()
{
    if (!viewportDisplay::accentColors(kLocateFlashColorsPref))
        return themeShade(230);
    QColor c(QString::fromStdString(PrefStore::get().value<std::string>("general", kLocateFlashColorPref, kLocateFlashDefaultColor)));
    return c.isValid() ? c : QColor(kLocateFlashDefaultColor);
}
} // namespace

void flashLocate(QWidget* target)
{
    PrefStore& prefs = PrefStore::get();
    if (!target || !prefs.value<bool>("general", kLocateFlashPref, true))
        return;
    FlashStyle style;
    style.pulses = std::clamp(int(std::lround(prefs.value<double>("general", kLocateFlashPulsesPref, 1.0))), 1, 10);
    style.pulseMs = std::clamp(int(prefs.value<double>("general", kLocateFlashPulseSecondsPref, 0.4) * 1000.0), 100, 5000);
    style.strength = std::clamp(prefs.value<double>("general", kLocateFlashStrengthPref, 0.7), 0.0, 1.0);
    style.reach = std::clamp(prefs.value<double>("general", kLocateFlashReachPref, 0.05), 0.0, 1.0);
    style.color = flashColor();
    for (QObject* child : target->children())
        if (auto* existing = dynamic_cast<LocateOverlay*>(child))
        {
            existing->restart();
            return;
        }
    new LocateOverlay(target, style);
}
} // namespace lila::ui
