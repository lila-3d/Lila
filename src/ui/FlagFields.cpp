#include "ui/FlagFields.h"

#include "ui/SpinScrub.h"
#include "ui/ValueReset.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QSpinBox>

#include <array>
#include <cmath>
#include <memory>

namespace lila::ui
{
namespace opt = cmds::options;

namespace
{
/** Label that scrubs a number when dragged sideways, like the channel box's name column. */
class ScrubLabel : public QLabel
{
public:
    ScrubLabel(const QString& text, std::function<void(double)> step, QWidget* parent) : QLabel(text, parent), mStep(std::move(step))
    {
        setCursor(Qt::SizeHorCursor);
        setToolTip("Drag sideways to scrub; Ctrl for fine steps, Shift for coarse");
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton)
            mLast = e->globalPosition().x();
        if (e->button() == Qt::MiddleButton)
            mMiddle.press(int(e->globalPosition().x()), int(e->globalPosition().y()), 0);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton)
            mMiddle.cancel();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & (Qt::LeftButton | Qt::MiddleButton)))
            return;
        // A middle press only scrubs once it is clearly a drag; a click resets the value instead.
        if (mMiddle.down() && !mMiddle.dragging())
        {
            mMiddle.move(int(e->globalPosition().x()), int(e->globalPosition().y()));
            mLast = e->globalPosition().x();
            if (!mMiddle.dragging())
                return;
        }
        double dx = e->globalPosition().x() - mLast;
        mLast = e->globalPosition().x();
        double step = (e->modifiers() & Qt::ControlModifier) ? 0.01 : (e->modifiers() & Qt::ShiftModifier) ? 1.0 : 0.1;
        if (dx != 0.0)
            mStep(dx * step);
    }

private:
    std::function<void(double)> mStep;
    double mLast = 0.0;
    valueReset::ClickTracker mMiddle;
};

QString tip(const opt::Field& f)
{
    return QString::fromStdString(f.flag + (f.doc.empty() ? "" : ": " + f.doc));
}

double num(const json& v, double fallback = 0.0)
{
    return v.is_number() ? v.get<double>() : v.is_boolean() ? double(v.get<bool>()) : fallback;
}

QString listText(const json& v)
{
    QStringList parts;
    for (const json& e : v.is_array() ? v : json::array())
        parts << QString::fromStdString(e.is_string() ? e.get<std::string>() : e.dump());
    return parts.join(", ");
}

json parseList(const QString& text)
{
    json out = json::array();
    for (const QString& p : text.split(',', Qt::SkipEmptyParts))
        if (QString t = p.trimmed(); !t.isEmpty())
            out.push_back(t.toStdString());
    return out;
}

QDoubleSpinBox* floatSpin(const opt::Field& f, QWidget* parent)
{
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(std::isnan(f.min) ? -1e6 : f.min, std::isnan(f.max) ? 1e6 : f.max);
    spin->setDecimals(3);
    spin->setSingleStep(std::isnan(f.step) ? 0.1 : f.step);
    spin->setSuffix(QString::fromStdString(f.suffix));
    spin->setKeyboardTracking(false);
    return spin;
}
} // namespace

