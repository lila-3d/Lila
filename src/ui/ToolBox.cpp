#include "ui/ToolBox.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/DisplayAids.h"
#include "ui/PaintSelect.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"

#include <QEvent>
#include <QMainWindow>
#include <QMenu>
#include <QTimer>
#include <QToolBar>

namespace lila::ui::toolBox
{
namespace
{
bool shown()
{
    return PrefStore::get().value<bool>("general", kShownPref, true);
}

/** Keeps the tool box's toolbar at the far left in its own column, and shown as the pref says. */
class Keeper : public QObject
{
public:
    Keeper(QMainWindow* window, QToolBar* bar) : QObject(window), mWindow(window), mBar(bar)
    {
        window->installEventFilter(this);
        mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
            if (file == "general")
                QMetaObject::invokeMethod(this, [this] { sync(); });
        });
    }
    ~Keeper() override { PrefStore::get().removeOnChanged(mPrefListener); }

    void sync()
    {
        // The collapsed-panel strip shares the left toolbar area; the tool box keeps a column of its own left of it.
        auto* strip = mWindow->findChild<QToolBar*>("sidebarLeft", Qt::FindDirectChildrenOnly);
        const bool inArea = mWindow->toolBarArea(mBar) == Qt::LeftToolBarArea;
        if (strip && mWindow->toolBarArea(strip) == Qt::LeftToolBarArea)
        {
            const bool sharing = mBar->isVisible() && strip->isVisible() && mBar->x() >= strip->x();
            if (!inArea || !mWindow->toolBarBreak(strip) || sharing)
            {
                mWindow->insertToolBar(strip, mBar);
                if (!mWindow->toolBarBreak(strip))
                    mWindow->insertToolBarBreak(strip);
            }
        }
        else if (!inArea)
        {
            mWindow->addToolBar(Qt::LeftToolBarArea, mBar);
            mWindow->addToolBarBreak(Qt::LeftToolBarArea);
        }
        if (mBar->isVisible() != shown())
            mBar->setVisible(shown());
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        // A state restored before the first show is applied lazily by the layout, so check again whenever it settles.
        if (obj == mWindow && (e->type() == QEvent::Show || e->type() == QEvent::LayoutRequest))
            QTimer::singleShot(0, this, [this] { sync(); });
        return false;
    }

private:
    QMainWindow* mWindow;
    QToolBar* mBar;
    int mPrefListener = 0;
};
} // namespace

void install(QMainWindow* window, iconBars::StateFn state)
{
    auto* bar = new QToolBar("Tool Box", window);
    bar->setObjectName("toolBox");
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setOrientation(Qt::Vertical);
    bar->toggleViewAction()->setVisible(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    bar->setStyleSheet("QToolBar { background: #383838; border: none; border-right: 1px solid #262626; padding: 0; spacing: 0; }");
    auto* icons = new iconBars::IconBar(iconBars::kToolBox, Qt::Vertical, kIconSize, std::move(state), bar);
    icons->setWrap(true);
    bar->addWidget(icons);
    window->addToolBar(Qt::LeftToolBarArea, bar);
    window->addToolBarBreak(Qt::LeftToolBarArea);
    auto* keeper = new Keeper(window, bar);
    keeper->sync();
}

void registerItems(std::function<ViewportWidget*()> active)
{
    actions::add({.id = "window.toggleToolBox", .label = "Tool Box", .category = "Windows",
                  .run = [] { PrefStore::get().set("general", kShownPref, !shown()); },
                  .doc = "The column of selection tools and display aids down the window's left edge.",
                  .python = "lila.prefs.set(\"general\", \"/ui/showToolBox\", not lila.prefs.get(\"general\", \"/ui/showToolBox\", True))"});
    auto selectTool = [](bool lasso) {
        PrefStore::get().set("general", paintSelect::kLassoPref, lasso);
        actions::run(nlohmann::json("tool.select"));
    };
    actions::add({.id = "select.marqueeTool", .label = "Select Tool (Marquee)", .category = "Select",
                  .run = [selectTool] { selectTool(false); },
                  .doc = "The select tool, dragging a rectangle to select.",
                  .python = "lila.prefs.set(\"general\", \"/select/lasso\", False)\nlila.ui.set_tool(\"select\")"});
    actions::add({.id = "select.lassoTool", .label = "Lasso Select Tool", .category = "Select",
                  .run = [selectTool] { selectTool(true); },
                  .doc = "The select tool, dragging a freehand lasso to select what falls inside it.",
                  .python = "lila.prefs.set(\"general\", \"/select/lasso\", True)\nlila.ui.set_tool(\"select\")"});

    using iconBars::Kind;
    const std::string bar = iconBars::kToolBox;
    auto add = [&](iconBars::Item item) {
        item.bar = bar;
        iconBars::add(std::move(item));
    };
    add({.id = "toolBox/select", .kind = Kind::Menu, .action = "select.marqueeTool", .icon = "pointer",
         .menuActions = {"#Drag Selects With", "select.marqueeTool", "select.lassoTool", "#Options", "select.toggleCameraBased",
                         "select.toggleBackfaceCulling", "viewport.togglePreselectObjects"}});
    add({.id = "toolBox/lasso", .action = "select.lassoTool", .icon = "lasso"});
    add({.id = "toolBox/paint", .kind = Kind::Menu, .action = "select.paintTool", .icon = "brush",
         .menuActions = {"select.paintRadiusSmaller", "select.paintRadiusLarger"}});
    add({.id = "toolBox/-modes", .kind = Kind::Separator});
    add({.id = "toolBox/object", .action = "select.objectMode", .icon = "cube"});
    add({.id = "toolBox/vertex", .action = "select.vertexMode", .icon = "point"});
    add({.id = "toolBox/edge", .action = "select.edgeMode", .icon = "line"});
    add({.id = "toolBox/face", .action = "select.faceMode", .icon = "square"});
    add({.id = "toolBox/-options", .kind = Kind::Separator});
    add({.id = "toolBox/soft", .action = "softSelect.toggle", .icon = "ripple"});
    add({.id = "toolBox/masks", .kind = Kind::Menu, .icon = "filter", .label = "Selection Masks",
         .menuActions = {"select.mask.mesh", "select.mask.joint", "select.mask.locator", "select.mask.other"}});
    add({.id = "toolBox/-display", .kind = Kind::Separator});
    add({.id = "toolBox/backfaceTint", .action = "viewport.aid.backfaceTint", .icon = "arrows-exchange"});
    add({.id = "toolBox/display", .kind = Kind::Menu, .icon = "eye", .label = "Mesh Display (active view)",
         .fill = [active](QMenu* m) { displayAids::fillMenu(m, active()); }});
}
} // namespace lila::ui::toolBox
