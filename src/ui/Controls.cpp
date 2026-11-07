#include "ui/Controls.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/Theme.h"

#include <QButtonGroup>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QVBoxLayout>

#include <algorithm>

namespace lila::ui::controls
{
namespace
{
QString labelFor(const std::string& actionId, const QString& label)
{
    if (!label.isEmpty())
        return label;
    const Action* a = actions::find(actionId);
    return a ? QString::fromStdString(actions::displayLabel(actionId)) : QString::fromStdString(actionId);
}

QToolButton* actionButton(const std::string& actionId, QWidget* parent, const QString& label)
{
    auto* b = new QToolButton(parent);
    b->setText(labelFor(actionId, label));
    b->setToolTip(QString::fromStdString(actions::tooltip(actionId)));
    b->setAccessibleName(b->text());
    QObject::connect(b, &QToolButton::clicked, b, [actionId] { actions::run(nlohmann::json(actionId)); });
    if (const Action* a = actions::find(actionId); a && a->checked)
    {
        b->setCheckable(true);
        b->setChecked(a->checked());
        // The action owns the state; the button only shows it.
        QObject::connect(b, &QToolButton::clicked, b, [b, actionId] {
            if (const Action* act = actions::find(actionId); act && act->checked)
                b->setChecked(act->checked());
        });
    }
    optionBox::withOptionBox(b, actionId, parent);
    return b;
}
} // namespace

void setRole(QWidget* w, const char* role)
{
    w->setProperty("role", role);
    w->style()->unpolish(w);
    w->style()->polish(w);
}

QWidget* section(const QString& title, QWidget* content, QWidget* parent, const char* prefsKey, bool openByDefault, QToolButton** headerOut)
{
    auto* box = new QWidget(parent);
    box->setAttribute(Qt::WA_StyledBackground);
    setRole(box, "section");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 4, 0, 6);
    v->setSpacing(2);
    auto* header = new QToolButton(box);
    header->setText(title);
    header->setCheckable(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setIconSize(QSize(14, 14));
    setRole(header, "sectionHeader");
    const std::string key = std::string(prefsKey) + "/" + title.toStdString();
    const bool open = !PrefStore::get().value<bool>("general", key, !openByDefault);
    header->setChecked(open);
    header->setIcon(icon(open ? "chevron-down" : "chevron-right"));
    content->setParent(box);
    content->setVisible(open);
    QObject::connect(header, &QToolButton::toggled, content, [header, content, key](bool on) {
        content->setVisible(on);
        header->setIcon(icon(on ? "chevron-down" : "chevron-right"));
        PrefStore::get().set("general", key, !on);
    });
    v->addWidget(header);
    v->addWidget(content, 1);
    // Keeps a collapsed header at the top when the section is given extra height.
    v->addStretch(0);
    if (headerOut)
        *headerOut = header;
    return box;
}

QToolButton* tile(const std::string& actionId, const char* iconName, QWidget* parent, const QString& label)
{
    QToolButton* b = actionButton(actionId, parent, label);
    b->setIcon(icon(iconName ? QString(iconName) : QString::fromStdString(actions::iconFor(actionId))));
    b->setIconSize(QSize(24, 24));
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setFixedHeight(58);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRole(b, "tile");
    return b;
}

QToolButton* push(const std::string& actionId, QWidget* parent, const QString& label)
{
    QToolButton* b = actionButton(actionId, parent, label);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRole(b, "push");
    return b;
}

QToolButton* split(const std::string& actionId, const std::vector<std::string>& variants, QWidget* parent, const QString& label)
{
    QToolButton* b = push(actionId, parent, label);
    b->setPopupMode(QToolButton::MenuButtonPopup);
    auto* menu = new QMenu(b);
    for (const std::string& id : variants)
    {
        QAction* a = menu->addAction(labelFor(id, {}), [id] { actions::run(nlohmann::json(id)); });
        a->setToolTip(QString::fromStdString(actions::tooltip(id)));
        a->setData(QString::fromStdString(id));
    }
    menu->setToolTipsVisible(true);
    optionBox::decorateMenu(menu);
    b->setMenu(menu);
    return b;
}

QToolButton* overflow(const std::vector<std::string>& actionIds, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon("dots"));
    b->setText(QString("%1 more").arg(actionIds.size()));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setPopupMode(QToolButton::InstantPopup);
    b->setToolTip("More commands; every one is also in the menus and Ctrl+T search");
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRole(b, "overflow");
    auto* menu = new QMenu(b);
    menu->setToolTipsVisible(true);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, actionIds] {
        menu->clear();
        for (const std::string& id : actionIds)
            if (actions::find(id))
            {
                QAction* a = menu->addAction(icon(QString::fromStdString(actions::iconFor(id))), labelFor(id, {}),
                                             [id] { actions::run(nlohmann::json(id)); });
                a->setToolTip(QString::fromStdString(actions::tooltip(id)));
                a->setData(QString::fromStdString(id));
                a->setEnabled(actions::isEnabled(nlohmann::json(id)));
            }
        optionBox::decorateMenu(menu);
    });
    b->setMenu(menu);
    b->setVisible(!actionIds.empty());
    return b;
}

