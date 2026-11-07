#include "ui/UndoHistory.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/UiCommon.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <string>
#include <unordered_map>

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

enum StepFilter
{
    kAllSteps = 0,
    kEditSteps,
    kSelectionSteps,
};

/**
 * A step's label as users read it: an action's label when one action runs that command, else the command name spaced
 * out ("polyCube" reads "Poly Cube"). Chunk labels that are already words stay as they are.
 */
QString stepLabel(const std::string& raw, const std::unordered_map<std::string, std::string>& actionLabels)
{
    if (raw.empty())
        return "Edit";
    if (raw.find(' ') != std::string::npos || !ops::find(raw))
        return niceName(raw);
    if (auto it = actionLabels.find(raw); it != actionLabels.end() && !it->second.empty())
        return QString::fromStdString(it->second);
    return niceName(raw);
}

/** Command name to its one plain action's label; commands several actions run (with different flags) map to "". */
std::unordered_map<std::string, std::string> commandActionLabels()
{
    std::unordered_map<std::string, std::string> out;
    for (const Action* a : actions::all())
    {
        if (a->command.empty())
            continue;
        auto [it, added] = out.try_emplace(a->command, a->label);
        if (!added && it->second != a->label)
            it->second.clear();
    }
    return out;
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
            ops::execute("undoJump", {{"position", position}}, CallSource::UI);
        }
        catch (const CommandError&)
        {
        }
    });

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(2, 2, 0, 0);
    footer->setSpacing(4);
    mSteps = new controls::Segmented(this);
    mSteps->setObjectName("undoStepFilter");
    mSteps->add("All", {}, "<b>All Steps</b><br>List every step.", kAllSteps);
    mSteps->add("Edits", {}, "<b>Edits</b><br>Hide selection steps. The current step always shows.", kEditSteps);
    mSteps->add("Selection", {}, "<b>Selection</b><br>List selection steps only. The current step always shows.", kSelectionSteps);
    const bool only = PrefStore::get().value<bool>("general", kOnlySelectionPref, false);
    const bool hide = PrefStore::get().value<bool>("general", kHideSelectionPref, false);
    mSteps->setCurrent(only ? kSelectionSteps : hide ? kEditSteps : kAllSteps);
    connect(mSteps, &controls::Segmented::picked, this, [this](int id) {
        PrefStore::get().set("general", kOnlySelectionPref, id == kSelectionSteps);
        PrefStore::get().set("general", kHideSelectionPref, id == kEditSteps);
        refresh();
    });
    footer->addWidget(mSteps);
    footer->addStretch(1);
    QToolButton* flush = controls::overflow({"edit.flushUndo", "edit.flushRedo", "edit.flushAll"}, this);
    flush->setObjectName("undoFlush");
    flush->setText("Flush");
    flush->setToolTip("<b>Flush</b><br>Clear undo steps, redo steps or the whole history.");
    flush->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    footer->addWidget(flush);
    v->addLayout(footer);

    mFooter = new QLabel(this);
    mFooter->setObjectName("undoMemory");
    mFooter->setContentsMargins(4, 0, 4, 2);
    QPalette dim = mFooter->palette();
    dim.setColor(QPalette::WindowText, palette().color(QPalette::PlaceholderText));
    mFooter->setPalette(dim);
    QFont footerFont = mFooter->font();
    footerFont.setPointSizeF(footerFont.pointSizeF() * 0.92);
    mFooter->setFont(footerFont);
    v->addWidget(mFooter);

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
    const bool hideSelection = mSteps->current() == kEditSteps;
    const bool onlySelection = mSteps->current() == kSelectionSteps;
    const std::unordered_map<std::string, std::string> actionLabels = commandActionLabels();
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
    QListWidgetItem* at = add("Start", 0, true);
    for (size_t i = 0; i < steps.size(); ++i)
    {
        const size_t position = i + 1;
        if (position != current && (steps[i].selectionOnly ? hideSelection : onlySelection))
            continue;
        QListWidgetItem* item = add(stepLabel(steps[i].label, actionLabels), position, steps[i].selectionOnly);
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
