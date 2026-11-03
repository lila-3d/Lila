#include "ui/RenamePanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/Panels.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
/** Lists every name the naming conventions would change and renames them once confirmed. */
void conformNames()
{
    nlohmann::json changes;
    try
    {
        changes = ops::execute("namingConvention", {{"check", true}}, CallSource::Script);
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
        return;
    }
    if (changes.empty())
    {
        log::info("every name already follows the naming conventions");
        return;
    }
    QDialog dialog(QApplication::activeWindow());
    dialog.setWindowTitle("Conform Names");
    dialog.resize(560, 420);
    auto* v = new QVBoxLayout(&dialog);
    v->addWidget(new QLabel(QString("%1 name%2 will change:").arg(changes.size()).arg(changes.size() == 1 ? "" : "s"), &dialog));
    auto* list = new QTreeWidget(&dialog);
    list->setColumnCount(2);
    list->setHeaderLabels({"Object", "New name"});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->header()->setSectionResizeMode(QHeaderView::Stretch);
    for (const nlohmann::json& c : changes)
        new QTreeWidgetItem(list, {QString::fromStdString(c[0].get<std::string>()), QString::fromStdString(c[1].get<std::string>())});
    v->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    buttons->addButton("Rename", QDialogButtonBox::AcceptRole)->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    v->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    try
    {
        nlohmann::json fixed = ops::execute("namingConvention", {{"fix", true}}, CallSource::UI);
        log::info("conformed {} name{}", fixed.size(), fixed.size() == 1 ? "" : "s");
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
    }
}

QSpinBox* intSpin(QWidget* parent, int lo, int hi, int value)
{
    auto* s = new QSpinBox(parent);
    s->setRange(lo, hi);
    s->setValue(value);
    return s;
}
} // namespace

RenamePanel::RenamePanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(3);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    v->addLayout(form);

    auto row = [this](std::initializer_list<QWidget*> widgets) {
        auto* w = new QWidget(this);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        for (QWidget* x : widgets)
            h->addWidget(x, qobject_cast<QLineEdit*>(x) ? 1 : 0);
        return w;
    };
    auto label = [this](const char* text) { return new QLabel(text, this); };

    mScope = new QComboBox(this);
    mScope->addItems({"Selection", "Selection, outliner order", "Hierarchy"});
    mScope->setToolTip("Selection numbers in the order you picked; Hierarchy renames each selected object and everything under it, top to bottom.");
    form->addRow("Rename", mScope);

    mBase = new QLineEdit(this);
    mBase->setPlaceholderText("new name, e.g. tail");
    mBase->setToolTip("Replaces every name and numbers them: tail with padding 2 gives tail_01, tail_02... Put # in it to place the number yourself.");
    mSeparator = new QLineEdit("_", this);
    mSeparator->setMaximumWidth(28);
    mSeparator->setToolTip("Between the name and its number");
    form->addRow("Name", row({mBase, mSeparator}));
    mPadding = intSpin(this, 0, 8, 2);
    mPadding->setToolTip("Digits in the number; 0 leaves it off");
    mStart = intSpin(this, -99999, 99999, 1);
    mStep = intSpin(this, -999, 999, 1);
    form->addRow("Padding", row({mPadding, label("Start"), mStart, label("Step"), mStep}));

    mFind = new QLineEdit(this);
    mFind->setPlaceholderText("find");
    mReplace = new QLineEdit(this);
    mReplace->setPlaceholderText("replace with");
    mReplace->setToolTip("# numbers the objects (## for 01), {name}, {parent} and {type} insert those; with regex, $1 is the first group.");
    form->addRow("Find", mFind);
    form->addRow("Replace", mReplace);
    mRegex = new QCheckBox("Regex", this);
    mMatchCase = new QCheckBox("Match case", this);
    form->addRow("", row({mRegex, mMatchCase}));

    mStripStart = intSpin(this, 0, 999, 0);
    mStripStart->setToolTip("Characters dropped from the front");
    mStripEnd = intSpin(this, 0, 999, 0);
    mStripEnd->setToolTip("Characters dropped from the end");
    mStripDigits = new QCheckBox("Number", this);
    mStripDigits->setToolTip("Drops the trailing number and the separator before it: Arm_02 becomes Arm");
    form->addRow("Strip", row({label("Front"), mStripStart, label("End"), mStripEnd, mStripDigits}));

    mCase = new QComboBox(this);
    mCase->addItem("Keep", "");
    for (const char* c : {"PascalCase", "camelCase", "snake_case", "UPPER_SNAKE", "lower", "UPPER"})
        mCase->addItem(c, c);
    form->addRow("Case", mCase);

    mPrefix = new QLineEdit(this);
    mPrefix->setPlaceholderText("prefix");
    mSuffix = new QLineEdit(this);
    mSuffix->setPlaceholderText("suffix");
    form->addRow("Add", row({mPrefix, mSuffix}));

    mPreview = new QTreeWidget(this);
    mPreview->setColumnCount(2);
    mPreview->setHeaderLabels({"Name", "New name"});
    mPreview->setRootIsDecorated(false);
    mPreview->setUniformRowHeights(true);
    mPreview->setSelectionMode(QAbstractItemView::NoSelection);
    mPreview->header()->setSectionResizeMode(QHeaderView::Stretch);
    Hotkeys::passThrough(mPreview);
    v->addWidget(mPreview, 1);

    auto* buttons = new QHBoxLayout;
    auto* resetButton = new QPushButton("Reset", this);
    auto* applyButton = new QPushButton("Rename", this);
    applyButton->setDefault(true);
    buttons->addWidget(resetButton);
    buttons->addStretch(1);
    buttons->addWidget(applyButton);
    v->addLayout(buttons);
    connect(resetButton, &QPushButton::clicked, this, [this] { reset(); });
    connect(applyButton, &QPushButton::clicked, this, [this] { apply(); });

    for (QLineEdit* e : {mBase, mSeparator, mFind, mReplace, mPrefix, mSuffix})
    {
        connect(e, &QLineEdit::textChanged, this, [this] { scheduleRefresh(); });
        connect(e, &QLineEdit::returnPressed, this, [this] { apply(); });
    }
    for (QSpinBox* s : {mPadding, mStart, mStep, mStripStart, mStripEnd})
        connect(s, &QSpinBox::valueChanged, this, [this] { scheduleRefresh(); });
    for (QCheckBox* c : {mRegex, mMatchCase, mStripDigits})
        connect(c, &QCheckBox::toggled, this, [this] { scheduleRefresh(); });
    for (QComboBox* c : {mScope, mCase})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { scheduleRefresh(); });

    QPointer<RenamePanel> self(this);
    mGraphListener = scene::Graph::current().addListener([self](scene::Change c, scene::NodeId) {
        if (self && c != scene::Change::Value && c != scene::Change::Connection)
            self->scheduleRefresh();
    });
    refresh();
}