QWidget* grid(const std::vector<QWidget*>& items, int columns, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* g = new QGridLayout(w);
    g->setContentsMargins(0, 0, 0, 0);
    g->setSpacing(2);
    for (size_t i = 0; i < items.size(); ++i)
    {
        items[i]->setParent(w);
        g->addWidget(items[i], int(i) / columns, int(i) % columns);
    }
    for (int c = 0; c < columns; ++c)
        g->setColumnStretch(c, 1);
    return w;
}

Segmented::Segmented(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground);
    setRole(this, "segmented");
    mRow = new QHBoxLayout(this);
    mRow->setContentsMargins(2, 2, 2, 2);
    mRow->setSpacing(2);
    mGroup = new QButtonGroup(this);
    mGroup->setExclusive(true);
    connect(mGroup, &QButtonGroup::idClicked, this, &Segmented::picked);
}

QToolButton* Segmented::add(const QString& text, const QString& iconName, const QString& tip, int id)
{
    auto* b = new QToolButton(this);
    b->setCheckable(true);
    if (!iconName.isEmpty())
    {
        b->setIcon(icon(iconName));
        b->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    }
    if (!text.isEmpty())
        b->setText(text);
    b->setToolTip(tip);
    b->setAccessibleName(text.isEmpty() ? tip : text);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setRole(b, "segment");
    mGroup->addButton(b, id);
    mRow->addWidget(b, 1);
    return b;
}

void Segmented::setCurrent(int id)
{
    if (QAbstractButton* b = mGroup->button(id); b && !b->isChecked())
        b->setChecked(true);
}

void Segmented::clearCurrent()
{
    // An exclusive group never lets its last checked button go, so exclusivity lifts for the uncheck.
    mGroup->setExclusive(false);
    for (QAbstractButton* b : mGroup->buttons())
        b->setChecked(false);
    mGroup->setExclusive(true);
}

int Segmented::current() const
{
    return mGroup->checkedId();
}

QDoubleSpinBox* valueField(double lo, double hi, int decimals, const QString& unit, QWidget* parent)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setKeyboardTracking(false);
    if (!unit.isEmpty())
        s->setSuffix(" " + unit);
    s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return s;
}

SliderField::SliderField(QWidget* parent) : QDoubleSpinBox(parent)
{
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setButtonSymbols(QAbstractSpinBox::NoButtons);
    setKeyboardTracking(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setRole(this, "slider");
    connect(this, &QDoubleSpinBox::valueChanged, this, qOverload<>(&QWidget::update));
}

void SliderField::paintEvent(QPaintEvent* e)
{
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath clip;
        clip.addRoundedRect(r, 4, 4);
        p.setClipPath(clip);
        p.fillRect(rect(), underMouse() ? themeColor("hover") : themeColor("input"));
        const double span = maximum() - minimum();
        const double f = span > 0 ? std::clamp((value() - minimum()) / span, 0.0, 1.0) : 0.0;
        p.fillRect(QRectF(0, 0, width() * f, height()), themeColor("sliderFill"));
    }
    QDoubleSpinBox::paintEvent(e);
}

QDoubleSpinBox* axisField(char axis, double lo, double hi, int decimals, const QString& unit, QWidget* parent)
{
    QDoubleSpinBox* s = valueField(lo, hi, decimals, unit, parent);
    setRole(s, "axis");
    auto* letter = new QLabel(QString(QChar(axis)), s);
    letter->setAttribute(Qt::WA_TransparentForMouseEvents);
    const char* token = axis == 'X' ? "axisX" : axis == 'Y' ? "axisY" : "axisZ";
    // The letter carries the axis with colour off; the hue only speeds reading it.
    letter->setStyleSheet(QString("color: %1; font-weight: 600; background: transparent;").arg(themeColor(token).name()));
    letter->setProperty("lilaRawColors", true);
    letter->move(7, 0);
    letter->setFixedHeight(22);
    letter->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    return s;
}

QWidget* vectorField(std::vector<QDoubleSpinBox*>& out, double lo, double hi, int decimals, const QString& unit, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(2);
    out.clear();
    for (char axis : {'X', 'Y', 'Z'})
    {
        QDoubleSpinBox* f = axisField(axis, lo, hi, decimals, unit, w);
        h->addWidget(f, 1);
        out.push_back(f);
    }
    return w;
}
namespace
{
class ScrollWidthFit : public QObject
{
public:
    explicit ScrollWidthFit(QScrollArea* scroll) : QObject(scroll), mScroll(scroll) {}

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() == QEvent::LayoutRequest || e->type() == QEvent::Show)
            if (QWidget* content = mScroll->widget(); content && obj == content)
            {
                const int w = content->minimumSizeHint().width() + mScroll->verticalScrollBar()->sizeHint().width() + mScroll->frameWidth() * 2;
                if (mScroll->minimumWidth() != w)
                    mScroll->setMinimumWidth(w);
            }
        return false;
    }

private:
    QScrollArea* mScroll;
};
} // namespace

void fitScrollWidth(QScrollArea* scroll)
{
    if (QWidget* content = scroll->widget())
        content->installEventFilter(new ScrollWidthFit(scroll));
}
} // namespace lila::ui::controls
