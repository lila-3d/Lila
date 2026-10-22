#include "ui/UndoHistory.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <tuple>

namespace lila::ui
{
namespace
{
constexpr const char* kHideSelectionPref = "/undo/hideSelectionSteps";
constexpr const char* kOnlySelectionPref = "/undo/onlySelectionSteps";

QString formatBytes(size_t bytes)
{
    if (bytes < 1024 * 1024)
        return QString("%1 KB").arg(double(bytes) / 1024.0, 0, 'f', 0);
    return QString("%1 MB").arg(double(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
}

QToolButton* toolButton(QWidget* parent, const char* iconName, const QString& tip)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(14, 14));
    return b;
}
} // namespace

UndoHistoryPanel::UndoHistoryPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);

    mList = new QListWidget(this);
    mList->setUniformItemSizes(true);
    mList->setSelectionMode(QAbstractItemView::SingleSelection);
    Hotkeys::passThrough(mList);
    v->addWidget(mList, 1);
    connect(mList, &QListWidget::itemClicked, this, [](QListWidgetItem* item) {
        size_t position = item->data(Qt::UserRole).toULongLong();
        if (position == UndoStack::get().undoCount())
            return;
        try
        {
            cmds::execute("undoJump", {{"position", position}}, CallSource::UI);
        }
        catch (const CommandError&)
        {
        }
    });

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(2, 0, 0, 0);
    mFooter = new QLabel(this);
    mFooter->setEnabled(false);
    footer->addWidget(mFooter, 1);
    const bool only = PrefStore::get().value<bool>("general", kOnlySelectionPref, false);
    mShowSelection = toolButton(this, "pointer", "Show selection steps only (the current step always shows)");
    mShowSelection->setCheckable(true);
    mShowSelection->setChecked(only);
    mHideSelection = toolButton(this, "pointer-off", "Hide selection steps (the current step always shows)");
    mHideSelection->setCheckable(true);
    mHideSelection->setChecked(!only && PrefStore::get().value<bool>("general", kHideSelectionPref, false));
    // Both on would show nothing, so turning one on turns the other off; both off shows every step.
    auto filterToggled = [this](QToolButton* other, const char* pref, bool on) {
        PrefStore::get().set("general", pref, on);
        if (on && other->isChecked())
            other->setChecked(false);
        refresh();
    };
    connect(mShowSelection, &QToolButton::toggled, this,
            [this, filterToggled](bool on) { filterToggled(mHideSelection, kOnlySelectionPref, on); });
    connect(mHideSelection, &QToolButton::toggled, this,
            [this, filterToggled](bool on) { filterToggled(mShowSelection, kHideSelectionPref, on); });
    footer->addWidget(mShowSelection);
    footer->addWidget(mHideSelection);

    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Plain);
    separator->setEnabled(false);
    footer->addWidget(separator);

    for (const auto& [id, iconName, tip] : {
             std::tuple{"edit.flushUndo", "history-off", "Flush Undo: clears the steps undo would revert, so undo cannot go back past this point"},
             std::tuple{"edit.flushRedo", "eraser", "Flush Redo: clears the steps redo would replay"},
             std::tuple{"edit.flushAll", "trash", "Flush All: clears the whole undo and redo history"},
         })
    {
        QToolButton* b = toolButton(this, iconName, tip);
        connect(b, &QToolButton::clicked, this, [action = std::string(id)] { actions::run(nlohmann::json(action)); });
        footer->addWidget(b);
    }
    v->addLayout(footer);

    QPointer<UndoHistoryPanel> self(this);
    UndoStack::get().addListener([self] {
        if (self)
            self->scheduleRefresh();
    });
    refresh();
}

void UndoHistoryPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void UndoHistoryPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

void UndoHistoryPanel::refresh()
{
    const UndoStack& u = UndoStack::get();
    std::vector<UndoStack::Step> steps = u.steps();
    const size_t current = u.undoCount();
    const bool hideSelection = mHideSelection->isChecked();
    const bool onlySelection = mShowSelection->isChecked();
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    const QColor faint = palette().color(QPalette::PlaceholderText);

    mList->setUpdatesEnabled(false);
    mList->clear();
    // Row positions are the undo count after jumping there: the first row is the state before any step.
    auto add = [&](const QString& label, size_t position, bool selection) {
        auto* item = new QListWidgetItem(label, mList);
        item->setData(Qt::UserRole, qulonglong(position));
        if (position > current)
            item->setForeground(dim);
        else if (selection)
            item->setForeground(faint);
        return item;
    };
    QListWidgetItem* at = add("(start)", 0, true);
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const size_t position = i + 1;
        if (position != current && (steps[i].selectionOnly ? hideSelection : onlySelection))
            continue;
        QListWidgetItem* item = add(QString::fromStdString(steps[i].label.empty() ? "(edit)" : steps[i].label), position,
                                    steps[i].selectionOnly);
        if (position == current)
            at = item;
    }
    mList->setCurrentItem(at);
    mList->setUpdatesEnabled(true);
    mList->scrollToItem(at);

    QString text = QString("%1 undo, %2 redo, %3").arg(u.undoCount()).arg(u.redoCount()).arg(formatBytes(u.memoryBytes()));
    if (u.byteBudget())
        text += " / " + formatBytes(u.byteBudget());
    mFooter->setText(text);
}
} // namespace lila::ui