RenamePanel::~RenamePanel() { scene::Graph::current().removeListener(mGraphListener); }

void RenamePanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

nlohmann::json RenamePanel::call(bool preview) const
{
    nlohmann::json c = {{"preview", preview},
                        {"hierarchy", mScope->currentIndex() == kHierarchy},
                        {"order", mScope->currentIndex() == kSelection ? "selection" : "outliner"},
                        {"start", mStart->value()},
                        {"step", mStep->value()},
                        {"find", mFind->text().toStdString()},
                        {"replace", mReplace->text().toStdString()},
                        {"regex", mRegex->isChecked()},
                        {"matchCase", mMatchCase->isChecked()},
                        {"stripStart", mStripStart->value()},
                        {"stripEnd", mStripEnd->value()},
                        {"stripDigits", mStripDigits->isChecked()},
                        {"case", mCase->currentData().toString().toStdString()},
                        {"prefix", mPrefix->text().toStdString()},
                        {"suffix", mSuffix->text().toStdString()}};
    if (QString base = mBase->text().trimmed(); !base.isEmpty())
        c["name"] = (base.contains('#') || mPadding->value() == 0 ? base : base + mSeparator->text() + QString(mPadding->value(), '#')).toStdString();
    return c;
}

void RenamePanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(0, this, [this] {
        mPending = false;
        refresh();
    });
}

void RenamePanel::refresh()
{
    mPreview->clear();
    nlohmann::json plans;
    try
    {
        plans = ops::execute("batchRename", call(true), CallSource::Script);
    }
    catch (const CommandError& e)
    {
        auto* item = new QTreeWidgetItem(mPreview, {QString::fromStdString(e.what())});
        item->setForeground(0, themeColor("statusError"));
        item->setFirstColumnSpanned(true);
        return;
    }
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    for (const nlohmann::json& p : plans)
    {
        const std::string status = p["status"].get<std::string>();
        auto* item = new QTreeWidgetItem(mPreview, {QString::fromStdString(p["from"].get<std::string>()), QString::fromStdString(p["to"].get<std::string>())});
        item->setToolTip(0, QString::fromStdString(p["path"].get<std::string>()));
        if (status == "unchanged")
            item->setForeground(1, dim);
        else if (status == "reference")
        {
            item->setForeground(1, dim);
            item->setToolTip(1, "Comes from a reference and keeps its name");
        }
        else if (status == "empty")
        {
            item->setText(1, "(empty, kept)");
            item->setForeground(1, themeColor("statusWarning"));
        }
        else if (status == "clash")
        {
            item->setForeground(1, themeColor("statusWarning"));
            item->setToolTip(1, "Another object has this name; Lila numbers it");
        }
    }
}

