#include "ui/MenuButtons.h"

#include <QAction>
#include <QToolButton>
#include <QVariant>

namespace lila::ui
{
namespace
{
constexpr const char* kAction = "rightClickAction";
constexpr const char* kBaseTip = "rightClickBaseTip";
} // namespace

void setRightClickAction(QToolButton* button, QAction* item)
{
    if (!button)
        return;
    if (!button->property(kBaseTip).isValid())
        button->setProperty(kBaseTip, button->toolTip());
    const QString base = button->property(kBaseTip).toString();
    button->setProperty(kAction, QVariant::fromValue(static_cast<QObject*>(item)));
    if (!item)
    {
        button->setToolTip(base);
        return;
    }
    button->setToolTip((base.isEmpty() ? QString() : base + "\n") + "Right-click: " + item->text().remove('&'));
    QObject::connect(item, &QObject::destroyed, button, [button, item] {
        if (button->property(kAction).value<QObject*>() == item)
            setRightClickAction(button, nullptr);
    });
    if (button->contextMenuPolicy() != Qt::CustomContextMenu)
    {
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(button, &QWidget::customContextMenuRequested, button, [button] {
            if (QAction* a = rightClickAction(button); a && a->isEnabled())
                a->trigger();
        });
    }
}

QAction* rightClickAction(const QToolButton* button)
{
    return button ? static_cast<QAction*>(button->property(kAction).value<QObject*>()) : nullptr;
}
} // namespace lila::ui
