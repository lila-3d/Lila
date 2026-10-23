#include "ui/SpinScrub.h"

#include "ops/Undo.h"
#include "ui/Theme.h"
#include "ui/ValueReset.h"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSpinBox>
#include <QToolTip>
#include <QVariantAnimation>

#include <cmath>

namespace lila::ui
{
namespace
{
constexpr double kPixelsPerStep = 6.0;

int64_t nowMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

/** Shows a widget's own tooltip with the reset hint under it. */
void showTipWithHint(QEvent* e, QWidget* w, const QString& own, const QString& defaultText)
{
    QString hint = resetHint(defaultText);
    QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), own.isEmpty() ? hint : own + "\n" + hint, w);
}

QString spinText(QAbstractSpinBox* box, double v)
{
    if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        return d->textFromValue(v) + d->suffix();
    if (auto* i = qobject_cast<QSpinBox*>(box))
        return QString::number(std::lround(v)) + i->suffix();
    return QString::number(v);
}

class SpinScrub : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (m->button() != Qt::MiddleButton)
                return false;
            if (QWidget* choice = choiceOf(obj))
            {
                mChoice = choice;
                mClick.press(int(m->globalPosition().x()), int(m->globalPosition().y()), nowMs());
                return true;
            }
            QAbstractSpinBox* box = spinBoxOf(obj);
            if (!box || !box->isEnabled() || box->isReadOnly())
                return false;
            mBox = box;
            mLastX = m->globalPosition().x();
            mCarry = 0.0;
            mClick.press(int(m->globalPosition().x()), int(m->globalPosition().y()), nowMs());
            QApplication::setOverrideCursor(Qt::SizeHorCursor);
            UndoStack::get().openChunk("Drag Value");
            return true;
        }
        case QEvent::MouseMove:
        {
            if (mChoice)
                mClick.move(int(static_cast<QMouseEvent*>(e)->globalPosition().x()), int(static_cast<QMouseEvent*>(e)->globalPosition().y()));
            if (!mBox)
                return false;
            auto* m = static_cast<QMouseEvent*>(e);
            // The release can go missing when focus moves mid-drag.
            if (!(m->buttons() & Qt::MiddleButton))
            {
                endDrag();
                return false;
            }
            double x = m->globalPosition().x();
            mClick.move(int(x), int(m->globalPosition().y()));
            mCarry += (x - mLastX) / kPixelsPerStep;
            mLastX = x;
            double steps = std::trunc(mCarry);
            if (steps != 0.0)
            {
                mCarry -= steps;
                step(steps, m->modifiers());
            }
            return true;
        }
        case QEvent::MouseButtonRelease:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (mChoice && m->button() == Qt::MiddleButton)
            {
                QPointer<QWidget> choice = mChoice;
                mChoice = nullptr;
                if (mClick.release(int(m->globalPosition().x()), int(m->globalPosition().y()), nowMs()))
                    resetChoice(choice);
                return true;
            }
            if (!mBox || m->button() != Qt::MiddleButton)
                return false;
            QPointer<QAbstractSpinBox> box = mBox;
            endDrag();
            if (mClick.release(int(m->globalPosition().x()), int(m->globalPosition().y()), nowMs()))
                resetToDefault(box);
            return true;
        }
        case QEvent::ToolTip:
            if (QWidget* choice = choiceOf(obj))
            {
                showTipWithHint(e, static_cast<QWidget*>(obj), static_cast<QWidget*>(obj)->toolTip(), choiceText(choice));
                return true;
            }
            if (QAbstractSpinBox* box = spinBoxOf(obj))
            {
                QVariant def = box->property(kDefaultProperty);
                if (!def.isValid())
                    return false;
                auto* label = qobject_cast<QLabel*>(obj);
                showTipWithHint(e, static_cast<QWidget*>(obj), label ? label->toolTip() : box->toolTip(), spinText(box, def.toDouble()));
                return true;
            }
            return false;
        default:
            return false;
        }
    }