FlagFieldWidgets makeFlagField(const opt::Field& f, const json& value, std::function<void(const json&)> changed, QWidget* parent)
{
    FlagFieldWidgets out;
    QString label = QString::fromStdString(f.label);
    QString defText = QString::fromStdString(valueReset::describe(f.defaultValue));
    switch (f.kind)
    {
    case opt::FieldKind::Check:
    {
        auto* box = new QCheckBox(label, parent);
        box->setChecked(num(value) != 0.0);
        QObject::connect(box, &QCheckBox::toggled, box, [changed](bool on) { changed(on); });
        bool on = num(f.defaultValue) != 0.0;
        onMiddleClickReset(box, [box, on] { box->setChecked(on); }, on ? "on" : "off");
        out.field = box;
        out.set = [box](const json& v) {
            QSignalBlocker b(box);
            box->setChecked(num(v) != 0.0);
        };
        break;
    }
    case opt::FieldKind::Int:
    {
        auto* spin = new QSpinBox(parent);
        int v = int(std::lround(num(value)));
        int d = int(std::lround(num(f.defaultValue)));
        spin->setRange(!std::isnan(f.min) ? int(f.min) : (v < 0 || d < 0) ? -1000000 : 0, std::isnan(f.max) ? 1000000 : int(f.max));
        spin->setSuffix(QString::fromStdString(f.suffix));
        spin->setValue(v);
        spin->setKeyboardTracking(false);
        QObject::connect(spin, &QSpinBox::valueChanged, spin, [changed](int x) { changed(x); });
        // An int scrubs a step per ten pixels.
        auto acc = std::make_shared<double>(0.0);
        auto* scrub = new ScrubLabel(label, [spin, acc](double dx) {
            *acc += dx;
            int whole = int(*acc);
            if (whole != 0)
            {
                *acc -= whole;
                spin->setValue(spin->value() + whole);
            }
        }, parent);
        setSpinDefault(spin, d);
        onMiddleClickReset(scrub, [spin, d] { spin->setValue(d); }, defText, spin);
        out.label = scrub;
        out.field = spin;
        out.set = [spin](const json& x) {
            QSignalBlocker b(spin);
            spin->setValue(int(std::lround(num(x))));
        };
        break;
    }
    case opt::FieldKind::Float:
    {
        QDoubleSpinBox* spin = floatSpin(f, parent);
        spin->setValue(num(value));
        QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [changed](double x) { changed(x); });
        auto* scrub = new ScrubLabel(label, [spin](double dx) { spin->setValue(spin->value() + dx); }, parent);
        double d = num(f.defaultValue);
        setSpinDefault(spin, d);
        onMiddleClickReset(scrub, [spin, d] { spin->setValue(d); }, defText, spin);
        out.label = scrub;
        out.field = spin;
        out.set = [spin](const json& x) {
            QSignalBlocker b(spin);
            spin->setValue(num(x));
        };
        break;
    }
    case opt::FieldKind::Choice:
    {
        auto* combo = new QComboBox(parent);
        for (size_t i = 0; i < f.choices.size(); ++i)
            combo->addItem(QString::fromStdString(i < f.choiceLabels.size() ? f.choiceLabels[i] : f.choices[i]), QString::fromStdString(f.choices[i]));
        auto select = [combo](const json& v) {
            int at = combo->findData(QString::fromStdString(v.is_string() ? v.get<std::string>() : std::string()));
            if (at >= 0)
                combo->setCurrentIndex(at);
        };
        select(value);
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo, changed](int) { changed(combo->currentData().toString().toStdString()); });
        auto* l = new QLabel(label, parent);
        int defIndex = combo->findData(QString::fromStdString(f.defaultValue.is_string() ? f.defaultValue.get<std::string>() : std::string()));
        if (defIndex >= 0)
            onMiddleClickReset(l, [combo, defIndex] { combo->setCurrentIndex(defIndex); }, combo->itemText(defIndex), combo);
        out.label = l;
        out.field = combo;
        out.set = [combo, select](const json& v) {
            QSignalBlocker b(combo);
            select(v);
        };
        break;
    }
    case opt::FieldKind::Text:
    case opt::FieldKind::List:
    {
        bool list = f.kind == opt::FieldKind::List;
        auto* line = new QLineEdit(parent);
        auto text = [list](const json& v) { return list ? listText(v) : QString::fromStdString(v.is_string() ? v.get<std::string>() : std::string()); };
        line->setText(text(value));
        if (list)
            line->setPlaceholderText("comma separated");
        QObject::connect(line, &QLineEdit::editingFinished, line, [line, list, changed] {
            changed(list ? parseList(line->text()) : json(line->text().toStdString()));
        });
        out.label = new QLabel(label, parent);
        out.field = line;
        out.set = [line, text](const json& v) {
            QSignalBlocker b(line);
            line->setText(text(v));
        };
        break;
    }
    case opt::FieldKind::Vec3:
    {
        auto* box = new QWidget(parent);
        auto* h = new QHBoxLayout(box);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        auto spins = std::make_shared<std::array<QDoubleSpinBox*, 3>>();
        auto emitAll = [spins, changed] { changed(json::array({(*spins)[0]->value(), (*spins)[1]->value(), (*spins)[2]->value()})); };
        for (int i = 0; i < 3; ++i)
        {
            QDoubleSpinBox* spin = floatSpin(f, box);
            spin->setValue(value.is_array() && value.size() == 3 ? num(value[i]) : 0.0);
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [emitAll](double) { emitAll(); });
            (*spins)[i] = spin;
            h->addWidget(spin);
        }
        out.label = new QLabel(label, parent);
        out.field = box;
        out.set = [spins](const json& v) {
            for (int i = 0; i < 3; ++i)
            {
                QSignalBlocker b((*spins)[i]);
                (*spins)[i]->setValue(v.is_array() && v.size() == 3 ? num(v[i]) : 0.0);
            }
        };
        break;
    }
    }
    out.field->setToolTip(tip(f));
    if (out.label && out.label->toolTip().isEmpty())
        out.label->setToolTip(tip(f));
    return out;
}
} // namespace lila::ui
