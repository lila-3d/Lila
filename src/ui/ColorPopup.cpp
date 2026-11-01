#include "ui/ColorPopup.h"

#include <QApplication>
#include <QConicalGradient>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lila::ui::colorPopup
{
namespace
{
constexpr int kPad = 10;
constexpr int kWheel = 150;
constexpr int kBar = 14;
constexpr int kRow = 18;
constexpr double kMinKelvin = 1500.0, kMaxKelvin = 12000.0;

double toSrgb(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double toLinear(double c)
{
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

QColor displayColor(const glm::dvec3& linear)
{
    return QColor::fromRgbF(float(toSrgb(linear.x)), float(toSrgb(linear.y)), float(toSrgb(linear.z)));
}

/** The black body tint the light's own kelvin conversion gives, for the temperature bar. */
glm::dvec3 kelvinTint(double kelvin)
{
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double r = t <= 66.0 ? 255.0 : 329.698727446 * std::pow(t - 60.0, -0.1332047592);
    double g = t <= 66.0 ? 99.4708025861 * std::log(t) - 161.1195681661 : 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    double b = t >= 66.0 ? 255.0 : t <= 19.0 ? 0.0 : 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    glm::dvec3 out{toLinear(r / 255.0), toLinear(g / 255.0), toLinear(b / 255.0)};
    return out / std::max({out.x, out.y, out.z, 1e-6});
}

class Popup final : public QWidget
{
public:
    explicit Popup(Options o) : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), mOpts(std::move(o))
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setMouseTracking(true);
        mStart = mOpts.value;
        // Colours past 1 keep their gain: the wheel edits the colour scaled into 0..1.
        mGain = std::max({mStart.color.x, mStart.color.y, mStart.color.z, 1.0});
        const glm::dvec3 c = mStart.color / mGain;
        QColor::fromRgbF(float(toSrgb(c.x)), float(toSrgb(c.y)), float(toSrgb(c.z))).getHsvF(&mH, &mS, &mV);
        mH = std::max(mH, 0.0f);
        const int h = kPad + kWheel + kPad + kRow + (mOpts.temperature ? kRow + 6 : 0) + kPad + (mOpts.title.isEmpty() ? 0 : kRow);
        setFixedSize(kPad + kWheel + kPad + kBar + kPad, h);
    }

    ~Popup() override
    {
        if (mOpts.closed)
            mOpts.closed(mAccepted ? current() : mStart, mAccepted);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Window));
        p.setPen(pal.color(QPalette::Mid));
        p.drawRect(rect().adjusted(0, 0, -1, -1));

        int y = kPad;
        if (!mOpts.title.isEmpty())
        {
            p.setPen(pal.color(QPalette::WindowText));
            p.drawText(QRect(kPad, y - 4, width() - 2 * kPad, kRow), Qt::AlignLeft | Qt::AlignVCenter, mOpts.title);
            y += kRow;
        }
        mWheelRect = QRectF(kPad, y, kWheel, kWheel);
        drawWheel(p);
        mBarRect = QRectF(kPad + kWheel + kPad, y, kBar, kWheel);
        drawValueBar(p);
        y += kWheel + kPad;

        // Old and new swatches; clicking the old one puts it back.
        const double half = (width() - 2 * kPad) * 0.5;
        mOldRect = QRectF(kPad, y, half, kRow);
        mNewRect = QRectF(kPad + half, y, half, kRow);
        p.setPen(Qt::NoPen);
        p.fillRect(mOldRect, displayColor(effective(mStart)));
        p.fillRect(mNewRect, displayColor(effective(current())));
        y += kRow + 6;

        if (mOpts.temperature)
        {
            mKelvinBox = QRectF(kPad, y + 3, kRow - 6, kRow - 6);
            p.setPen(pal.color(QPalette::Mid));
            p.setBrush(mUseTemp ? pal.color(QPalette::Highlight) : pal.color(QPalette::Base));
            p.drawRect(mKelvinBox);
            if (mUseTemp)
            {
                p.setPen(QPen(pal.color(QPalette::HighlightedText), 1.6));
                const QRectF b = mKelvinBox.adjusted(2.5, 2.5, -2.5, -2.5);
                p.drawPolyline(QPolygonF({QPointF(b.left(), b.center().y()), QPointF(b.left() + b.width() * 0.4, b.bottom()), b.topRight()}));
            }
            mKelvinRect = QRectF(kPad + kRow, y, width() - 2 * kPad - kRow, kRow);
            QLinearGradient g(mKelvinRect.topLeft(), mKelvinRect.topRight());
            for (int i = 0; i <= 8; ++i)
                g.setColorAt(i / 8.0, displayColor(kelvinTint(kMinKelvin + (kMaxKelvin - kMinKelvin) * i / 8.0)));
            p.setOpacity(mUseTemp ? 1.0 : 0.35);
            p.fillRect(mKelvinRect, g);
            const double x = mKelvinRect.left() + mKelvinRect.width() * (std::clamp(mKelvin, kMinKelvin, kMaxKelvin) - kMinKelvin) /
                                                       (kMaxKelvin - kMinKelvin);
            p.setPen(QPen(Qt::black, 3));
            p.drawLine(QPointF(x, mKelvinRect.top()), QPointF(x, mKelvinRect.bottom()));
            p.setPen(QPen(Qt::white, 1));
            p.drawLine(QPointF(x, mKelvinRect.top()), QPointF(x, mKelvinRect.bottom()));
            p.setPen(Qt::black);
            p.drawText(mKelvinRect.adjusted(4, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter, QString("%1 K").arg(std::lround(mKelvin)));
            p.setOpacity(1.0);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF at = e->position();
        if (!rect().contains(at.toPoint()))
        {
            close();
            return;
        }
        if (mOldRect.contains(at))
        {
            setFrom(mStart);
            report();
            return;
        }
        if (mOpts.temperature && (mKelvinBox.contains(at) || QRectF(0, mKelvinBox.top(), kPad + kRow, kRow).contains(at)))
        {
            mUseTemp = !mUseTemp;
            report();
            return;
        }
        mGrab = mWheelRect.adjusted(-4, -4, 4, 4).contains(at) ? Grab::Wheel
                : mBarRect.adjusted(-4, 0, 4, 0).contains(at)  ? Grab::Value
                : mOpts.temperature && mKelvinRect.contains(at) ? Grab::Kelvin
                                                                : Grab::None;
        mouseMoveEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || mGrab == Grab::None)
            return;
        const QPointF at = e->position();
        switch (mGrab)
        {
        case Grab::Wheel:
        {
            const QPointF d = at - mWheelRect.center();
            const double r = mWheelRect.width() * 0.5;
            mH = float(std::fmod(std::atan2(-d.y(), d.x()) / (2.0 * std::numbers::pi) + 1.0, 1.0));
            mS = float(std::clamp(std::hypot(d.x(), d.y()) / r, 0.0, 1.0));
            break;
        }
        case Grab::Value: mV = float(std::clamp(1.0 - (at.y() - mBarRect.top()) / mBarRect.height(), 0.0, 1.0)); break;
        case Grab::Kelvin:
            mUseTemp = true;
            mKelvin = kMinKelvin + (kMaxKelvin - kMinKelvin) * std::clamp((at.x() - mKelvinRect.left()) / mKelvinRect.width(), 0.0, 1.0);
            // Shift steps by 100 K.
            if (e->modifiers() & Qt::ShiftModifier)
                mKelvin = std::round(mKelvin / 100.0) * 100.0;
            break;
        case Grab::None: break;
        }
        report();
    }

    void mouseReleaseEvent(QMouseEvent*) override { mGrab = Grab::None; }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
        {
            mAccepted = false;
            if (mOpts.changed && current() != mStart)
                mOpts.changed(mStart);
            close();
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
        {
            close();
            return;
        }
        QWidget::keyPressEvent(e);
    }

private:
    enum class Grab
    {
        None,
        Wheel,
        Value,
        Kelvin,
    };

    static glm::dvec3 effective(const Value& v) { return v.useTemperature ? v.color * kelvinTint(v.temperature) : v.color; }

    Value current() const
    {
        Value v;
        float r, g, b;
        QColor::fromHsvF(mH, mS, mV).getRgbF(&r, &g, &b);
        v.color = glm::dvec3(toLinear(r), toLinear(g), toLinear(b)) * mGain;
        v.useTemperature = mUseTemp;
        v.temperature = mKelvin;
        return v;
    }

    void setFrom(const Value& v)
    {
        const glm::dvec3 c = v.color / mGain;
        QColor::fromRgbF(float(toSrgb(c.x)), float(toSrgb(c.y)), float(toSrgb(c.z))).getHsvF(&mH, &mS, &mV);
        mH = std::max(mH, 0.0f);
        mUseTemp = v.useTemperature;
        mKelvin = v.temperature;
    }

    void report()
    {
        update();
        if (mOpts.changed)
            mOpts.changed(current());
    }

    void drawWheel(QPainter& p)
    {
        const QPointF c = mWheelRect.center();
        const double r = mWheelRect.width() * 0.5;
        QConicalGradient hue(c, 0.0);
        for (int i = 0; i <= 6; ++i)
            hue.setColorAt(i / 6.0, QColor::fromHsvF(float(i % 6) / 6.0f, 1.0f, 1.0f));
        p.setPen(Qt::NoPen);
        p.setBrush(hue);
        p.drawEllipse(c, r, r);
        QRadialGradient white(c, r);
        white.setColorAt(0.0, Qt::white);
        white.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.setBrush(white);
        p.drawEllipse(c, r, r);
        p.setBrush(QColor(0, 0, 0, int(std::lround((1.0 - mV) * 255))));
        p.drawEllipse(c, r, r);

        const double a = mH * 2.0 * std::numbers::pi;
        const QPointF m = c + QPointF(std::cos(a), -std::sin(a)) * (mS * r);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Qt::black, 3));
        p.drawEllipse(m, 5, 5);
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(m, 5, 5);
    }

    void drawValueBar(QPainter& p)
    {
        QLinearGradient g(mBarRect.topLeft(), mBarRect.bottomLeft());
        g.setColorAt(0.0, QColor::fromHsvF(mH, mS, 1.0f));
        g.setColorAt(1.0, Qt::black);
        p.setPen(Qt::NoPen);
        p.fillRect(mBarRect, g);
        const double y = mBarRect.top() + (1.0 - mV) * mBarRect.height();
        p.setPen(QPen(Qt::black, 3));
        p.drawLine(QPointF(mBarRect.left() - 2, y), QPointF(mBarRect.right() + 2, y));
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(QPointF(mBarRect.left() - 2, y), QPointF(mBarRect.right() + 2, y));
    }

    Options mOpts;
    Value mStart;
    double mGain = 1.0;
    float mH = 0.0f, mS = 0.0f, mV = 1.0f;
    bool mUseTemp = mOpts.value.useTemperature;
    double mKelvin = mOpts.value.temperature;
    bool mAccepted = true;
    Grab mGrab = Grab::None;
    QRectF mWheelRect, mBarRect, mOldRect, mNewRect, mKelvinRect, mKelvinBox;
};

QPointer<Popup>& openPopup()
{
    static QPointer<Popup> p;
    return p;
}
} // namespace

void show(QPoint globalPos, Options options)
{
    if (Popup* old = openPopup())
        old->close();
    auto* popup = new Popup(std::move(options));
    QRect r(globalPos, popup->size());
    if (QScreen* screen = QApplication::screenAt(globalPos))
    {
        const QRect avail = screen->availableGeometry();
        r.moveLeft(std::clamp(r.left(), avail.left(), avail.right() - r.width()));
        r.moveTop(std::clamp(r.top(), avail.top(), avail.bottom() - r.height()));
    }
    popup->move(r.topLeft());
    openPopup() = popup;
    popup->show();
}
} // namespace lila::ui::colorPopup