private:
    static QAbstractSpinBox* spinBoxOf(QObject* obj)
    {
        // A form row's label (its buddy is the field) scrubs and resets the field too.
        if (auto* label = qobject_cast<QLabel*>(obj))
            return qobject_cast<QAbstractSpinBox*>(label->buddy());
        // The press lands on the spin box's inner line edit as often as on the box itself.
        for (QObject* o = obj; o; o = o->parent())
            if (auto* box = qobject_cast<QAbstractSpinBox*>(o))
                return box;
            else if (o->isWidgetType() && static_cast<QWidget*>(o)->isWindow())
                break;
        return nullptr;
    }

    /** A check box or combo box with a default, or the form label naming one as its buddy. */
    static QWidget* choiceOf(QObject* obj)
    {
        QWidget* w = nullptr;
        if (auto* label = qobject_cast<QLabel*>(obj))
            w = label->buddy();
        else if (obj->isWidgetType())
            w = static_cast<QWidget*>(obj);
        if (!w || !w->isEnabled() || !w->property(kDefaultProperty).isValid())
            return nullptr;
        return qobject_cast<QCheckBox*>(w) || qobject_cast<QComboBox*>(w) ? w : nullptr;
    }

    static QString choiceText(QWidget* w)
    {
        QVariant def = w->property(kDefaultProperty);
        if (auto* combo = qobject_cast<QComboBox*>(w))
            return combo->itemText(def.toInt());
        return def.toBool() ? "on" : "off";
    }

    static void resetChoice(QWidget* w)
    {
        if (!w)
            return;
        QVariant def = w->property(kDefaultProperty);
        if (auto* combo = qobject_cast<QComboBox*>(w))
            combo->setCurrentIndex(def.toInt());
        else if (auto* check = qobject_cast<QCheckBox*>(w))
            check->setChecked(def.toBool());
        flashReset(w);
    }

    static void resetToDefault(QAbstractSpinBox* box)
    {
        if (!box)
            return;
        QVariant def = box->property(kDefaultProperty);
        if (!def.isValid())
            return;
        if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
            d->setValue(def.toDouble());
        else if (auto* i = qobject_cast<QSpinBox*>(box))
            i->setValue(int(std::lround(def.toDouble())));
        else
            return;
        flashReset(box);
    }

    void endDrag()
    {
        mBox = nullptr;
        QApplication::restoreOverrideCursor();
        UndoStack::get().closeChunk();
    }

    void step(double steps, Qt::KeyboardModifiers mods)
    {
        double scale = (mods & Qt::ShiftModifier) ? 10.0 : 1.0;
        if (auto* d = qobject_cast<QDoubleSpinBox*>(mBox.data()))
        {
            if (mods & Qt::ControlModifier)
                scale *= 0.1;
            double unit = d->singleStep() * scale;
            // Land on whole multiples of the step so values stay tidy.
            double v = std::round((d->value() + steps * unit) / unit) * unit;
            d->setValue(v);
        }
        else if (auto* i = qobject_cast<QSpinBox*>(mBox.data()))
            i->setValue(i->value() + int(steps * scale) * i->singleStep());
        else
            mBox->stepBy(int(steps * scale));
    }

    QPointer<QAbstractSpinBox> mBox;
    QPointer<QWidget> mChoice;
    double mLastX = 0.0;
    double mCarry = 0.0;
    valueReset::ClickTracker mClick;
};

/** A fading light laid over a field. */
class Flash : public QWidget
{
public:
    Flash(QWidget* parent, QRect rect) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(rect);
        auto* fade = new QVariantAnimation(this);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setDuration(350);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            mAlpha = v.toDouble();
            update();
        });
        connect(fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show();
        raise();
        fade->start();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), themeShade(255, int(120 * mAlpha)));
    }

private:
    double mAlpha = 1.0;
};

class ResetClick : public QObject
{
public:
    ResetClick(QWidget* target, std::function<void()> reset, QString defaultText, QWidget* flash)
        : QObject(target), mReset(std::move(reset)), mDefault(std::move(defaultText)), mFlash(flash ? flash : target)
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
            if (auto* m = static_cast<QMouseEvent*>(e); m->button() == Qt::MiddleButton)
                mClick.press(int(m->globalPosition().x()), int(m->globalPosition().y()), nowMs());
            return false;
        case QEvent::MouseMove:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            mClick.move(int(m->globalPosition().x()), int(m->globalPosition().y()));
            return false;
        }
        case QEvent::MouseButtonRelease:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (m->button() != Qt::MiddleButton || !mClick.down())
                return false;
            if (!mClick.release(int(m->globalPosition().x()), int(m->globalPosition().y()), nowMs()))
                return false;
            QPointer<QWidget> flash = mFlash;
            mReset();
            if (flash)
                flashReset(flash);
            return true;
        }
        case QEvent::ToolTip:
            showTipWithHint(e, static_cast<QWidget*>(obj), static_cast<QWidget*>(obj)->toolTip(), mDefault);
            return true;
        default:
            return false;
        }
    }

private:
    std::function<void()> mReset;
    QString mDefault;
    QPointer<QWidget> mFlash;
    valueReset::ClickTracker mClick;
};
} // namespace

void installSpinScrub(QObject* parent)
{
    qApp->installEventFilter(new SpinScrub(parent));
}

void setSpinDefault(QAbstractSpinBox* box, double value)
{
    box->setProperty(kDefaultProperty, value);
}

void setCheckDefault(QCheckBox* box, bool on)
{
    box->setProperty(kDefaultProperty, on);
}

void setComboDefault(QComboBox* box, int index)
{
    if (index >= 0)
        box->setProperty(kDefaultProperty, index);
}

QString resetHint(const QString& defaultText)
{
    return QString("Middle-click to reset to default (%1)").arg(defaultText);
}

void flashReset(QWidget* w, QRect rect)
{
    if (!w)
        return;
    // A scroll area paints its content in the viewport, which would cover a flash laid on the frame.
    if (auto* area = qobject_cast<QAbstractScrollArea*>(w); area && rect.isNull())
        w = area->viewport();
    new Flash(w, rect.isNull() ? w->rect() : rect);
}

void onMiddleClickReset(QWidget* target, std::function<void()> reset, const QString& defaultText, QWidget* flash)
{
    target->installEventFilter(new ResetClick(target, std::move(reset), defaultText, flash));
}
} // namespace lila::ui
