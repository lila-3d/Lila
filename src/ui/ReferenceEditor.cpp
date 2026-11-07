#include "ui/ReferenceEditor.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/References.h"
#include "io/SceneFile.h"
#include "scene/Graph.h"
#include "ui/Controls.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QFileDialog>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <vector>

namespace lila::ui
{
using json = nlohmann::json;

ReferenceEditor::ReferenceEditor(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    std::vector<QWidget*> needSelection;
    auto button = [&](const char* label, const QString& tip, std::function<void()> fn) {
        auto* b = new QToolButton(this);
        b->setText(label);
        b->setToolTip(tip);
        controls::setRole(b, "push");
        connect(b, &QToolButton::clicked, this, fn);
        buttons->addWidget(b);
        return b;
    };
    button("Create...", "<b>Create Reference</b><br>Reference a Lila or Maya scene.", [this] { createReferenceDialog(this); });
    buttons->addSpacing(10);
    needSelection.push_back(button("Reload", "<b>Reload</b><br>Read the selected references from disk again.",
                                   [this] { runOnSelected("reloadReference"); }));
    needSelection.push_back(button("Load", "<b>Load</b><br>Bring the selected unloaded references back into the scene.",
                                   [this] { runOnSelected("loadReference"); }));
    needSelection.push_back(button("Unload", "<b>Unload</b><br>Take the selected references' objects out of the scene, keeping "
                                             "their edits.",
                                   [this] { runOnSelected("unloadReference"); }));
    buttons->addStretch(1);
    auto* more = new QToolButton(this);
    more->setIcon(icon("dots"));
    more->setToolTip("<b>More</b><br>Import or remove the selected references.");
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    moreMenu->setToolTipsVisible(true);
    moreMenu->addAction("Import Objects", this, [this] { runOnSelected("importReference"); })
        ->setToolTip("Make the selected references' objects part of this scene and drop the reference.");
    moreMenu->addAction(icon("trash"), "Remove Reference", this, [this] { runOnSelected("removeReference"); })
        ->setToolTip("Remove the selected references and their objects.");
    more->setMenu(moreMenu);
    buttons->addWidget(more);
    needSelection.push_back(more);
    v->addLayout(buttons);

    mTree = new QTreeWidget(this);
    mTree->setHeaderLabels({"Namespace", "State", "Edits", "File"});
    mTree->headerItem()->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
    mTree->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    mTree->setRootIsDecorated(false);
    mTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    setEmptyHint(mTree, "No references. Click Create to reference a scene.");
    v->addWidget(mTree, 1);
    auto syncButtons = [this, needSelection] {
        const bool any = !mTree->selectedItems().isEmpty();
        for (QWidget* w : needSelection)
            w->setEnabled(any);
    };
    connect(mTree, &QTreeWidget::itemSelectionChanged, this, syncButtons);
    syncButtons();

    // Referenced files edited elsewhere either reload on their own (pref) or get flagged here.
    mWatcher = new QFileSystemWatcher(this);
    connect(mWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        QTimer::singleShot(300, this, [this, path] {
            json refs = ops::execute("references", json::object());
            for (const json& r : refs)
                if (QString::fromStdString(r.value("path", "")) == path)
                {
                    std::string ns = r.value("namespace", "");
                    if (PrefStore::get().value<bool>("general", "/references/autoReload", true))
                    {
                        log::info("reference {} changed on disk; reloading", ns);
                        ops::execute("reloadReference", {{"args", {ns}}}, CallSource::UI);
                    }
                    else if (!mChanged.contains(path))
                        mChanged.push_back(path);
                }
            refresh();
        });
    });

    // References change the node set, so any structural scene change is a cue to refresh.
    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Cleared || c == scene::Change::Reset)
            scheduleRefresh();
    });
    refresh();
}

ReferenceEditor::~ReferenceEditor()
{
    scene::Graph::current().removeListener(mListener);
}

void ReferenceEditor::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        if (isVisible())
            refresh();
        else
        {
            mStale = true;
            watchFiles();
        }
    });
}

void ReferenceEditor::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (mStale)
        refresh();
}

void ReferenceEditor::refresh()
{
    mStale = false;
    mTree->clear();
    json refs = ops::execute("references", json::object());
    for (const json& r : refs)
    {
        auto* item = new QTreeWidgetItem(mTree);
        item->setText(0, QString::fromStdString(r.value("namespace", "")));
        item->setText(1, r.value("loaded", true) ? "Loaded" : "Unloaded");
        item->setText(2, QString::number(r.value("edits", json::array()).size()));
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(3, QString::fromStdString(r.value("path", "")));
        if (mChanged.contains(item->text(3)))
        {
            item->setText(1, item->text(1) + ", Changed");
            item->setToolTip(1, "The file changed on disk. Reload to read it again.");
        }
    }
    watchFiles();
}

void ReferenceEditor::watchFiles()
{
    QStringList wanted;
    for (const io::Reference& r : io::refs::all())
        wanted.push_back(QString::fromStdString(r.path.generic_string()));
    QStringList watched = mWatcher->files();
    for (const QString& f : watched)
        if (!wanted.contains(f))
            mWatcher->removePath(f);
    // Editors often save by replacing the file, which drops it from the watch; re-adding covers that.
    for (const QString& f : wanted)
        if (!f.isEmpty() && !mWatcher->files().contains(f))
            mWatcher->addPath(f);
}

void ReferenceEditor::runOnSelected(const char* command)
{
    for (QTreeWidgetItem* item : mTree->selectedItems())
    {
        try
        {
            ops::execute(command, {{"args", {item->text(0).toStdString()}}}, CallSource::UI);
            mChanged.removeAll(item->text(3));
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }
    refresh();
}

void ReferenceEditor::createReferenceDialog(QWidget* parent)
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    const ui::fileDialog::ContextScope scope("reference", "References");
    QString file = ui::fileDialog::getOpenFileName(parent, "Create Reference", start,
                                                "Scenes (*.lila *.lilaa *.ma *.mb);;Lila scenes (*.lila *.lilaa);;Maya scenes (*.ma *.mb)");
    if (file.isEmpty())
        return;
    bool maya = file.endsWith(".ma", Qt::CaseInsensitive) || file.endsWith(".mb", Qt::CaseInsensitive);
    try
    {
        ops::execute(maya ? "mayaReference" : "createReference", {{"args", {file.toStdString()}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}
} // namespace lila::ui