void RenamePanel::apply()
{
    try
    {
        nlohmann::json plans = ops::execute("batchRename", call(false), CallSource::UI);
        int renamed = 0;
        for (const nlohmann::json& p : plans)
            renamed += p["status"] == "renamed" || p["status"] == "clash";
        log::info("renamed {} object{}", renamed, renamed == 1 ? "" : "s");
    }
    catch (const CommandError& e)
    {
        log::warn("{}", e.what());
    }
    refresh();
}

void RenamePanel::reset()
{
    for (QLineEdit* e : {mBase, mFind, mReplace, mPrefix, mSuffix})
        e->clear();
    mSeparator->setText("_");
    mPadding->setValue(2);
    mStart->setValue(1);
    mStep->setValue(1);
    mStripStart->setValue(0);
    mStripEnd->setValue(0);
    for (QCheckBox* c : {mRegex, mMatchCase, mStripDigits})
        c->setChecked(false);
    mCase->setCurrentIndex(0);
}

void registerRenamePanel()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    panels::add({.id = kRenameDock,
                 .title = "Rename",
                 .create = [](QWidget* p) -> QWidget* { return new RenamePanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "forms",
                 .open = false,
                 .toggleAction = "window.rename"});
    actions::add({.id = "edit.batchRename",
                  .label = "Batch Rename...",
                  .category = "Edit",
                  .run = [] { panels::show(kRenameDock); },
                  .doc = "Rename many objects at once: name a chain with padded numbers, find and replace (regex too), strip, "
                         "change case, add a prefix or suffix, with a live preview."});
    actions::add({.id = "edit.conformNames",
                  .label = "Conform Names",
                  .category = "Edit",
                  .run =
                      [] {
                          if (!scene::Graph::current().namingPolicy().enabled)
                          {
                              log::warn("Naming conventions are off: turn on Enforce conventions in Preferences > Naming");
                              return;
                          }
                          conformNames();
                      },
                  .doc = "Lists every object in the scene that breaks the naming conventions (Preferences > Naming) with "
                         "its new name, and renames them once you confirm. Referenced objects keep their names.",
                  .python = "namingConvention(fix=True)"});
    actions::add({.id = "edit.checkNames",
                  .label = "Check Names",
                  .category = "Edit",
                  .run =
                      [] {
                          try
                          {
                              nlohmann::json bad = ops::execute("namingConvention", {{"check", true}}, CallSource::Script);
                              if (bad.empty())
                              {
                                  log::info("every name follows the naming conventions");
                                  return;
                              }
                              nlohmann::json paths = nlohmann::json::array();
                              for (const nlohmann::json& b : bad)
                                  paths.push_back(b[0]);
                              ops::execute("select", {{"args", paths}}, CallSource::UI);
                              log::warn("{} name{} break the naming conventions; they are selected", bad.size(), bad.size() == 1 ? "" : "s");
                          }
                          catch (const CommandError& e)
                          {
                              log::warn("{}", e.what());
                          }
                      },
                  .doc = "Selects every object whose name breaks the naming conventions (Preferences > Naming), whether or not "
                         "they are enforced. Conform Names renames them.",
                  .python = "select([bad[0] for bad in namingConvention(check=True)])"});
    actions::add({.id = "naming.toggleEnforce",
                  .label = "Enforce Naming Conventions",
                  .category = "Edit",
                  .run =
                      [] {
                          try
                          {
                              ops::execute("namingConvention", {{"enable", !scene::Graph::current().namingPolicy().enabled}}, CallSource::UI);
                          }
                          catch (const CommandError& e)
                          {
                              log::warn("{}", e.what());
                          }
                      },
                  .doc = "New and renamed objects follow the naming conventions as they are named (for this workspace, else for "
                         "you).",
                  .python = "namingConvention(enable=True)",
                  .checked = [] { return scene::Graph::current().namingPolicy().enabled; }});
}
} // namespace lila::ui
