#include "ui/WheelGuard.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QScrollBar>

namespace lila::ui
{
namespace
{
bool guarded(QObject* obj)
{
    if (qobject_cast<QAbstractSpinBox*>(obj) || qobject_cast<QComboBox*>(obj))
        return true;
    return qobject_cast<QAbstractSlider*>(obj) && !qobject_cast<QScrollBar*>(obj);
}

class WheelGuard : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::Polish && guarded(obj))
        {
            // Qt focuses a WheelFocus widget the wheel lands on before any filter sees the event.
            auto* w = static_cast<QWidget*>(obj);
            if ((w->focusPolicy() & Qt::WheelFocus) == Qt::WheelFocus)
                w->setFocusPolicy(Qt::StrongFocus);
        }
        else if (e->type() == QEvent::Wheel && guarded(obj) && !static_cast<QWidget*>(obj)->hasFocus())
        {
            // Ignored and filtered, the event goes on up to the parent, the panel's scroll area.
            e->ignore();
            return true;
        }
        return false;
    }
};
} // namespace

void installWheelGuard(QObject* parent)
{
    qApp->installEventFilter(new WheelGuard(parent));
}
} // namespace lila::ui
