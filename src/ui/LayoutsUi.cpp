#include "ui/LayoutsUi.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "python/Python.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/Layouts.h"
#include "ui/Shelf.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "ui/ToolBox.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <set>

namespace lila::ui
{
using json = nlohmann::json;

namespace
{
/** Re-reads the layout list each time it opens, so files dropped into a layouts folder show up. */
class LayoutSwitcher : public QComboBox
{
public:
    LayoutSwitcher(std::function<void()> refresh, QWidget* parent) : QComboBox(parent), mRefresh(std::move(refresh)) {}

    void showPopup() override
    {
        mRefresh();
        QComboBox::showPopup();
    }

private:
    std::function<void()> mRefresh;
};

QString displayName(const layouts::Entry& e)
{
    QString name = QString::fromStdString(e.name);
    return e.origin == layouts::Origin::Workspace ? name + "  (folder)" : name;
}

QString originLabel(const layouts::Entry& e)
{
    switch (e.origin)
    {
    case layouts::Origin::BuiltIn: return "Built in";
    case layouts::Origin::Extension: return "Extension";
    case layouts::Origin::User: return e.shadowsBuiltIn ? "User (replaces built in)" : "User";
    case layouts::Origin::Workspace: return e.shadowsBuiltIn ? "Folder (replaces built in)" : "Folder";
    }
    return {};
}
} // namespace

LayoutController::LayoutController(QMainWindow* window, DockSidebars* sidebars, ViewportLayout* viewports, std::function<void()> resetToDefault)
    : QObject(window), mWindow(window), mSidebars(sidebars), mViewports(viewports), mReset(std::move(resetToDefault))
{
    layouts::setGuiHooks({
        .apply = [this](const std::string&, const json& layout) { apply(layout); },
        .capture = [this] { return capture(); },
    });

    actions::add({"window.layout.saveAs", "Save Current Layout As...", "Windows", [this] { saveAs(); }, nullptr,
                  "Saves the panel arrangement, viewports and shelf under a name.", "lila.ops.saveLayout(\"My Layout\")"});
    actions::add({"window.layout.manage", "Manage Layouts...", "Windows", [this] { manage(); }, nullptr,
                  "Apply, rename, delete or revert saved layouts, or add them to the shelf."});
    actions::add({"window.layout.addToShelf", "Add Current Layout to Shelf", "Windows", [this] {
                      std::string cur = layouts::current();
                      if (!cur.empty())
                          addToShelf(cur);
                  }, [] { return !layouts::current().empty(); }, "Adds a shelf button that switches to the current layout."});
    QPointer<LayoutController> self(this);
    mContributedListener = layouts::contributed().addListener([self](RegistryChange, const std::string&, const std::string&) {
        if (self)
            self->refreshSwitcher();
    });

    mMenu = new QMenu("Layouts", window);
    mMenu->setIcon(icon("layout"));
    connect(mMenu, &QMenu::aboutToShow, this, &LayoutController::fillMenu);
    fillMenu();

    auto* corner = new QWidget(window);
    auto* row = new QHBoxLayout(corner);
    row->setContentsMargins(0, 0, 4, 0);
    row->setSpacing(2);
    mSwitcher = new LayoutSwitcher([this] { refreshSwitcher(); }, corner);
    mSwitcher->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    mSwitcher->setMinimumContentsLength(10);
    mSwitcher->setToolTip("Layout: pick one to rearrange the panels");
    connect(mSwitcher, &QComboBox::activated, this, [this](int index) {
        std::string name = mSwitcher->itemData(index).toString().toStdString();
        if (!name.empty())
            run("applyLayout", {{"name", name}});
        refreshSwitcher();
    });
    auto* more = new QToolButton(corner);
    more->setIcon(icon("layout"));
    more->setAutoRaise(true);
    more->setPopupMode(QToolButton::InstantPopup);
    more->setMenu(mMenu);
    more->setToolTip("Layouts: save, manage, add to shelf");
    row->addWidget(more);
    row->addWidget(mSwitcher);
    // Keeps whatever already sits in the corner, the workspace name.
    if (QWidget* existing = window->menuBar()->cornerWidget(Qt::TopRightCorner))
        row->insertWidget(0, existing);
    window->menuBar()->setCornerWidget(corner, Qt::TopRightCorner);
    refreshSwitcher();

    mPrefListener = PrefStore::get().onChanged([self](const std::string& file) {
        if (self && (file == "general" || file.starts_with("layouts/")) && file != "layouts/main")
            self->refreshSwitcher();
    });
    Workspace::get().onChanged([self] {
        if (self)
            self->refreshSwitcher();
    });
}

LayoutController::~LayoutController()
{
    PrefStore::get().removeOnChanged(mPrefListener);
    layouts::contributed().removeListener(mContributedListener);
    layouts::setGuiHooks({});
}

void LayoutController::run(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

QDockWidget* LayoutController::dock(const QString& name) const
{
    for (QDockWidget* d : mWindow->findChildren<QDockWidget*>(name))
        if (d->parentWidget() == mWindow)
            return d;
    return nullptr;
}

void LayoutController::fillMenu()
{
    mMenu->clear();
    std::string cur = layouts::current();
    bool builtIns = true;
    for (const layouts::Entry& e : layouts::list())
    {
        if (builtIns && e.origin != layouts::Origin::BuiltIn && !e.shadowsBuiltIn)
        {
            mMenu->addSeparator();
            builtIns = false;
        }
        QAction* a = mMenu->addAction(displayName(e));
        a->setCheckable(true);
        a->setChecked(e.name == cur);
        a->setStatusTip(QString("Apply the %1 layout").arg(QString::fromStdString(e.name)));
        std::string name = e.name;
        connect(a, &QAction::triggered, this, [this, name] { run("applyLayout", {{"name", name}}); });
    }
    mMenu->addSeparator();
    mMenu->addAction(icon("device-floppy"), "Save Current Layout As...", this, [] { actions::run(json("window.layout.saveAs")); });
    QAction* shelf = mMenu->addAction(icon("plus"), "Add Current Layout to Shelf", this, [] { actions::run(json("window.layout.addToShelf")); });
    shelf->setEnabled(!cur.empty() && layouts::find(cur));
    mMenu->addAction(icon("pencil"), "Manage Layouts...", this, [] { actions::run(json("window.layout.manage")); });
    mMenu->addSeparator();
    mMenu->addAction("Reset to Default", this, [] { actions::run(json("window.resetLayout")); });
}

void LayoutController::refreshSwitcher()
{
    refreshLayoutActions();
    if (!mSwitcher)
        return;
    std::vector<layouts::Entry> all = layouts::list();
    std::string cur = layouts::current();
    QStringList want;
    for (const layouts::Entry& e : all)
        want << displayName(e);
    bool same = mSwitcher->count() == int(all.size()) + (cur.empty() ? 1 : 0);
    for (int i = 0, offset = cur.empty() ? 1 : 0; same && i < int(all.size()); ++i)
        same = mSwitcher->itemText(i + offset) == want[i];
    if (!same)
    {
        QSignalBlocker block(mSwitcher);
        mSwitcher->clear();
        // No layout applied yet: show a placeholder rather than claiming one.
        if (cur.empty())
            mSwitcher->addItem("Layout", QString());
        for (const layouts::Entry& e : all)
            mSwitcher->addItem(displayName(e), QString::fromStdString(e.name));
    }
    QSignalBlocker block(mSwitcher);
    int index = mSwitcher->findData(QString::fromStdString(cur));
    mSwitcher->setCurrentIndex(index >= 0 ? index : 0);
}

void LayoutController::refreshLayoutActions()
{
    std::set<std::string> live;
    for (const layouts::Entry& e : layouts::list())
    {
        std::string id = "layout.apply." + e.name;
        live.insert(id);
        if (mLayoutActions.contains(id) && actions::find(id))
            continue;
        std::string name = e.name;
        actions::add({id, name + " Layout", "Layouts", [this, name] { run("applyLayout", {{"name", name}}); }, nullptr,
                      "Arranges the panels, viewports and shelf as the " + name + " layout.",
                      "lila.ops.applyLayout(\"" + name + "\")"});
        mLayoutActions.insert(id);
    }
    for (auto it = mLayoutActions.begin(); it != mLayoutActions.end();)
    {
        if (live.contains(*it))
        {
            ++it;
            continue;
        }
        actions::remove(*it);
        it = mLayoutActions.erase(it);
    }
}

std::string LayoutController::currentShelf() const
{
    if (auto* panel = mWindow->findChild<ShelfPanel*>())
        if (auto* tabs = panel->findChild<QTabBar*>(); tabs && tabs->currentIndex() >= 0)
            return tabs->tabText(tabs->currentIndex()).toStdString();
    std::vector<std::string> names = shelves::names();
    return names.empty() ? std::string() : names.front();
}

json LayoutController::capture() const
{
    json docks = json::object();
    for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
    {
        if (d->parentWidget() != mWindow || d->objectName().isEmpty())
            continue;
        bool collapsed = mSidebars && mSidebars->isCollapsed(d);
        json entry = {{"title", d->windowTitle().toStdString()},
                      {"open", collapsed || d->toggleViewAction()->isChecked()},
                      {"floating", d->isFloating()},
                      {"collapsed", collapsed}};
        if (d->isFloating())
        {
            QRect g = d->geometry();
            entry["geometry"] = {g.x(), g.y(), g.width(), g.height()};
        }
        docks[d->objectName().toStdString()] = std::move(entry);
    }
    json collapsed = json::array();
    if (mSidebars)
        for (const QString& name : mSidebars->collapsedNames())
            collapsed.push_back(name.toStdString());
    json tabSizes = json::object();
    if (mSidebars)
        for (auto& [name, size] : mSidebars->tabSizes())
            tabSizes[name.toStdString()] = size;
    QDockWidget* shelf = dock("shelf");
    return {{"version", 1},
            {"tabSizes", std::move(tabSizes)},
            {"state", mWindow->saveState().toBase64().toStdString()},
            {"docks", std::move(docks)},
            {"collapsed", std::move(collapsed)},
            {"viewport", {{"quad", mViewports->quad()}, {"camera", mViewports->active()->viewName()}}},
            {"shelf", {{"visible", shelf && shelf->toggleViewAction()->isChecked()}, {"current", currentShelf()}}}};
}

void LayoutController::ensureDocks(const json& docks)
{
    tearoff::recreate(mWindow, tearoff::openDocks(docks));
    if (!docks.is_object() || !python::initialized())
        return;
    for (auto& [name, entry] : docks.items())
    {
        // Python panels (lila.ui.panel) are docked the first time they're shown, so they may not exist yet.
        if (!name.starts_with("py_") || !entry.value("open", false) || dock(QString::fromStdString(name)))
            continue;
        std::string title = entry.value("title", "");
        if (title.empty())
            continue;
        if (!python::run("import lila.ui\nlila.ui.show_panel(" + json(title).dump() + ")", "<layout>").ok)
            log::warn("layout: couldn't recreate panel '{}'; is the script that defines it loaded?", title);
    }
}

void LayoutController::applyPreset(const json& preset)
{
    mReset();
    std::set<std::string> open;
    for (const json& n : preset.value("open", json::array()))
        if (n.is_string())
            open.insert(n.get<std::string>());
    for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
        if (d->parentWidget() == mWindow && !d->objectName().isEmpty())
            d->setVisible(open.count(d->objectName().toStdString()) > 0);
    // splitDockWidget can't take a dock out of a tab group (it tabs it again), so the dock is re-added to its area instead.
    for (const json& n : preset.value("bottom", json::array()))
    {
        QDockWidget* d = n.is_string() ? dock(QString::fromStdString(n.get<std::string>())) : nullptr;
        Qt::DockWidgetArea area = d ? mWindow->dockWidgetArea(d) : Qt::NoDockWidgetArea;
        if (!d || d->isHidden() || d->isFloating() || area == Qt::NoDockWidgetArea)
            continue;
        mWindow->removeDockWidget(d);
        mWindow->addDockWidget(area, d, Qt::Vertical);
        d->show();
    }
    // A dock just shown has no tab until the dock layout updates, so raising it now would be a no-op.
    json raise = preset.value("raise", json::array());
    QTimer::singleShot(0, mWindow, [this, raise] {
        for (const json& n : raise)
            if (QDockWidget* d = n.is_string() ? dock(QString::fromStdString(n.get<std::string>())) : nullptr; d && !d->isHidden())
                d->raise();
    });
}

void LayoutController::apply(const json& layout)
{
    mWindow->setUpdatesEnabled(false);
    json docks = layout.value("docks", json::object());
    ensureDocks(docks);
    if (layout.contains("state") && layout["state"].is_string())
    {
        mSidebars->expandAll();
        mWindow->restoreState(QByteArray::fromBase64(QByteArray::fromStdString(layout["state"].get<std::string>())));
        toolBox::place(mWindow);
        for (QDockWidget* d : mWindow->findChildren<QDockWidget*>())
        {
            if (d->parentWidget() != mWindow || d->objectName().isEmpty())
                continue;
            auto it = docks.find(d->objectName().toStdString());
            // Panels the layout never knew about stay out of the way.
            if (it == docks.end())
            {
                d->hide();
                continue;
            }
            json g = it->value("geometry", json());
            if (d->isFloating() && g.is_array() && g.size() == 4)
                d->setGeometry(g[0].get<int>(), g[1].get<int>(), g[2].get<int>(), g[3].get<int>());
        }
        QStringList collapsed;
        for (const json& n : layout.value("collapsed", json::array()))
            if (n.is_string())
                collapsed << QString::fromStdString(n.get<std::string>());
        mSidebars->restoreCollapsed(collapsed);
        std::map<QString, int> tabSizes;
        json savedSizes = layout.value("tabSizes", json::object());
        for (auto& [name, size] : savedSizes.items())
            if (size.is_number_integer())
                tabSizes[QString::fromStdString(name)] = size.get<int>();
        mSidebars->setTabSizes(std::move(tabSizes));
    }
    else if (layout.contains("preset"))
        applyPreset(layout["preset"]);

    json view = layout.value("viewport", json::object());
    if (view.is_object())
    {
        mViewports->setQuad(view.value("quad", mViewports->quad()));
        std::string camera = view.value("camera", "");
        if (!mViewports->quad() && !camera.empty())
            mViewports->active()->setView(camera);
    }
    std::string shelf = layout.value(json::json_pointer("/shelf/current"), std::string());
    if (auto* panel = mWindow->findChild<ShelfPanel*>(); panel && !shelf.empty())
        if (auto* tabs = panel->findChild<QTabBar*>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i).toStdString() == shelf)
                    tabs->setCurrentIndex(i);

    // Sizes and repaint wait for the dock layout to settle, so the window redraws once.
    json sizes = layout.value(json::json_pointer("/preset/size"), json::object());
    QPointer<QMainWindow> window(mWindow);
    QTimer::singleShot(0, mWindow, [this, window, sizes] {
        if (!window)
            return;
        for (auto& [name, s] : sizes.items())
        {
            QDockWidget* d = dock(QString::fromStdString(name));
            if (!d || d->isHidden() || d->isFloating() || !s.is_object())
                continue;
            if (s.contains("width"))
                window->resizeDocks({d}, {int(s["width"].get<double>() * window->width())}, Qt::Horizontal);
            if (s.contains("height"))
                window->resizeDocks({d}, {int(s["height"].get<double>() * window->height())}, Qt::Vertical);
        }
        window->setUpdatesEnabled(true);
    });
}

void LayoutController::addToShelf(const std::string& name)
{
    std::string shelf = currentShelf();
    if (shelf.empty())
    {
        log::error("there is no shelf to add the layout to");
        return;
    }
    shelves::addItem(shelf, {{"label", name},
                             {"icon", "layout"},
                             {"tooltip", "Apply the " + name + " layout"},
                             {"target", {{"command", "applyLayout"}, {"args", {{"name", name}}}}}});
    log::info("added a {} layout button to the {} shelf", name, shelf);
}

void LayoutController::saveAs()
{
    QDialog dlg(mWindow);
    dlg.setWindowTitle("Save Layout");
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel("Name"));
    auto* name = new QLineEdit;
    std::string cur = layouts::current();
    if (std::optional<layouts::Entry> e = layouts::find(cur); e && e->origin != layouts::Origin::BuiltIn)
        name->setText(QString::fromStdString(cur));
    name->selectAll();
    v->addWidget(name);
    auto* inFolder = new QCheckBox("Save in this folder (.lila/layouts), for everyone working in it");
    inFolder->setEnabled(Workspace::get().isOpen());
    v->addWidget(inFolder);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        std::string n = name->text().trimmed().toStdString();
        if (std::string why = layouts::invalidReason(n); !why.empty())
        {
            QMessageBox::warning(&dlg, "Save Layout", QString::fromStdString(why));
            return;
        }
        layouts::Origin scope = inFolder->isChecked() ? layouts::Origin::Workspace : layouts::Origin::User;
        std::optional<layouts::Entry> existing = layouts::find(n);
        bool clash = existing && existing->origin == scope;
        if (clash && QMessageBox::question(&dlg, "Save Layout", QString("Replace the %1 layout?").arg(QString::fromStdString(n))) != QMessageBox::Yes)
            return;
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    try
    {
        ops::execute("saveLayout", {{"name", name->text().trimmed().toStdString()}, {"workspace", inFolder->isChecked()}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(mWindow, "Save Layout", e.what());
    }
    refreshSwitcher();
}

void LayoutController::manage()
{
    QDialog dlg(mWindow);
    dlg.setWindowTitle("Layouts");
    dlg.resize(460, 340);
    auto* h = new QHBoxLayout(&dlg);
    auto* tree = new QTreeWidget;
    tree->setColumnCount(2);
    tree->setHeaderLabels({"Layout", "Saved in"});
    tree->setRootIsDecorated(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    h->addWidget(tree, 1);
    auto* side = new QVBoxLayout;
    h->addLayout(side);
    auto* applyButton = new QPushButton("Apply");
    auto* saveButton = new QPushButton("Save Current As...");
    auto* renameButton = new QPushButton("Rename...");
    auto* deleteButton = new QPushButton("Delete");
    auto* shelfButton = new QPushButton("Add to Shelf");
    auto* closeButton = new QPushButton("Close");
    for (QPushButton* b : {applyButton, saveButton, renameButton, deleteButton, shelfButton})
        side->addWidget(b);
    side->addStretch(1);
    side->addWidget(closeButton);

    std::vector<layouts::Entry> entries;
    auto selected = [&]() -> const layouts::Entry* {
        int row = tree->currentItem() ? tree->indexOfTopLevelItem(tree->currentItem()) : -1;
        return row >= 0 && row < int(entries.size()) ? &entries[row] : nullptr;
    };
    auto updateButtons = [&] {
        const layouts::Entry* e = selected();
        bool saved = e && e->origin != layouts::Origin::BuiltIn && e->origin != layouts::Origin::Extension;
        applyButton->setEnabled(e);
        shelfButton->setEnabled(e);
        renameButton->setEnabled(saved);
        deleteButton->setEnabled(saved);
        deleteButton->setText(saved && e->shadowsBuiltIn ? "Revert to Built In" : "Delete");
    };
    auto reload = [&](const std::string& select) {
        entries = layouts::list();
        tree->clear();
        std::string cur = layouts::current();
        for (const layouts::Entry& e : entries)
        {
            auto* item = new QTreeWidgetItem({QString::fromStdString(e.name), originLabel(e)});
            if (e.name == cur)
            {
                QFont f = item->font(0);
                f.setBold(true);
                item->setFont(0, f);
            }
            tree->addTopLevelItem(item);
            if (e.name == select)
                tree->setCurrentItem(item);
        }
        updateButtons();
    };
    connect(tree, &QTreeWidget::currentItemChanged, &dlg, [&] { updateButtons(); });
    connect(tree, &QTreeWidget::itemDoubleClicked, &dlg, [&] { applyButton->click(); });
    connect(applyButton, &QPushButton::clicked, &dlg, [&] {
        if (const layouts::Entry* e = selected())
        {
            std::string name = e->name;
            run("applyLayout", {{"name", name}});
            reload(name);
        }
    });
    connect(saveButton, &QPushButton::clicked, &dlg, [&] {
        saveAs();
        reload(layouts::current());
    });
    connect(renameButton, &QPushButton::clicked, &dlg, [&] {
        const layouts::Entry* e = selected();
        if (!e)
            return;
        std::string from = e->name;
        bool ok = false;
        QString to = QInputDialog::getText(&dlg, "Rename Layout", "New name", QLineEdit::Normal, QString::fromStdString(from), &ok).trimmed();
        if (!ok || to.isEmpty() || to.toStdString() == from)
            return;
        try
        {
            ops::execute("renameLayout", {{"name", from}, {"to", to.toStdString()}}, CallSource::UI);
            reload(to.toStdString());
        }
        catch (const CommandError& err)
        {
            QMessageBox::warning(&dlg, "Rename Layout", err.what());
        }
    });
    connect(deleteButton, &QPushButton::clicked, &dlg, [&] {
        const layouts::Entry* e = selected();
        if (!e)
            return;
        std::string name = e->name;
        QString what = e->shadowsBuiltIn ? "Revert %1 to the built-in layout?" : "Delete the %1 layout?";
        if (QMessageBox::question(&dlg, "Layouts", what.arg(QString::fromStdString(name))) != QMessageBox::Yes)
            return;
        run("deleteLayout", {{"name", name}});
        reload(name);
    });
    connect(shelfButton, &QPushButton::clicked, &dlg, [&] {
        if (const layouts::Entry* e = selected())
            addToShelf(e->name);
    });
    connect(closeButton, &QPushButton::clicked, &dlg, &QDialog::accept);
    reload(layouts::current());
    dlg.exec();
    refreshSwitcher();
}
} // namespace lila::ui
