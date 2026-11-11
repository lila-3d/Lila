#include "ui/NumberExpr.h"

#include "curve/Sketch.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSpinBox>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
std::optional<double> evalArithmetic(const QString& text)
{
    static const std::map<std::string, double> kNoNames;
    const std::string s = text.trimmed().toStdString();
    if (s.empty())
        return std::nullopt;
    return curve::sketch::evalExpression(s, kNoNames);
}

/** Integer and decimal spin boxes; date and time edits keep their own parsing. */
QAbstractSpinBox* mathBox(QObject* obj)
{
    for (QObject* o : {obj, obj->parent()})
    {
        if (qobject_cast<QSpinBox*>(o) || qobject_cast<QDoubleSpinBox*>(o))
            return static_cast<QAbstractSpinBox*>(o);
    }
    return nullptr;
}

QLineEdit* editOf(QAbstractSpinBox* box)
{
    return box->findChild<QLineEdit*>(QString(), Qt::FindDirectChildrenOnly);
}

void applyMath(QAbstractSpinBox* box)
{
    QLineEdit* edit = editOf(box);
    if (!edit || box->isReadOnly())
        return;
    QString text = edit->text();
    int pos = 0;
    if (box->validate(text, pos) == QValidator::Acceptable)
        return;
    QString prefix, suffix;
    double current = 0.0;
    if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        prefix = d->prefix(), suffix = d->suffix(), current = d->value();
    else if (auto* i = qobject_cast<QSpinBox*>(box))
        prefix = i->prefix(), suffix = i->suffix(), current = i->value();
    if (!prefix.isEmpty() && text.startsWith(prefix))
        text.remove(0, prefix.size());
    if (!suffix.isEmpty() && text.endsWith(suffix))
        text.chop(suffix.size());
    std::optional<double> v = evalExpression(text, current);
    if (!v)
        return;
    if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        d->setValue(*v);
    else if (auto* i = qobject_cast<QSpinBox*>(box))
        i->setValue(int(std::lround(std::clamp(*v, double(i->minimum()), double(i->maximum())))));
}

class SpinMath : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (!obj->isWidgetType())
            return false;
        switch (e->type())
        {
        case QEvent::FocusIn:
            // The spin box's validator rejects '*', '(' and the like keystroke by keystroke; applyMath judges the whole text.
            if (QAbstractSpinBox* box = mathBox(obj))
                if (QLineEdit* edit = editOf(box))
                    edit->setValidator(nullptr);
            break;
        case QEvent::KeyPress:
        {
            const int key = static_cast<QKeyEvent*>(e)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter)
                if (QAbstractSpinBox* box = mathBox(obj))
                    applyMath(box);
            break;
        }
        case QEvent::FocusOut:
            if (QAbstractSpinBox* box = mathBox(obj))
                applyMath(box);
            break;
        default: break;
        }
        return false;
    }
};
} // namespace

std::optional<double> evalExpression(const QString& text, double current)
{
    QString t = text.trimmed();
    if (t.size() > 2 && t[1] == '=' && QString("+-*/").contains(t[0]))
    {
        std::optional<double> v = evalArithmetic(t.mid(2));
        if (!v)
            return std::nullopt;
        switch (t[0].toLatin1())
        {
        case '+': return current + *v;
        case '-': return current - *v;
        case '*': return current * *v;
        case '/': return *v != 0.0 ? std::optional(current / *v) : std::nullopt;
        }
    }
    if (t.startsWith('='))
        t = t.mid(1);
    return evalArithmetic(t);
}

void installSpinMath(QObject* parent)
{
    qApp->installEventFilter(new SpinMath(parent));
}
} // namespace lila::ui
