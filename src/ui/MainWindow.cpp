#include "ui/MainWindow.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "mcp/McpServer.h"
#include "render/Renderer.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/RenderSettings.h"
#include "ui/Actions.h"
#include "ui/GameExporter.h"
#include "ui/MeshSequenceExporter.h"
#include "ui/AnimatedSend.h"
#include "ui/Hotbox.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/ImagePlaneUi.h"
#include "ui/Layouts.h"
#include "ui/LayoutsUi.h"
#include "ui/Animation.h"
#include "ui/ApiReference.h"
#include "ui/DocsUi.h"
#include "ui/Ghosting.h"
#include "ui/GraphEditor.h"
#include "ui/SpinScrub.h"
#include "ui/WheelGuard.h"
#include "ui/Theme.h"
#include "ui/TimeSlider.h"
#include "ui/AttributeEditor.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/AnimLayersPanel.h"
#include "ui/OffsetsPanel.h"
#include "ui/RenamePanel.h"
#include "ui/DisplayAids.h"
#include "ui/LodDisplay.h"
#include "ui/DockDragCancel.h"
#include "ui/DockSidebar.h"
#include "ui/Environments.h"
#include "ui/ExtensionManagerDialog.h"
#include "ui/ChannelBox.h"
#include "ui/CursorBadges.h"
#include "ui/Outliner.h"
#include "ui/MaterialEditor.h"
#include "ui/MenuSearch.h"
#include "ui/Menus.h"
#include "ui/Modes.h"
#include "ui/ModelingToolkit.h"
#include "ui/TopologyToolkit.h"
#include "ui/ModifierStack.h"
#include "ui/OptionBox.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/QuickCommand.h"
#include "ui/UvEditor.h"
#include "ui/PreferencesDialog.h"
#include "ui/ProgressOverlay.h"
#include "ui/ReferenceEditor.h"
#include "ui/RenderSettingsDialog.h"
#include "ui/SetsUi.h"
#include "ui/Shelf.h"
#include "ui/StatusOutput.h"
#include "ui/SceneDialogs.h"
#include "ui/SourceControlPanel.h"
#include "ui/BakerPanel.h"
#include "ui/TextureGraphUi.h"
#include "ui/MaterialBrowser.h"
#include "ui/TextureSetPanel.h"
#include "ui/TexPipelineUi.h"
#include "ui/BatchRunPanel.h"
#include "ui/SpacesUi.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "vcs/SourceControl.h"
#include "ui/Terminal.h"
#include "ui/Theme.h"
#include "ui/ToolBox.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/ScriptEditor.h"
#include "ui/ScriptPanel.h"
#include "ui/UndoHistory.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportLayout.h"
#include "ui/ViewportToolbar.h"
#include "ui/ViewportWidget.h"

#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QBoxLayout>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QScreen>
#include <QStatusBar>
#include <QWidgetAction>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace lila::ui
{
namespace
{
constexpr const char* kLayoutFile = "layouts/main";
constexpr const char* kChannelBoxDock = "channelBox";
// A saved layout older than this is dropped once, so everyone gets a changed default.
constexpr int kLayoutVersion = 5;
const QStringList kDefaultCollapsed = {"sourceControl", "referenceEditor", "materialEditor", "uvEditor", "scriptEditor", "scriptOutput", kTerminalDock};
constexpr int kDefaultSidePanelWidth = 340;

/** Maya's help line: the status bar shows what's under the cursor straight away, without waiting for the tooltip. */
class HelpLine : public QObject
{
public:
    HelpLine(QLabel* label, QObject* parent) : QObject(parent), mLabel(label) {}

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::Enter:
            if (auto* w = qobject_cast<QWidget*>(obj); w && !w->toolTip().isEmpty())
                show(w->toolTip());
            break;
        case QEvent::Leave:
            if (auto* w = qobject_cast<QWidget*>(obj); w && !w->toolTip().isEmpty())
                show({});
            break;
        case QEvent::Show:
            if (auto* menu = qobject_cast<QMenu*>(obj); menu && !menu->property("lilaHelpLine").toBool())
            {
                menu->setProperty("lilaHelpLine", true);
                connect(menu, &QMenu::hovered, this, [this](QAction* a) { show(a->statusTip().isEmpty() ? a->toolTip() : a->statusTip()); });
                connect(menu, &QMenu::aboutToHide, this, [this] { show({}); });
            }
            break;
        default:
            break;
        }
        return false;
    }

private:
    void show(const QString& text)
    {
        // Tooltips may be rich text; the help line is one plain line.
        QString plain = Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
        mLabel->setText(plain.section(QChar('\n'), 0, 0));
    }

    QLabel* mLabel;
};
}

MainWindow::MainWindow()
{
    setWindowTitle("Lila");
    resize(1600, 960);
#ifdef _WIN32
    BOOL dark = TRUE;
    DwmSetWindowAttribute(HWND(winId()), 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
#endif
    setDockOptions(AnimatedDocks | AllowNestedDocks | AllowTabbedDocks | GroupedDragging);
    dockDragCancel::install();

    mViewports = new ViewportLayout(this);
    setCentralWidget(mViewports);
    toolBox::install(this, [this](const std::string& id) { return viewportActionState(id, mViewports->active()); });

    registerPanels();
    for (panels::Panel* p : panels::registry().allInOrder())
        createPanelDock(*p);
    mPanelListener = panels::registry().addListener([this](RegistryChange change, const std::string& id, const std::string&) {
        if (change == RegistryChange::Added)
        {
            if (const panels::Panel* p = panels::registry().find(id))
                createPanelDock(*p);
        }
        else
            removePanelDock(id);
    });
    setShowDockHook([this](const char* name) { revealDock(findChild<QDockWidget*>(name)); });
    setToggleDockHook([this](const char* name) { toggleDock(findChild<QDockWidget*>(name)); });
    tearoff::setDockHook([this](const QString& title, const QString& name, QWidget* content) {
        return addDock(title, name, content, Qt::RightDockWidgetArea);
    });
    setUvEditorOpener([this] { toggleDock(findChild<QDockWidget*>(kUvEditorDock)); });
    setNodeEditorShowHook([this] { revealDock(findChild<QDockWidget*>(kNodeEditorDock)); });
    QDockWidget* shelf = findChild<QDockWidget*>("shelf");

    mLayoutTimer = new QTimer(this);
    mLayoutTimer->setSingleShot(true);
    mLayoutTimer->setInterval(400);
    connect(mLayoutTimer, &QTimer::timeout, this, &MainWindow::saveLayout);

    registerActions();
    optionBox::install();
    mLayouts = new LayoutController(this, mSidebars, mViewports, [this] { resetLayout(); });
    buildMenus();
    buildStatusBar();
    restoreLayout();
    applyChromePrefs();
    modes::restore();
    connect(shelf, &QDockWidget::visibilityChanged, this, [this, shelf](bool visible) {
        // Layouts and Reset Layout reopen the shelf; the pref keeps it closed.
        if (visible && !mShelfShown)
            QTimer::singleShot(0, this, [this, shelf] {
                if (!mShelfShown && shelf->isVisible())
                    mSidebars->close(shelf);
            });
    });

    io::onStateChanged([self = QPointer<MainWindow>(this)] { if (self) self->refreshTitle(); });

    mAutosaveTimer = new QTimer(this);
    connect(mAutosaveTimer, &QTimer::timeout, this, [] { io::autosave(); });
    auto applyUndoLimit = [] {
        PrefStore& p = PrefStore::get();
        UndoStack::get().setLimit(p.value<bool>("general", "/undo/infinite", true) ? 0 : size_t(std::max(1.0, p.value<double>("general", "/undo/steps", 500.0))));
        UndoStack::get().setByteBudget(size_t(std::max(0.0, p.value<double>("general", "/undo/memoryMB", 2048.0)) * 1024.0 * 1024.0));
    };
    applyUndoLimit();
    PrefStore::get().onChanged([this, applyUndoLimit](const std::string& file) {
        if (file != "general")
            return;
        QMetaObject::invokeMethod(qApp, applyUndoLimit);
        QMetaObject::invokeMethod(this, [this] { applyChromePrefs(); });
    });
    auto autosaveInterval = [] { return int(std::max(5.0, PrefStore::get().value<double>("general", "/autosave/intervalSeconds", 30.0)) * 1000); };
    mAutosaveTimer->start(autosaveInterval());
    PrefStore::get().onChanged([this, autosaveInterval](const std::string& file) {
        if (file == "general" && mAutosaveTimer->interval() != autosaveInterval())
            QMetaObject::invokeMethod(this, [this, autosaveInterval] { mAutosaveTimer->start(autosaveInterval()); });
    });

    Workspace::get().onChanged([this] {
        refreshWorkspaceLabel();
        refreshRecentFolders();
    });
}

MainWindow::~MainWindow()
{
    panels::registry().removeListener(mPanelListener);
}

QDockWidget* MainWindow::addDock(const QString& title, const QString& objectName, QWidget* content, Qt::DockWidgetArea area)
{
    auto* dock = new QDockWidget(title, this);
    dock->setObjectName(objectName);
    dock->setWidget(content);
    addDockWidget(area, dock);
    if (!mSidebars)
    {
        mSidebars = new DockSidebars(this);
        connect(mSidebars, &DockSidebars::changed, this, &MainWindow::scheduleLayoutSave);
    }
    mSidebars->manage(dock);
    connect(dock, &QDockWidget::dockLocationChanged, this, &MainWindow::scheduleLayoutSave);
    connect(dock, &QDockWidget::topLevelChanged, this, &MainWindow::scheduleLayoutSave);
    connect(dock, &QDockWidget::visibilityChanged, this, &MainWindow::scheduleLayoutSave);
    connect(dock, &QDockWidget::visibilityChanged, this, [this, dock](bool visible) {
        QDockWidget* channels = findChild<QDockWidget*>(kChannelBoxDock);
        if (visible && channels && dock != channels && tabifiedDockWidgets(channels).contains(dock))
            mTabBeforeChannelBox = dock;
    });
    return dock;
}

void MainWindow::registerPanels()
{
    using panels::Panel;
    auto dock = [this](const char* name) { return findChild<QDockWidget*>(name); };
    auto make = [](auto factory) { return std::function<QWidget*(QWidget*)>(factory); };

    panels::add({.id = "shelf", .title = "Shelf", .create = make([](QWidget* p) { return new ShelfPanel(p); }), .area = Qt::TopDockWidgetArea,
                 .place = [this](QDockWidget* shelf) {
                     shelf->setFeatures(shelf->features() & ~QDockWidget::DockWidgetVerticalTitleBar);
                     QWidget* grip = ShelfPanel::dockTitleBar(shelf);
                     grip->setToolTip("Drag to move the shelf; right-click to collapse, float or close it");
                     grip->setContextMenuPolicy(Qt::CustomContextMenu);
                     connect(grip, &QWidget::customContextMenuRequested, this, [this, shelf, grip](const QPoint& at) {
                         QMenu menu;
                         menu.addAction("Collapse", this, [this, shelf] { mSidebars->collapse(shelf); });
                         menu.addAction(shelf->isFloating() ? "Dock" : "Float", shelf, [shelf] { shelf->setFloating(!shelf->isFloating()); });
                         menu.addAction("Close", shelf, &QDockWidget::close);
                         menu.exec(grip->mapToGlobal(at));
                     });
                     shelf->setTitleBarWidget(grip);
                 }});
    panels::add({.id = "outliner", .title = "Outliner", .create = make([](QWidget* p) { return new OutlinerPanel(p); }),
                 .area = Qt::LeftDockWidgetArea});
    panels::add({.id = "sourceControl", .title = "Source Control", .create = make([](QWidget* p) { return new SourceControlPanel(p); }),
                 .area = Qt::LeftDockWidgetArea});
    panels::add({.id = kReferenceEditorDock, .title = "Reference Editor", .create = make([](QWidget* p) { return new ReferenceEditor(p); }),
                 .area = Qt::LeftDockWidgetArea});
    panels::add({.id = kDisplayLayersDock, .title = "Display Layers", .create = make([](QWidget* p) { return new DisplayLayersPanel(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "stack-2"});
    registerAnimLayersPanel();
    registerOffsetsPanel();
    registerRenamePanel();
    registerBatchRunPanel();
    registerBakerPanel();
    registerTextureGraphUi();
    registerMaterialBrowserUi();
    registerTextureSetPanel();
    registerTexturePipelineUi();
    panels::add({.id = kUndoHistoryDock, .title = "Undo History", .create = make([](QWidget* p) { return new UndoHistoryPanel(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "history",
                 .place = [this, dock](QDockWidget* d) { splitDockWidget(dock("outliner"), d, Qt::Vertical); }});
    // Split after the undo history, so the sets sit between it and the outliner.
    panels::add({.id = kQuickSelectSetsDock, .title = "Quick Select Sets", .create = make([](QWidget* p) { return new QuickSelectSetsPanel(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "list-details",
                 .place = [this, dock](QDockWidget* d) { splitDockWidget(dock("outliner"), d, Qt::Vertical); }});
    panels::add({.id = "scriptOutput", .title = "Script Output", .create = make([](QWidget* p) { return new ScriptPanel(p); }),
                 .area = Qt::BottomDockWidgetArea});
    panels::add({.id = kScriptEditorDock, .title = "Script Editor", .create = make([](QWidget* p) {
                     auto* scriptEditor = new ScriptEditor(p);
                     scriptEditor->setEditorFont(monoFont());
                     scriptEditor->setIconProvider([](const QString& name) { return icon(name); });
                     scriptEditor->setRunKeyLookup([](QKeyEvent* k) -> QString {
                         for (const char* id : {"script.run", "script.runLine", "script.runCell"})
                             if (Hotkeys::get().isKeyFor(k, id))
                                 return id;
                         return {};
                     });
                     return scriptEditor;
                 }),
                 .area = Qt::BottomDockWidgetArea});
    panels::add({.id = kApiReferenceDock, .title = "API Reference", .create = make([](QWidget* p) { return new ApiReference(p); }),
                 .area = Qt::RightDockWidgetArea, .open = false, .place = [this, dock](QDockWidget* apiDock) {
                     QDockWidget* editorDock = dock(kScriptEditorDock);
                     auto* scriptEditor = editorDock ? qobject_cast<ScriptEditor*>(editorDock->widget()) : nullptr;
                     auto* apiRef = qobject_cast<ApiReference*>(apiDock->widget());
                     if (!scriptEditor || !apiRef)
                         return;
                     // The docs site's API page when there is one, else the live reference.
                     connect(scriptEditor, &ScriptEditor::showDocs, this, [apiRef, apiDock](const QString& symbol) {
                         if (docs::openApi(symbol.toStdString()))
                             return;
                         revealDock(apiDock);
                         apiRef->showSymbol(symbol);
                     });
                     connect(scriptEditor, &ScriptEditor::apiDocsRequested, this, [] { docs::open("python/"); });
                     connect(scriptEditor, &ScriptEditor::apiReferenceRequested, this, [] { panels::show(kApiReferenceDock); });
                     connect(scriptEditor, &ScriptEditor::batchRunRequested, this, [] { actions::run("window.batchRun"); });
                     connect(apiRef, &ApiReference::insertCode, this, [scriptEditor, editorDock](const QString& code) {
                         editorDock->show();
                         editorDock->raise();
                         CodeEditor* ed = scriptEditor->currentEditor();
                         if (!ed)
                             ed = scriptEditor->newTab();
                         ed->insertPlainText(code);
                         ed->setFocus();
                     });
                 },
                 .floatFirst = true});
    panels::add({.id = kTerminalDock, .title = "Terminal", .create = make([](QWidget* p) { return new TerminalPanel(p); }),
                 .area = Qt::BottomDockWidgetArea});
    // The UV editor opens as its own column left of the channel box.
    panels::add({.id = kUvEditorDock, .title = "UV Editor", .create = make([](QWidget* p) { return new UvEditor(p); }),
                 .area = Qt::RightDockWidgetArea});
    panels::add({.id = kChannelBoxDock, .title = "Channel Box", .create = make([](QWidget* p) { return new ChannelBox(p); }),
                 .area = Qt::RightDockWidgetArea, .place = [this, dock](QDockWidget* channels) {
                     // Back to its own width when its tab is picked after a wider one, such as the modeling toolkit.
                     channels->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth);
                     splitDockWidget(dock(kUvEditorDock), channels, Qt::Horizontal);
                     // Split before the tabs join, so the layer editor sits under the whole channel box tab group as in Maya.
                     splitDockWidget(channels, dock(kDisplayLayersDock), Qt::Vertical);
                 }});
    panels::add({.id = "attributeEditor", .title = "Attribute Editor", .create = make([](QWidget* p) { return new AttributeEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .place = [this, dock](QDockWidget* d) { tabifyDockWidget(dock(kChannelBoxDock), d); }});
    auto tabWithAttributes = [this, dock](QDockWidget* d) { tabifyDockWidget(dock("attributeEditor"), d); };
    panels::add({.id = kModelingToolkitDock, .title = "Modeling Toolkit", .create = make([](QWidget* p) { return new ModelingToolkit(p); }),
                 .area = Qt::RightDockWidgetArea, .open = false, .place = [tabWithAttributes](QDockWidget* toolkit) {
                     tabWithAttributes(toolkit);
                     toolkit->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 5 / 2);
                 }});
    panels::add({.id = kTopologyToolkitDock, .title = "Topology Toolkit", .create = make([](QWidget* p) { return new TopologyToolkit(p); }),
                 .area = Qt::RightDockWidgetArea, .open = false, .place = [tabWithAttributes](QDockWidget* toolkit) {
                     tabWithAttributes(toolkit);
                     toolkit->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 2);
                 }});
    panels::add({.id = kModifierStackDock, .title = "Modifier Stack", .create = make([](QWidget* p) { return new ModifierStackWidget(p); }),
                 .area = Qt::RightDockWidgetArea, .place = tabWithAttributes});
    panels::add({.id = kMaterialEditorDock, .title = "Material Editor", .create = make([](QWidget* p) { return new MaterialEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .place = [tabWithAttributes, dock](QDockWidget* materials) {
                     tabWithAttributes(materials);
                     // Swatches and texture slots need more room than the channel box's width.
                     materials->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 5 / 4);
                     dock(kChannelBoxDock)->raise();
                 }});
    panels::add({.id = kTimeSliderDock, .title = "Time Slider", .create = make([](QWidget* p) { return new TimeSlider(p); }),
                 .area = Qt::BottomDockWidgetArea, .place = [](QDockWidget* timeSlider) {
                     timeSlider->setTitleBarWidget(new QWidget(timeSlider));
                     // Fixed height, so a bottom area left with only the slider shrinks to it instead of handing it the space.
                     timeSlider->widget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
                 }});
    panels::add({.id = kGraphEditorDock, .title = "Graph Editor", .create = make([](QWidget* p) { return new GraphEditor(p); }),
                 .area = Qt::BottomDockWidgetArea, .open = false});
    panels::add({.id = kNodeEditorDock, .title = "Node Editor", .create = make([](QWidget* p) { return new NodeEditor(p); }),
                 .area = Qt::BottomDockWidgetArea, .open = false, .place = [this, dock](QDockWidget* nodeDock) {
                     arrangeBottomDocks(this, {dock("scriptOutput"), dock(kScriptEditorDock), dock(kTerminalDock), dock(kGraphEditorDock), nodeDock},
                                        dock(kScriptEditorDock), dock(kTimeSliderDock));
                 }});
}

void MainWindow::createPanelDock(const panels::Panel& p)
{
    const QString name = QString::fromStdString(p.id);
    if (!p.create || findChild<QDockWidget*>(name))
        return;
    QWidget* content = nullptr;
    try
    {
        content = p.create(this);
    }
    catch (const std::exception& e)
    {
        log::error("panel {}: {}", p.id, e.what());
    }
    if (!content)
        return;
    QDockWidget* d = addDock(QString::fromStdString(p.title), name, content, p.area);
    if (!p.icon.empty())
        d->setWindowIcon(icon(QString::fromStdString(p.icon)));
    if (p.place)
        p.place(d);
    if (!p.open)
        d->hide();
    if (mMenus)
        mMenus->scheduleRebuild();
}

void MainWindow::removePanelDock(const std::string& id)
{
    QDockWidget* d = findChild<QDockWidget*>(QString::fromStdString(id));
    if (!d)
        return;
    if (mSidebars->isCollapsed(d))
        mSidebars->expand(d);
    removeDockWidget(d);
    // Now rather than later: an extension's panel code goes away with its library right after.
    delete d;
    if (mMenus)
        mMenus->scheduleRebuild();
}

namespace
{
/** Shown on screen: for a tabbed dock, its tab is the current one. */
bool showingTab(QDockWidget* d)
{
    return dockShowing(d);
}

/** Closes the widget it filters on Escape (which a popup QLineEdit otherwise ignores) or on the key bound to toggleAction. */
struct EscapeCloses final : QObject
{
    EscapeCloses(QObject* parent, std::string toggleAction = {}) : QObject(parent), toggleAction(std::move(toggleAction)) {}
    std::string toggleAction;
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress && e->type() != QEvent::ShortcutOverride)
            return false;
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() != Qt::Key_Escape && (toggleAction.empty() || !Hotkeys::get().isKeyFor(k, toggleAction)))
            return false;
        e->accept();
        if (e->type() == QEvent::KeyPress)
            static_cast<QWidget*>(obj)->close();
        return true;
    }
};
} // namespace

void MainWindow::toggleChannelBox()
{
    QDockWidget* channels = findChild<QDockWidget*>(kChannelBoxDock);
    if (!channels)
        return;
    QList<QDockWidget*> group = tabifiedDockWidgets(channels);
    if (!showingTab(channels))
    {
        for (QDockWidget* d : group)
            if (showingTab(d))
                mTabBeforeChannelBox = d;
        revealDock(channels);
        return;
    }
    QDockWidget* back = mTabBeforeChannelBox && group.contains(mTabBeforeChannelBox) ? mTabBeforeChannelBox.data() : nullptr;
    QDockWidget* attrs = findChild<QDockWidget*>("attributeEditor");
    if (!back && (group.contains(attrs) || group.isEmpty()))
        back = attrs;
    for (QDockWidget* d : group)
        if (!back && d->toggleViewAction()->isChecked())
            back = d;
    revealDock(back ? back : attrs);
}

void MainWindow::toggleUvEditor()
{
    QDockWidget* uv = findChild<QDockWidget*>(kUvEditorDock);
    if (showingTab(uv))
        mSidebars->collapse(uv);
    else
        revealDock(uv);
}

void MainWindow::run(const char* command, const json& call)
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

namespace
{
/**
 * Restores, raises and activates a window even while another app is in front, as when Lila is closed from the
 * taskbar: Windows refuses SetForegroundWindow to a background process unless it shares the foreground thread's input.
 */
void bringToFront(QWidget* w)
{
    if (w->isMinimized())
        w->showNormal();
    w->raise();
    w->activateWindow();
#ifdef _WIN32
    HWND hwnd = HWND(w->winId());
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    HWND front = GetForegroundWindow();
    DWORD frontThread = front ? GetWindowThreadProcessId(front, nullptr) : 0, self = GetCurrentThreadId();
    bool attached = frontThread && frontThread != self && AttachThreadInput(frontThread, self, TRUE);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    if (attached)
        AttachThreadInput(frontThread, self, FALSE);
#endif
}
} // namespace

bool MainWindow::confirmDiscard()
{
    if (!io::modified())
        return true;
    QString name = io::displayName() == "untitled" ? QString("untitled scene") : QString::fromStdString(io::displayName());
    bringToFront(this);
    QMessageBox box(QMessageBox::Question, "Unsaved changes", "Save changes to " + name + "?",
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Save);
    QTimer::singleShot(0, &box, [&box] { bringToFront(&box); });
    auto choice = QMessageBox::StandardButton(box.exec());
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save)
        return saveScene(false);
    return true;
}

bool MainWindow::saveScene(bool saveAs, const std::filesystem::path& target)
{
    std::filesystem::path path = target.empty() ? io::currentPath() : target;
    if (target.empty() && (saveAs || path.empty()))
    {
        QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
        if (!path.empty())
            start = QString::fromStdWString(path.wstring());
        QString chosen = QFileDialog::getSaveFileName(this, "Save Scene", start, "Lila binary (*.lila);;Lila text (*.lilaa)");
        if (chosen.isEmpty())
            return false;
        path = chosen.toStdWString();
    }
    else if (target.empty() && io::upToDate(path))
    {
        log::info("No changes to save");
        return true;
    }
    // Read-only files under source control get checked out first (or we ask, per the autoCheckout pref).
    std::string why;
    vcs::WriteAccess access = vcs::SourceControl::get().requestWrite(path, &why);
    if (access == vcs::WriteAccess::NeedsPrompt)
    {
        auto answer = QMessageBox::question(this, "Check out?", QString::fromStdString(why + "\n\nCheck it out and save?"));
        if (answer != QMessageBox::Yes)
            return false;
        access = vcs::SourceControl::get().requestWrite(path, &why, true);
    }
    if (access == vcs::WriteAccess::Denied)
    {
        QMessageBox::warning(this, "Save failed", QString::fromStdString(why));
        return false;
    }
    try
    {
        ops::execute("saveScene", {{"args", {path.generic_string()}}}, CallSource::UI);
        addRecentFile(path);
        vcs::markForAddIfNew(path);
        refreshSourceStatus();
        return true;
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Save failed", e.what());
        return false;
    }
}

void MainWindow::openScene(const std::filesystem::path& given)
{
    if (mStartupDialog)
        mStartupDialog->close();
    if (!confirmDiscard())
        return;
    std::filesystem::path path = given;
    if (path.empty())
    {
        QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
        QString chosen = QFileDialog::getOpenFileName(this, "Open Scene", start,
                                                      QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Open, "All supported")));
        if (chosen.isEmpty())
            return;
        path = chosen.toStdWString();
    }
    try
    {
        io::translators::run(io::translators::Op::Open, path, json::object(), CallSource::UI);
        addRecentFile(path);
        refreshSourceStatus();
        mViewports->frameUnlessRestored();
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Open failed", e.what());
    }
}

void MainWindow::openRecoveredScene(const std::filesystem::path& file)
{
    if (!confirmDiscard())
        return;
    try
    {
        ops::execute("openRecoveredScene", {{"args", {QString::fromStdWString(file.generic_wstring()).toStdString()}}, {"force", true}}, CallSource::UI);
        mViewports->frameUnlessRestored();
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Open failed", e.what());
    }
}

void MainWindow::incrementAndSave()
{
    if (io::currentPath().empty())
    {
        saveScene(true);
        return;
    }
    saveScene(false, io::history::nextIncrement(io::currentPath()));
}

void MainWindow::showVersions()
{
    auto* dialog = new VersionsDialog(io::currentPath(),
                                      {[this] { return confirmDiscard(); },
                                       [this] {
                                           refreshSourceStatus();
                                           mViewports->frameUnlessRestored();
                                       }},
                                      this);
    dialog->show();
}

namespace
{
/** The crash report about a session being recovered, else the newest GUI crash the user has not seen yet. */
std::filesystem::path pendingCrashReport(const std::vector<io::recovery::Session>& stale, bool recovering)
{
    for (const std::filesystem::path& dir : crash::reports())
    {
        int64_t pid = crash::reportPid(dir);
        bool fromStale = std::any_of(stale.begin(), stale.end(), [&](const io::recovery::Session& s) { return s.pid == pid; });
        if (crash::acknowledged(dir) && !(fromStale && recovering))
            continue;
        if (fromStale || crash::reportField(dir, "app", "mode") == "gui")
            return dir;
    }
    return {};
}
} // namespace

void MainWindow::startup(const std::vector<io::recovery::Session>& stale, bool showStartup)
{
    if (themeChooserPending())
        runFirstRunSetup(this);
    std::vector<io::recovery::Candidate> candidates = io::recovery::findRecoverable(stale);
    io::recovery::keep(candidates);
    io::recovery::forget(stale);
    io::recovery::prune(PrefStore::get().value<int>("general", "/recovery/keepDays", 30));
    std::filesystem::path crashReport = pendingCrashReport(stale, !candidates.empty());
    if (!crashReport.empty() && candidates.empty())
    {
        RecoveryDialog dialog(candidates, crashReport, this);
        dialog.exec();
        crash::acknowledge(crashReport);
    }
    if (!candidates.empty())
    {
        RecoveryDialog dialog(candidates, crashReport, this);
        dialog.exec();
        if (!crashReport.empty())
            crash::acknowledge(crashReport);
        const io::recovery::Candidate& c = dialog.selected();
        try
        {
            if (dialog.choice() == RecoveryDialog::Choice::Recover)
            {
                io::loadVersion(c.file, c.scene, !c.scene.empty(), "untitled (recovered)");
                if (!c.scene.empty())
                    addRecentFile(c.scene);
                mViewports->frameUnlessRestored();
                return;
            }
            if (dialog.choice() == RecoveryDialog::Choice::OpenSaved)
            {
                openScene(c.scene);
                return;
            }
        }
        catch (const std::exception& e)
        {
            QMessageBox::warning(this, "Recovery failed", e.what());
        }
    }
    if (!showStartup)
        return;
    const bool welcome = PrefStore::get().value<bool>("general", kShowWelcomePref, true);
    if (welcome || PrefStore::get().value<bool>("general", kShowRecentPref, true))
        showStartupDialog(welcome);
}

void MainWindow::showStartupDialog(bool welcome)
{
    if (mStartupDialog)
        mStartupDialog->close();
    StartupDialog::Hooks hooks{
        .open = [this](const std::filesystem::path& p) { openScene(p); },
        .openExample = [this](const io::examples::Example& x) { openExample(x); },
        .newScene = [] { actions::run("file.new"); },
    };
    auto* dialog = new StartupDialog(std::move(hooks), welcome, this);
    mStartupDialog = dialog;
    dialog->present();
}

void MainWindow::openExample(const io::examples::Example& example)
{
    if (!confirmDiscard())
        return;
    try
    {
        ops::execute("openExample", {{"args", {example.name}}, {"force", true}}, CallSource::UI);
        mViewports->frameUnlessRestored();
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Open failed", e.what());
    }
}

namespace
{
void runTranslator(io::translators::Op op, const QString& file, const std::string& translator)
{
    try
    {
        io::translators::run(op, std::filesystem::path(file.toStdWString()), json::object(), CallSource::UI, translator);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}
} // namespace

void MainWindow::importFile()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString file = QFileDialog::getOpenFileName(this, "Import", start,
                                                QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Import, "Scenes and meshes")));
    if (file.isEmpty())
        return;
    runTranslator(io::translators::Op::Import, file, {});
}

void MainWindow::convertMayaFolder()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString dir = QFileDialog::getExistingDirectory(this, "Convert Maya Files in Folder", start);
    if (dir.isEmpty() || !confirmDiscard())
        return;
    try
    {
        json results = ops::execute("mayaConvert", {{"source", dir.toStdString()}, {"force", true}}, CallSource::UI);
        int failed = 0;
        for (const json& r : results)
            failed += r.contains("error") ? 1 : 0;
        QMessageBox::information(this, "Convert Maya Files",
                                 QString("Converted %1 of %2 file(s) to .lila next to the originals. Details are in the log.")
                                     .arg(int(results.size()) - failed)
                                     .arg(int(results.size())));
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Convert Maya Files", e.what());
    }
}

void MainWindow::exportSelection()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString selected;
    QString file = QFileDialog::getSaveFileName(this, "Export Selection", start,
                                                QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Export, {})), &selected);
    if (file.isEmpty())
        return;
    // The chosen filter entry names the format when the typed name has no extension a translator knows.
    std::string translator;
    if (!io::translators::forPath(io::translators::Op::Export, file.toStdWString()))
        for (const io::translators::FileTranslator* t : io::translators::supporting(io::translators::Op::Export))
            if (selected.startsWith(QString::fromStdString(t->label + " (")))
            {
                translator = t->id;
                if (!t->extensions.empty())
                    file += QString::fromStdString(t->extensions.front());
            }
    runTranslator(io::translators::Op::Export, file, translator);
}

void MainWindow::sendToUnreal()
{
    // First send asks where each object goes; after that, saving the scene re-exports and reimports it.
    json selection = ops::execute("ls", {{"selection", true}});
    if (selection.empty())
    {
        QMessageBox::information(this, "Send to Unreal", "Select the objects to send first.");
        return;
    }
    json existing = ops::execute("unrealMappings", json::object());
    auto leafName = [](const std::string& path) { return path.substr(path.rfind('|') + 1); };
    for (const json& name : selection)
    {
        bool mapped = false;
        for (const json& m : existing)
            mapped = mapped || m.value("node", "") == name.get<std::string>();
        if (mapped)
            continue;
        bool ok = false;
        QString path = QInputDialog::getText(this, "Send to Unreal", QString::fromStdString("Content path for " + name.get<std::string>()),
                                             QLineEdit::Normal, QString::fromStdString("/Game/Meshes/SM_" + leafName(name.get<std::string>())), &ok);
        if (!ok || path.isEmpty())
            return;
        run("unrealMap", {{"args", {name}}, {"contentPath", path.toStdString()}, {"kind", "static"}});
    }
    run("unrealSync");
}

void MainWindow::refreshSourceStatus()
{
    QPointer<QLabel> label = mSourceLabel;
    sceneSourceStatus(io::currentPath(), [label](QString text) {
        if (label)
            label->setText(text);
    });
}

void MainWindow::addRecentFile(const std::filesystem::path& path)
{
    json recent = PrefStore::get().value("general", "/recentFiles", json::array());
    if (!recent.is_array())
        recent = json::array();
    std::string s = path.generic_string();
    recent.erase(std::remove(recent.begin(), recent.end(), json(s)), recent.end());
    recent.insert(recent.begin(), s);
    if (recent.size() > 15)
        recent.erase(recent.begin() + 15, recent.end());
    PrefStore::get().set("general", "/recentFiles", recent);
}

void MainWindow::refreshTitle()
{
    auto& ws = Workspace::get();
    QString scene = QString::fromStdString(io::displayName());
    QString title = scene + (io::modified() ? "*" : "");
    if (ws.isOpen())
    {
        std::filesystem::path root = ws.root();
        if (root.has_parent_path() && root.parent_path() != root.root_path())
            title += " - " + QString::fromStdWString(root.parent_path().filename().wstring()) + "\\" + QString::fromStdWString(root.filename().wstring());
        else
            title += " - " + QString::fromStdWString(root.filename().wstring());
    }
    setWindowTitle(title + " - Lila");
}

void MainWindow::registerActions()
{
    actions::add({"file.new", "New Scene", "File", [this] {
                      if (confirmDiscard())
                          actions::run(json{{"command", "newScene"}});
                  }, nullptr, "Clears the scene and starts an empty one, asking to save changes first."});
    actions::add({"file.open", "Open Scene...", "File", [this] { openScene({}); }, nullptr, "Opens a Lila or Maya scene, replacing the current one. An FBX, OBJ or other importable file opens as a new untitled scene with it imported."});
    actions::add({"file.save", "Save Scene", "File", [this] { saveScene(false); }, nullptr, "Saves the scene to its file, asking for a name the first time."});
    actions::add({"file.saveAs", "Save Scene As...", "File", [this] { saveScene(true); }, nullptr, "Saves the scene under a new name and keeps working in that file."});
    actions::add({"file.incrementAndSave", "Increment and Save", "File", [this] { incrementAndSave(); }, nullptr,
                  "Save as the next free scene_v###, then keep working in it", "incrementAndSave()"});
    actions::add({"file.welcome", "Welcome...", "File", [this] { showStartupDialog(true); }, nullptr,
                  "The welcome dialog: example scenes and recent scenes."});
    actions::add({"file.versions", "Versions...", "File", [this] { showVersions(); }, nullptr,
                  "Backups, autosaves, emergency saves, increments and source control revisions of this scene",
                  "sceneVersions()"});
    actions::add({"file.openRecoveryFolder", "Open Recovery Folder", "File",
                  [] {
                      std::error_code ec;
                      std::filesystem::create_directories(io::recovery::recoveredDir(), ec);
                      QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(io::recovery::recoveredDir().wstring())));
                  },
                  nullptr, "Shows the folder of scenes recovered after a crash in the file browser."});
    actions::add({"file.clearRecoveredScenes", "Clear Recovered Scenes", "File",
                  [this] {
                      if (QMessageBox::question(this, "Clear Recovered Scenes", "Delete every recovered scene? This can't be undone.") == QMessageBox::Yes)
                          ops::execute("clearRecoveredScenes", json::object(), CallSource::UI);
                  },
                  nullptr, "Deletes the scenes recovered after crashes, listed under File > Recovered Scenes.", "clearRecoveredScenes()"});
    actions::add({"file.openFolder", "Open Folder...", "File", [this] { openFolder(); }, nullptr,
                  "Opens a project folder: file browsing, source control and workspace layouts work from it."});
    actions::add({"file.closeFolder", "Close Folder", "File", [] { Workspace::get().close(); }, nullptr, "Closes the project folder; the scene stays open."});
    actions::add({"file.exit", "Exit", "File", [this] { close(); }, nullptr, "Quits Lila, asking to save changes first."});
    actions::add({.id = "viewport.escape",
                  .label = "Escape",
                  .category = "View",
                  .run =
                      [this] {
                          if (scene::anim::playDirection() != 0)
                              actions::run("anim.stop");
                          else
                              mViewports->active()->escape();
                      },
                  .doc = "Stops playback, else cancels what the view is doing, else drops the object selection."});
    actions::add({"viewport.frameSelected", "Frame Selected", "View", [this] { mViewports->active()->frameSelection(false); }, nullptr,
                  "Moves the active view's camera to fit the selection."});
    actions::add({"viewport.frameAll", "Frame All", "View", [this] { mViewports->active()->frameSelection(true); }, nullptr,
                  "Moves the active view's camera to fit everything in the scene."});
    actions::add({"viewport.cameraUndo", "Undo View Change", "View", [this] { mViewports->active()->cameraUndo(); }, nullptr,
                  "Puts the active view's camera back where it was before its last move. Separate from scene undo."});
    actions::add({"viewport.cameraRedo", "Redo View Change", "View", [this] { mViewports->active()->cameraRedo(); }, nullptr,
                  "Redoes the camera move that Undo View Change took back."});
    actions::add({"file.import", "Import...", "File", [this] { importFile(); }, nullptr, "Adds the contents of an FBX, OBJ or Maya file to the scene.",
                  "importFbx(path)  # or importObj(path), mayaImport(path)"});
    actions::add({"file.convertMaya", "Convert Maya Files...", "File", [this] { convertMayaFolder(); }, nullptr,
                  "Converts every .ma and .mb file in a folder to a .lila scene.", "mayaConvert(source=folder)  # every .ma / .mb in it to .lila"});
    actions::add({"file.exportSelection", "Export Selection...", "File", [this] { exportSelection(); }, nullptr,
                  "Writes the selected objects to an FBX or OBJ file, with their modifiers applied.",
                  "exportFbx(path, profile=\"ue5\")  # or exportObj(path)"});
    actions::add({"file.createReference", "Create Reference...", "File", [this] { ReferenceEditor::createReferenceDialog(this); }, nullptr,
                  "References a Lila scene or a Maya .ma / .mb file into this one under a namespace; Maya files are converted to a .lila beside them.",
                  "createReference(path)  # or mayaReference(path) for .ma / .mb"});
    actions::add({"unreal.send", "Send to Unreal", "Unreal", [this] { sendToUnreal(); }, nullptr,
                  "Maps the selection to Unreal assets and exports it to the connected editor.",
                  "unrealMap(node, contentPath=\"/Game/Meshes/SM_Name\"); unrealSync()"});
    actions::add({"unreal.sync", "Sync to Unreal", "Unreal", [this] { run("unrealSync"); }, nullptr,
                  "Exports and reimports every object already mapped to an Unreal asset, as saving does.", "unrealSync()"});
    actions::add({"unreal.connect", "Connect to Unreal", "Unreal", [this] { run("unrealConnect"); }, nullptr,
                  "Connects to an Unreal Editor running the LilaLink plugin.", "unrealConnect()"});
    for (const char* view : {"persp", "top", "front", "right", "left", "back", "bottom"})
    {
        std::string label = std::string(1, char(std::toupper(view[0]))) + (view + 1) + " View";
        std::string doc = std::string(view) == "persp" ? "Switches the active view to the perspective camera."
                                                       : std::string("Switches the active view to the orthographic ") + view + " camera.";
        actions::add({std::string("viewport.view.") + view, label, "View", [this, view] { mViewports->active()->setView(view); }, nullptr, doc,
                      std::string("lila.ui.run_action(\"viewport.view.") + view + "\")"});
    }
    auto selectedCamera = []() -> scene::Node* {
        scene::Graph& g = scene::Graph::current();
        for (scene::NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("camera"))
                return n;
        return nullptr;
    };
    actions::add({"viewport.lookThroughSelected", "Look Through Selected", "View",
                  [this, selectedCamera] {
                      if (scene::Node* n = selectedCamera())
                          run("lookThru", {{"args", {n->path()}}});
                  },
                  [selectedCamera] { return selectedCamera() != nullptr; }, "The active view looks through the selected camera; navigating it then moves the camera.",
                  "lila.ops.lookThru(\"camera1\")"});
    actions::add({"create.cameraFromView", "Camera from View", "Create", [this] { createCameraFromView(); }, nullptr,
                  "Creates a camera where the active view's camera is, with its lens, and selects it."});
    ops::registerCommand({
        .name = "lookThru",
        .doc = "Make the active view look through a camera node, or switch it to a view (persp, top, front, side, ...). "
               "Returns the name of what the active view looks through; with no argument it only returns it.",
        .positionalDoc = "camera",
        .undoable = false,
        .run = [this](const Args& a) -> json {
            ViewportWidget* v = mViewports->active();
            if (a.positional.empty())
                return v->viewName();
            if (!a.positional[0].is_string())
                throw CommandError("lookThru: camera name expected");
            std::string name = a.positional[0].get<std::string>();
            if (name == "side")
                name = "right";
            v->setView(name);
            if (v->viewName() != name)
                throw CommandError("lookThru: '" + name + "' is not a camera or a view name");
            return v->viewName();
        },
    });
    actions::add({"edit.rename", "Rename...", "Edit", [this] { renameSelection(); }, actions::hasSelection,
                  "Renames the selected objects at the cursor; several get numbered names."});
    actions::add({"window.resetLayout", "Reset Layout", "Windows", [this] {
                      std::string current = layouts::current();
                      if (current.empty() || !layouts::find(current))
                      {
                          resetLayout();
                          return;
                      }
                      try
                      {
                          ops::execute("applyLayout", {{"name", current}}, CallSource::UI);
                          fitToScreen();
                      }
                      catch (const CommandError& e)
                      {
                          log::error("{}", e.what());
                      }
                  }, nullptr, "Puts every panel back where the current layout has it, keeping that layout picked. With none picked, "
                              "back to the default arrangement."});
    actions::add({"window.toggleChannelBox", "Toggle Channel Box", "Windows", [this] { toggleChannelBox(); }, nullptr,
                  "Brings the channel box to the front of its tabs; again to go back to the tab shown before it."});
    actions::add({"window.toggleUvEditor", "Toggle UV Editor", "Windows", [this] { toggleUvEditor(); }, nullptr,
                  "Shows the UV editor, or tucks it back into its sidebar when it is showing."});
    actions::add({"window.expandPanels", "Expand Collapsed Panels", "Windows", [this] { mSidebars->expandAll(); }, nullptr,
                  "Opens every panel collapsed to a sidebar tab."});
    actions::add({"window.searchMenus", "Search Menus...", "Windows", [this] { menuSearch::openBox(menuBar(), this); }, nullptr,
                  "Finds any item in the main menus by typing part of its name or menu, and runs it or opens its options."});
    quickCommand::registerActions(this);
    actions::add({"window.searchScene", "Search Scene...", "Windows", [this] { menuSearch::openSceneFinder(menuBar(), this); }, nullptr,
                  "Finds any object in the scene by name, wildcard, type: or path, selects it and reveals it in the outliner."});
    auto scriptEditor = [this]() -> ScriptEditor* {
        QDockWidget* d = findChild<QDockWidget*>(kScriptEditorDock);
        return d ? qobject_cast<ScriptEditor*>(d->widget()) : nullptr;
    };
    actions::add({"script.run", "Run Script", "Script Editor", [scriptEditor] {
                      if (ScriptEditor* e = scriptEditor())
                          e->runCurrent(false);
                  }, nullptr, "Runs the script editor's selection, or its whole tab."});
    actions::add({"script.runLine", "Run Line", "Script Editor", [scriptEditor] {
                      if (ScriptEditor* e = scriptEditor())
                          e->runLine();
                  }, nullptr, "Runs the script editor's line under the caret, or the selection when there is one."});
    actions::add({"script.runCell", "Run Cell", "Script Editor", [scriptEditor] {
                      if (ScriptEditor* e = scriptEditor())
                          e->runCurrent(true);
                  }, nullptr, "Runs the script editor's cell at the caret: the lines between two # %% marker comments."});
    actions::add({"script.searchApi", "Search Scripting API...", "Script Editor", [this] {
                      QDockWidget* d = findChild<QDockWidget*>(kScriptEditorDock);
                      QPointer<ScriptEditor> editor = d ? qobject_cast<ScriptEditor*>(d->widget()) : nullptr;
                      if (!editor)
                          return;
                      menuSearch::openApiSearch(menuBar(), this,
                                                {.insert = [editor](const QString& code) {
                                                     if (!editor)
                                                         return;
                                                     CodeEditor* ed = editor->currentEditor();
                                                     ed = ed ? ed : editor->newTab();
                                                     ed->insertCall(code);
                                                 },
                                                 .showDocs = [editor](const QString& key) {
                                                     if (editor)
                                                         emit editor->showDocs(key);
                                                 }});
                  }, nullptr,
                  "Finds a command, flag or lila Python class or function by name or description and inserts the call into the script editor."});
    actions::add({"window.preferences", "Preferences...", "Windows", [this] {
                      if (!closeShownWindow<PreferencesDialog>())
                          PreferencesDialog::open(this);
                  }, nullptr,
                  "Settings for the viewport, manipulators, files, Python and the rest of Lila."});
    actions::add({"window.extensionManager", "Extension Manager...", "Windows", [this] {
                      if (!closeShownWindow<ExtensionManagerDialog>())
                          ExtensionManagerDialog::open(this);
                  }, nullptr,
                  "Load, unload and auto load extensions, and see their versions and load errors."});
    actions::add({"viewport.isolate", "Isolate Select", "View", [this] { mViewports->active()->toggleIsolate(); },
                  [this] { return mViewports->active()->isolating() || actions::hasSelection(); },
                  "Shows only the selection in the active view: whole objects, or just the faces of selected components. Again to show everything."});
    actions::add({"viewport.isolateAdd", "Add Selected to Isolation", "View", [this] { mViewports->active()->addToIsolation(); },
                  [] { return actions::hasSelection(); },
                  "Adds the selected objects or components to what the active view isolates, starting isolation if it is off."});
    actions::add({"viewport.isolateRemove", "Remove Selected from Isolation", "View", [this] { mViewports->active()->removeFromIsolation(); },
                  [this] { return mViewports->active()->isolating() && actions::hasSelection(); },
                  "Hides the selected objects or components in the active view's isolation."});
    imagePlanes::install([this] { return mViewports->active(); });
    sets::registerActions(this);
    GameExporter::registerActions(this);
    MeshSequenceExporter::registerActions(this);
    animatedSend::registerActions(this);
    hud::registerActions([this] { return mViewports->active(); });
    displayAids::registerActions([this] { return mViewports->active(); });
    lodDisplay::registerActions([this] { return mViewports->active(); });
    toolBox::registerItems([this] { return mViewports->active(); });
    cursorBadges::registerDefaults();
    actions::add({"viewport.toggleQuad", "Toggle Four Views", "View", [this] { mViewports->toggleQuad(); }, nullptr,
                  "Switches between one view and the top, front, side and perspective views together."});
    auto toggleGate = [this](const char* flag, const char* key) {
        std::string cam = mViewports->active()->viewName();
        bool on = scene::renderSettings().camera(cam).toJson()[key].get<bool>();
        try
        {
            ops::execute("cameraSettings", {{"args", json::array({cam})}, {flag, !on}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    };
    actions::add({"viewport.toggleResolutionGate", "Resolution Gate", "View", [toggleGate] { toggleGate("gate", "resolutionGate"); },
                  nullptr, "Shows the render resolution's frame in the active view, darkening outside it.",
                  "lila.ops.cameraSettings(\"persp\", gate=True)"});
    actions::add({"viewport.toggleFilmGate", "Film Gate", "View", [toggleGate] { toggleGate("filmGate", "filmGate"); }, nullptr,
                  "Shows the camera's film back in the active view.", "lila.ops.cameraSettings(\"persp\", filmGate=True)"});
    actions::add({"viewport.cameraSettings", "Camera Settings...", "View",
                  [this] { RenderSettingsDialog::open(this, mViewports->active()->viewName()); }, nullptr,
                  "The active view's camera gates, film fit, overscan and gate mask."});
    actions::add({"window.renderSettings", "Render Settings...", "Windows", [this] {
                      if (!closeShownWindow<RenderSettingsDialog>())
                          RenderSettingsDialog::open(this);
                  }, nullptr,
                  "Render resolution and per-camera settings.", "lila.ops.setRenderResolution(width=1920, height=1080)"});
    actions::add({"window.toggleMaximizePanel", "Maximize Viewport", "Windows", [this] {
                      if (!mMaximizeHidden.empty())
                      {
                          for (auto& [d, showing] : mMaximizeHidden)
                              if (d)
                                  d->show();
                          for (auto& [d, showing] : mMaximizeHidden)
                              if (d && showing)
                                  d->raise();
                          mMaximizeHidden.clear();
                          scheduleLayoutSave();
                          return;
                      }
                      for (QDockWidget* d : findChildren<QDockWidget*>())
                          if (d->parentWidget() == this && !d->isHidden() && !d->isFloating())
                              mMaximizeHidden.emplace_back(d, d->isVisible());
                      for (auto& [d, showing] : mMaximizeHidden)
                          d->hide();
                      scheduleLayoutSave();
                  }, nullptr, "Hides every docked panel so the viewports fill the window; again to bring them back."});
    actions::add({"help.about", "About Lila", "Help", [this] { showAbout(); }, nullptr, "Version and copyright."});
    actions::add({"help.prefsFolder", "Open Prefs Folder", "Help", [] {
                      QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(PrefStore::get().userDir().wstring())));
                  }, nullptr, "Opens the folder holding your preferences, hotkeys, shelves and layouts."});
    actions::add({"help.crashReports", "Open Crash Reports Folder", "Help", [] { openCrashFolder(crash::reportsDir()); }, nullptr,
                  "One folder per crash: report.txt (what crashed, stacks, log, system) and a minidump."});
    actions::add({"help.copyLastCrash", "Copy Last Crash Report", "Help", [this] {
                      std::vector<std::filesystem::path> all = crash::reports();
                      if (all.empty())
                      {
                          QMessageBox::information(this, "Copy Last Crash Report", "Lila has no crash reports.");
                          return;
                      }
                      copyCrashReport(all.front());
                      statusBar()->showMessage("Crash report copied: " + QString::fromStdWString(all.front().wstring()), 5000);
                  }, nullptr, "Puts the newest crash report on the clipboard, ready to paste into a bug report."});
}

QMenu* MainWindow::refreshKeysOnShow(QMenu* menu)
{
    menu->setToolTipsVisible(true);
    // Hotkeys are live-editable, so the key column is refreshed each time the menu opens.
    connect(menu, &QMenu::aboutToShow, menu, [menu] {
        for (QAction* qa : menu->actions())
        {
            std::string id = qa->data().toString().toStdString();
            if (id.empty())
                continue;
            QString key = Hotkeys::get().keyFor(id);
            qa->setText(QString::fromStdString(actions::displayLabel(id)) + (key.isEmpty() ? "" : "	" + key));
            qa->setToolTip(QString::fromStdString(actions::tooltip(id)));
            qa->setEnabled(actions::isEnabled(json(id)));
        }
    });
    optionBox::decorateMenu(menu);
    return menu;
}

void MainWindow::buildMenus()
{
    using namespace menus;
    auto menu = [](const char* id, const char* title, Area area = Area::MenuBar, std::function<void(QMenu*)> setup = {}) {
        addMenu({.id = id, .title = title, .area = area, .setup = std::move(setup)});
    };

    menu("File", "File");
    menus::addActions("File", {"#Scene", "file.new", "file.open"});
    addSubmenu("File", "Recent Scenes", [this](QMenu* recent) {
        recent->setProperty(menuSearch::kNoSearchProperty, true);
        connect(recent, &QMenu::aboutToShow, this, [this, recent] {
            recent->clear();
            json files = PrefStore::get().value("general", "/recentFiles", json::array());
            for (const json& r : files.is_array() ? files : json::array())
                if (r.is_string())
                {
                    std::string p = r.get<std::string>();
                    recent->addAction(QString::fromStdString(p), [this, p] { openScene(std::filesystem::path(p)); });
                }
            if (recent->isEmpty())
                recent->addAction("(none)")->setEnabled(false);
        });
    });
    addSubmenu("File", "Recovered Scenes", [this](QMenu* menu) {
        menu->setProperty(menuSearch::kNoSearchProperty, true);
        menu->setToolTipsVisible(true);
        connect(menu, &QMenu::aboutToShow, this, [this, menu] {
            menu->clear();
            for (const io::recovery::Recovered& r : io::recovery::recovered())
            {
                QString text = r.scene.empty() ? QString("untitled") : QString::fromStdWString(r.scene.filename().wstring());
                text += "    " + QDateTime::fromSecsSinceEpoch(r.time).toString("yyyy-MM-dd hh:mm");
                if (r.kind == io::history::Kind::Emergency)
                    text += "    emergency save";
                if (!r.scene.empty())
                    text += "    " + QString::fromStdWString(r.scene.generic_wstring());
                QAction* a = menu->addAction(text, [this, file = r.file] { openRecoveredScene(file); });
                a->setToolTip("Opens as a new untitled scene. Recovered from " + QString::fromStdWString(r.file.wstring()));
            }
            if (menu->isEmpty())
                menu->addAction("(none)")->setEnabled(false);
            menu->addSeparator();
            for (const char* id : {"file.openRecoveryFolder", "file.clearRecoveredScenes"})
                menu->addAction(QString::fromStdString(actions::displayLabel(id)), [id] { actions::run(id); })
                    ->setToolTip(QString::fromStdString(actions::tooltip(id)));
        });
    });
    menus::addActions("File", {"file.save", "file.saveAs", "file.incrementAndSave", "file.versions", "#Import and Export", "file.import", "file.convertMaya",
                        "file.exportSelection", "file.gameExporter", "file.exportMeshSequence", "window.batchRun", "file.createReference", "#Unreal", "unreal.send", "animated.sendOptions", "unreal.sync", "unreal.sendBake",
                        "deform.exportMLDeformer", "unreal.connect", "unreal.collision", "#Workspace", "file.openFolder"});
    addSubmenu("File", "Recent Folders", [this](QMenu* recent) {
        recent->setProperty(menuSearch::kNoSearchProperty, true);
        mRecentMenu = recent;
        refreshRecentFolders();
    });
    menus::addActions("File", {"file.closeFolder", "", "file.welcome", "", "file.exit"});

    menu("Edit", "Edit");
    menus::addActions("Edit", {"#History", "edit.undo", "edit.redo", "edit.repeatLast", "adjustLastOp.show", "edit.flushUndo", "edit.flushRedo", "edit.flushAll", "#Selection",
                        "edit.selectAll", "edit.deselect", "edit.invertSelection", "edit.delete"});
    addSubmenu("Edit", "Quick Select Sets", [](QMenu* m) { sets::attachQuickSelectMenu(m); });
    menus::addActions("Edit", {"#Hierarchy", "edit.duplicate", "edit.duplicateWithTransform", "edit.instance", "edit.instanceReference", "edit.array", "edit.bakeArray",
                        "edit.makeUnique", "edit.convertToInstance", "select.instances", "select.instanceSource", "edit.group", "edit.lodGroup", "edit.generateLods", "edit.parent", "edit.parentChain", "edit.unparent", "#Naming", "edit.rename", "edit.batchRename", "edit.conformNames", "#Settings",
                        "window.preferences", "window.extensionManager"});

    menu("Modify", "M&odify");
    menus::addActions("Modify", {"#Transforms", "modify.freezeTransforms", "modify.resetTransforms", "modify.centerPivot", "modify.resetPivot",
                                 "#Snapping", "snapSurface.set", "snapSurface.clear", "snapSurface.toggle", "snapSurface.alignToNormal"});

    menu("Create", "Create");
    addSubmenu("Create", "Polygon Primitives");
    menus::addActions("Create/Polygon Primitives", {"create.cube", "create.sphere", "create.cylinder", "create.capsule", "create.cone", "create.plane", "create.torus",
                                             "create.disc", "create.pipe", "create.quadSphere", "create.icoSphere"});
    menus::addActions("Create", {"create.locator", "create.camera", "create.cameraFromView", "create.emptyGroup", "create.imagePlane"});
    addSubmenu("Create", "Sets");
    menus::addActions("Create/Sets", {"sets.create", "sets.createQuickSelect"});

    // Toggles show their state, read from the active view when they are per view.
    auto syncChecks = [this](QMenu* m) {
        connect(m, &QMenu::aboutToShow, m, [this, m] {
            for (QAction* qa : m->actions())
                if (std::optional<bool> on = viewportActionState(qa->data().toString().toStdString(), mViewports->active()))
                {
                    qa->setCheckable(true);
                    qa->setChecked(*on);
                }
        });
    };
    menu("Select", "Select", Area::MenuBar, syncChecks);
    menus::addActions("Select", {"#Mode", "select.objectMode", "select.vertexMode", "select.edgeMode", "select.faceMode", "#Components", "select.allComponents",
                          "select.grow", "select.loop", "select.ring", "#Hierarchy", "select.hierarchy", "select.pickWalkUp", "select.pickWalkDown",
                          "select.pickWalkLeft", "select.pickWalkRight", "#Convert", "select.toVertices", "select.toEdges", "select.toFaces",
                          "select.containedFaces", "#Tools", "select.paintTool", "select.toggleLasso", "select.toggleCameraBased", "select.toggleBackfaceCulling"});

    menu("Mesh", "Mesh");
    menus::addActions("Mesh", {"#Combine", "mesh.combine", "mesh.smartJoin", "mesh.separate", "mesh.extract", "mesh.duplicateFaces", "mesh.boundingMesh",
                               "#Mirror", "mesh.mirror"});
    addSubmenu("Mesh", "Symmetry");
    menus::addActions("Mesh/Symmetry", {"mesh.toggleSymmetry", "mesh.symmetryX", "mesh.symmetryY", "mesh.symmetryZ", "mesh.symmetryRadial", "mesh.symmetryOff", "",
                                 "mesh.symmetryCheck", "mesh.symmetrizePositive", "mesh.symmetrizeNegative", "mesh.symmetryRebuild", "mesh.symmetrizeRadial",
                                 "mesh.symmetryRepair", "mesh.symmetryRebuildTopological"});
    menus::addActions("Mesh", {"#Topology", "mesh.triangulate", "mesh.quadrangulate", "mesh.subdivide", "#Attributes",
                               "mesh.transferAttributes"});
    menus::addActions("Mesh", {"#Cleanup", "mesh.repair", "mesh.selectIssues", "#Nodes", "window.nodeEditor"});

    menu("Topology", "Topology", Area::MenuBar, syncChecks);
    menus::addActions("Topology", {"#Session", "retopo.setReference", "window.topologyToolkit", "#Draw", "retopo.draw", "topo.pencil", "topo.patch",
                                   "topo.strip", "topo.stroke", "topo.cylinder", "#Shape", "topo.handles", "topo.relax", "topo.settle", "mesh.relax",
                                   "#Auto Retopo", "topo.region", "topo.runRegion", "mesh.quadRemesh", "retopo.poles", "mesh.voxelRemesh",
                                   "#Constructs", "topo.freezeAll", "#Display", "topo.qualityOverlay"});

    menu("Model", "Model");
    menus::addActions("Model", {"#Components", "mesh.extrude", "mesh.inset", "mesh.insetRegion", "mesh.bevel", "mesh.insertLoop", "mesh.knife", "mesh.bridge",
                                "mesh.fillHole", "mesh.poke", "#Merge", "mesh.merge", "mesh.mergeCenter", "mesh.targetWeld", "mesh.collapse", "#Edges", "mesh.dissolve",
                                "mesh.spin", "#Shape", "mesh.straighten", "mesh.circularize", "mesh.spherize", "mesh.flatten", "mesh.conformShape",
                                "mesh.conformToObject"});

    menu("Curves", "Curves");
    menus::addActions("Curves", {"#Create", "curve.bezierPen", "curve.cvTool", "curve.epTool", "curve.pencil", "curve.catmullRom", "curve.kappa",
                                 "curve.circle", "curve.fromEdges", "curve.sketchTool", "#Surfaces", "curve.extrude", "curve.revolve", "curve.loft",
                                 "curve.patchFill", "curve.network", "curve.borderBlend", "#Pipes and Cables", "curve.pipeTool", "curve.pipe",
                                 "curve.pipePresetPipe", "curve.pipePresetHose", "curve.pipePresetCable", "curve.pipePresetConduit", "curve.cableTool",
                                 "curve.cable", "#Edit", "curve.fillet", "curve.offset", "curve.rebuild", "curve.evenSpacing", "curve.fit", "curve.project",
                                 "curve.attach", "curve.detach", "curve.reverse", "curve.close", "curve.open", "curve.deletePoints", "#Blend and Trim",
                                 "curve.blendTool", "curve.blend", "curve.match", "curve.trimTool", "curve.extend", "curve.split", "curve.fair"});
    addSubmenu("Curves", "Planar");
    menus::addActions("Curves/Planar", {"curve.text", "curve.bevelPlus", "", "curve.booleanUnion", "curve.booleanSubtract", "curve.booleanIntersect",
                                        "curve.booleanXor", "", "curve.offsetRegion", "curve.insetSkeleton", "curve.centerLine", "", "curve.roof"});
    addSubmenu("Curves", "Deform Curves");
    menus::addActions("Curves/Deform Curves", {"curve.bend", "curve.curl", "curve.scaleCurvature", "curve.smooth"});
    addSubmenu("Curves", "Along Curve");
    menus::addActions("Curves/Along Curve", {"curve.deformAlongCurve", "curve.arrayAlongCurve", "curve.instanceAlongCurve", "", "curve.flowAlongSurface"});
    addSubmenu("Curves", "On Surface");
    menus::addActions("Curves/On Surface", {"curve.drawOnSurfaces", "curve.keepDrawnOnSurface", "", "curve.keepOnSurface", "curve.releaseFromSurface"});
    addSubmenu("Curves", "Sections");
    menus::addActions("Curves/Sections", {"curve.contour", "curve.sectionTool"});
    addSubmenu("Curves", "Sketch");
    menus::addActions("Curves/Sketch", {"curve.sketchTool", "", "curve.sketchCombine", "curve.sketchSolve", "curve.sketchClear"});
    addSubmenu("Curves", "Groom");
    menus::addActions("Curves/Groom", {"curve.sculptCurves", "curve.groomSurface", "curve.groomGuides", "", "curve.hairCards"});
    menus::addActions("Curves", {"#Handles", "curve.handleAuto", "curve.handleVector", "curve.handleAligned", "curve.handleFree", "#Convert",
                                 "curve.toBezier", "curve.toNurbs", "curve.toPoly", "#Display", "curve.toggleComb", "#Unreal", "curve.sendToUnreal"});

    menu("Surface", "Surface", Area::MenuBar, syncChecks);
    menus::addActions("Surface", {"#Normals", "mesh.softenEdges", "mesh.hardenEdges", "mesh.setNormalAngle", "mesh.flip", "mesh.conform",
                                  "mesh.softFoliageNormals", "#Vertex Colors", "vcolor.paint", "vcolor.createSet", "vcolor.fill", "vcolor.gradient",
                                  "vcolor.invert", "vcolor.levels", "vcolor.blur", "#Bake to Vertex Colors", "vcolor.bakeAO", "vcolor.bakeCavity",
                                  "vcolor.bakeCurvature", "vcolor.bakeHeight", "#Display", "viewport.vertexColors.off", "viewport.vertexColors.shaded",
                                  "viewport.vertexColors.unlit", "viewport.toggleBackfaceCulling"});

    menu("Sculpt", "Sculpt");
    menus::addActions("Sculpt", {"#Sculpt", "sculpt.tool", "sculpt.enter", "sculpt.exit", "sculpt.cancel", "window.sculpt", "#Shape",
                                 "mesh.relaxKeepForm", "mesh.reproject", "mesh.transferShape"});

    menu("Booleans", "Booleans");
    menus::addActions("Booleans", {"mesh.booleanUnion", "mesh.booleanDifference", "mesh.booleanIntersection"});

    // The modifier kinds fill in above these rows each time it opens.
    menu("Modifiers", "Modifiers", Area::MenuBar, [](QMenu* m) { fillAddModifierMenu(m, false); });
    menus::addActions("Modifiers", {"#Stack", "window.modifierStack", "modifier.applyAll", "mesh.bakeModifierStack", "#Auto Bevel",
                                    "select.foldEdges", "bevel.editEdges", "bevel.applyEdges", "bevel.cancelEdges", "bevel.measureThin",
                                    "bevel.clearOverrides", "bevel.showEdges"});

    menu("UV", "UV");
    menus::addActions("UV", {"window.uvEditor", "uv.auto", "uv.autoSeams", "uv.unwrap", "uv.projectPlanar", "uv.projectBox", "uv.projectCylindrical",
                      "uv.projectSpherical", "uv.cut", "uv.sew", "uv.straighten", "uv.orient", "uv.rotateLeft", "uv.rotateRight", "uv.flipX", "uv.flipY", "uv.pack", "uv.lightmap", "", "uv.autoSeams2",
                      "uv.bff", "uv.bffDisk", "uv.bffRectangle", "uv.bffCones", "uv.unwrapSymmetric", "uv.unwrapSymmetricStack"});
    addSubmenu("UV", "Pack");
    menus::addActions("UV/Pack", {"uv.pack", "uv.packAroundSelected", "uv.packSelectionOnly", "uv.packStacked", "uv.packPerObject", "uv.packUdimByMaterial", "uv.selectStacks"});
    addSubmenu("UV", "Live");
    menus::addActions("UV/Live", {"uv.liveMode", "uv.liveRelax", "uv.liveFlatten", "uv.liveSettle", "uv.pin", "uv.unpin", "uv.weld", "uv.brush3d",
                           "uv.checker3d", "uv.seams3d", "uv.includeChildren", "uv.heatmapTwoColour"});
    addSubmenu("UV", "Optimize");
    menus::addActions("UV/Optimize", {"uv.optimizeConformal", "uv.optimizeAuthalic", "uv.optimizeBlend", "uv.optimizeIsometric", "uv.mosaic"});
    addSubmenu("UV", "Align");
    menus::addActions("UV/Align", {"uv.straightenEdges", "uv.alignU", "uv.alignV", "uv.orientByEdge"});

    menu("Rig", "Rig");
    menus::addActions("Rig", {"#Skeleton", "rig.jointTool", "rig.orientJoint", "rig.mirrorJoints", "rig.insertJoint", "rig.rerootSkeleton", "rig.centerJoint",
                       "rig.snapToComponentCenter", "#Skin", "rig.bindSkin",
                       "rig.bindSkinHeatmap", "rig.unbindSkin", "rig.goToBindPose", "rig.editSkeleton", "#Weights", "rig.paintWeights", "rig.mirrorSkinWeights",
                       "rig.copySkinWeights", "rig.normalizeWeights", "rig.pruneWeights", "#Constraints", "rig.parentConstraint", "rig.pointConstraint",
                       "rig.orientConstraint", "rig.aimConstraint", "rig.removeConstraint", "#IK", "rig.ikHandle", "#Display", "rig.xrayJoints",
                       "rig.dominantInfluenceSelect"});
    addSubmenu("Rig", "Spaces");
    setupSpacesMenu();
    addSubmenu("Rig", "Skinning Method");
    menus::addActions("Rig/Skinning Method", {"rig.skinLinear", "rig.skinDualQuaternion", "rig.skinCentresOfRotation", "rig.skinDirectDeltaMush"});

    menu("Deform", "Deform");
    menus::addActions("Deform", {"#Morph Targets", "deform.morph", "deform.addMorphTarget", "window.morphTargets"});
    addSubmenu("Deform", "Morph Tools");
    menus::addActions("Deform/Morph Tools", {"deform.editMorphTarget", "deform.extractMorphTarget", "deform.setMorphTarget", "deform.addInBetween",
                                             "", "deform.flipMorphTarget", "deform.symmetrizeMorphTarget", "", "deform.removeMorphTarget"});
    addSubmenu("Deform", "Correctives");
    menus::addActions("Deform/Correctives", {"deform.sculptCorrective", "deform.addCorrective", "", "deform.poseDriver", "deform.setPoseDriverTarget"});
    menus::addActions("Deform", {"deform.driveChannel", "window.drivenChannels", "#Deformers", "deform.lattice"});
    addSubmenu("Deform", "Nonlinear");
    menus::addActions("Deform/Nonlinear", {"deform.bend", "deform.twist", "deform.taper", "deform.squash", "deform.sine", "deform.wave"});
    menus::addActions("Deform", {"deform.softMod", "deform.deltaMush", "deform.wrap", "deform.cage", "curve.deformAlongCurve", "curve.flowAlongSurface"});
    addSubmenu("Deform", "Cage Coordinates");
    menus::addActions("Deform/Cage Coordinates", {"deform.cageGreen", "deform.cageSomigliana"});
    addSubmenu("Deform", "Handles");
    menus::addActions("Deform/Handles", {"deform.kelvinlet", "", "deform.arap", "deform.laplacian", "deform.addHandle", "deform.setAnchors", "",
                                         "deform.bbw", "deform.bbwSkinWeights"});
    addSubmenu("Deform", "Shape");
    menus::addActions("Deform/Shape", {"deform.shear", "deform.inflate", "deform.cast", "deform.smooth", "deform.warp", "deform.noise", "deform.textureDeformer", "",
                                       "deform.tension"});
    addSubmenu("Deform", "Surface");
    menus::addActions("Deform/Surface", {"deform.shrinkwrap", "deform.surfaceDeform", "deform.addWrapDriver", "", "deform.collision", "deform.addCollider", "",
                                         "deform.rigidify", "deform.skinSlide", "", "deform.uvPin", "deform.proximityPin"});
    menus::addActions("Deform", {"deform.correction", "deform.editCorrection"});
    addSubmenu("Deform", "Fields");
    menus::addActions("Deform/Fields", {"deform.field", "deform.addFieldLayer", "deform.connectField", "deform.disconnectField", "", "deform.softSelectField"});
    addSubmenu("Deform", "Weights");
    menus::addActions("Deform/Weights", {"deform.paintWeights", "deform.floodWeights", "deform.smoothWeights", "deform.mirrorWeights", "deform.copyWeights", "",
                                         "deform.exportWeights", "deform.importWeights", "", "deform.setMembership", "deform.clearMembership"});
    addSubmenu("Deform", "Display");
    menus::addActions("Deform/Display", {"deform.heatmap", "deform.timing"});
    menus::addActions("Deform", {"", "deform.rebind", "#Secondary Motion", "deform.jiggle"});
    addSubmenu("Deform", "Bake for Unreal");
    menus::addActions("Deform/Bake for Unreal", {"deform.bakeSkinWeights", "deform.bakeHelperJoints", "deform.bakeCorrectiveMorphs", "",
                                                 "deform.rangeOfMotion", "deform.exportMLDeformer", "", "deform.bakeTension", "deform.bakeSurfacePin"});

    menu("Display", "Display", Area::MenuBar, syncChecks);
    menus::addActions("Display", {"#Shading", "viewport.wireframe", "viewport.shaded", "viewport.textured", "viewport.wireOnShaded", "viewport.defaultMaterial",
                           "viewport.toggleXray", "viewport.toggleBackfaceCulling", "#Lighting and Effects"});
    addSubmenu("Display", "Lighting", syncChecks);
    menus::addActions("Display/Lighting", {"viewport.lighting.environment", "viewport.lighting.studio", "viewport.lighting.flat"});
    addSubmenu("Display", "Vertex Colors", syncChecks);
    menus::addActions("Display/Vertex Colors", {"viewport.vertexColors.off", "viewport.vertexColors.shaded", "viewport.vertexColors.unlit", "",
                                         "viewport.colorChannel.rgb", "viewport.colorChannel.r", "viewport.colorChannel.g",
                                         "viewport.colorChannel.b", "viewport.colorChannel.a", "", "viewport.grayscaleWeights",
                                         "viewport.monotone"});
    menus::addActions("Display", {"viewport.toggleShadows", "viewport.toggleAO", "viewport.toggleAA", "viewport.toggleKuwahara",
                                  "viewport.toggleOutline"});
    addSubmenu("Display", "Antialiasing Samples", syncChecks);
    menus::addActions("Display/Antialiasing Samples", {"viewport.aaSamples2", "viewport.aaSamples4", "viewport.aaSamples8"});
    addSubmenu("Display", "Environment", [](QMenu* env) { connect(env, &QMenu::aboutToShow, env, [env] { fillEnvironmentMenu(env); }); });
    menus::addActions("Display", {"viewport.toggleEnvBackground", "#Visibility", "display.toggleVisibility", "display.hide", "display.show", "display.showLastHidden", "display.showAll",
                           "viewport.isolate", "viewport.isolateAdd", "viewport.isolateRemove"});
    addSubmenu("Display", "Level of Detail", syncChecks);
    menus::addActions("Display/Level of Detail", {"viewport.lod.screenSize", "viewport.lod.all", "", "viewport.lod.level0", "viewport.lod.level1",
                                                  "viewport.lod.level2", "viewport.lod.level3", "viewport.lod.level4", "viewport.lod.level5",
                                                  "viewport.lod.level6", "viewport.lod.level7"});
    addSubmenu("Display", "Mesh Display", syncChecks);
    for (const displayAids::Info& i : displayAids::all())
        addItem({.menu = "Display/Mesh Display", .kind = Kind::Action, .action = std::string("viewport.aid.") + i.id});
    addItem({.id = "Display/Mesh Display/normalLength", .menu = "Display/Mesh Display", .kind = Kind::Custom,
             .setup = [](QMenu* m) { m->addAction(displayAids::normalLengthRow(m)); }});
    addSubmenu("Display", "Measure", syncChecks);
    menus::addActions("Display/Measure", {"measure.selection", "measure.copyDistance", "measure.copyDiameter", "measure.copyLength", "",
                                   "measure.distanceTool", "measure.angleTool", "measure.radiusTool", "", "measure.distanceFromSelection",
                                   "measure.angleFromSelection", "measure.radiusFromSelection", "", "measure.toggleShow"});
    menus::addActions("Display", {"#Layers"});
    addSubmenu("Display", "Display Layers");
    menus::addActions("Display/Display Layers", {"window.displayLayers", "layers.create", "layers.createFromSelected", "layers.addSelected",
                                          "layers.removeSelected"});
    menus::addActions("Display", {"#Camera"});
    addSubmenu("Display", "Views", syncChecks);
    menus::addActions("Display/Views", {"viewport.view.persp", "viewport.view.top", "viewport.view.front", "viewport.view.right", "viewport.view.left",
                                 "viewport.view.back", "viewport.view.bottom"});
    menus::addActions("Display", {"viewport.frameSelected", "viewport.frameAll", "viewport.cameraUndo", "viewport.cameraRedo", "#Overlays",
                           "viewport.toggleGrid", "viewport.toggleResolutionGate", "viewport.toggleFilmGate", "viewport.cameraSettings",
                           "window.renderSettings", "#View", "viewport.toggleToolbar", "viewport.toggleBottomBar", "window.toggleToolBox", "viewport.resetExposure"});

    menu("Animation", "Animation");
    menus::addActions("Animation", {"#Keys", "anim.setKey", "anim.setKeyTranslate", "anim.setKeyRotate", "anim.setKeyScale", "anim.autoKeyToggle", "#Playback",
                             "anim.play", "anim.playBackward", "anim.stop", "anim.firstFrame", "anim.lastFrame", "anim.nextFrame",
                             "anim.prevFrame", "anim.nextKey", "anim.prevKey", "#Editors", "window.graphEditor", "window.timeSlider"});
    menus::addActions("Animation", {"#Ghosting and Poses"});
    addGhostingMenu("Animation");
    menus::addActions("Animation", {"window.poseLibrary", "pose.mirror", "window.controlPicker", "picker.editMode"});
    menus::addActions("Animation", {"#Offsets", "anim.createOffset", "anim.bakeOffset", "anim.deleteOffset", "window.offsets"});

    menu("Editors", "Editors", Area::MenuBar, syncChecks);
    addItem({.id = "Editors/panels", .menu = "Editors", .kind = Kind::Custom, .setup = [this](QMenu* m) {
                 // Filled on open: extensions add and remove panels at any time.
                 connect(m, &QMenu::aboutToShow, m, [this, m] { fillEditorsMenu(m); });
                 fillEditorsMenu(m);
             }});

    menu("Windows", "Windows", Area::MenuBar, syncChecks);
    addItem({.id = "Windows/layouts", .menu = "Windows", .kind = Kind::Custom, .setup = [this](QMenu* m) { m->addMenu(mLayouts->menu()); }});
    menus::addActions("Windows", {"window.resetLayout", "window.expandPanels", "window.toggleMaximizePanel", "#Show", "window.toggleToolBox",
                           "window.toggleChannelBox", "window.toggleUvEditor", "window.commandLine", "#Customize", "window.hotkeyEditor",
                           "window.markingMenuEditor", "window.extensionManager"});

    menu("Help", "Help");
    menus::addActions("Help", {"help.docs", "help.updateDocs", "", "window.searchMenus", "window.searchScene", "", "help.agentSetup", "help.prefsFolder", "help.crashReports", "help.copyLastCrash", "help.hotkeyCheatSheet", "help.about"});

    buildPanelMenus();

    mMenus = new MenuBuilder(this, [this](QMenu* m, bool topLevel) {
        if (topLevel)
            tearoff::enable(m);
        refreshKeysOnShow(m);
    });
    connect(mMenus, &MenuBuilder::rebuilt, this, [this] {
        menuBar()->clear();
        for (QMenu* m : mMenus->menus(Area::MenuBar))
            menuBar()->addMenu(m);
        // The layout switcher lives in the menu bar corner, so the hotbox carries it too for when the menu bar is hidden.
        std::vector<QMenu*> panels = mMenus->menus(Area::Hotbox);
        panels.push_back(mLayouts->menu());
        if (Hotbox* hb = Hotbox::instance())
            hb->setPanelMenus(std::move(panels));
        else
            Hotbox::install(this, std::move(panels));
    });
    mMenus->rebuild();
}

void MainWindow::fillEditorsMenu(QMenu* menu)
{
    // Panels by what they are for; a panel not named here (most extension panels) goes under Other.
    static const std::vector<std::pair<const char*, std::vector<const char*>>> kGroups = {
        {"Scene", {"Outliner", "Attribute Editor", "Channel Box", "Display Layers", "Quick Select Sets", "Rename", "Reference Editor",
                   "Source Control", "Undo History", "Shelf", "Docs"}},
        {"Modeling", {"Modeling Toolkit", "Topology Toolkit", "Modifier Stack", "Node Editor", "Morph Targets", "Driven Channels",
                      "Foliage Blades", "Tree Generator", "Ground Tiles", "Effects"}},
        {"UVs and Texturing", {"UV Editor", "Material Editor", "Material Browser", "Texture Set", "Texture Graph", "Texture", "2D View", "Baker"}},
        {"Animation and Rigging", {"Graph Editor", "Time Slider", "Anim Layers", "Pose Library", "Control Picker", "Offsets", "Rig Templates",
                                   "Puppeteer", "Anim Bank", "Stacker", "Mixer", "Cycle Maker", "Overlap", "Physics Animation"}},
        {"Set Dressing", {"Dress", "Prop Library"}},
        {"Scripting", {"Script Editor", "Script Output", "API Reference", "Terminal", "Batch Script Execution"}},
    };
    menu->clear();
    const QList<QDockWidget*> found = findChildren<QDockWidget*>();
    std::vector<QDockWidget*> docks(found.begin(), found.end());
    std::sort(docks.begin(), docks.end(), [](QDockWidget* a, QDockWidget* b) {
        return QString::localeAwareCompare(a->windowTitle(), b->windowTitle()) < 0;
    });
    std::vector<QDockWidget*> placed;
    auto section = [&](const QString& title, const std::vector<QDockWidget*>& members) {
        if (members.empty())
            return;
        menu->addSection(title);
        for (QDockWidget* d : members)
            menu->addAction(d->toggleViewAction());
    };
    for (const auto& [group, titles] : kGroups)
    {
        std::vector<QDockWidget*> members;
        for (QDockWidget* d : docks)
            if (std::find(titles.begin(), titles.end(), d->windowTitle().trimmed().toStdString()) != titles.end())
                members.push_back(d);
        placed.insert(placed.end(), members.begin(), members.end());
        section(group, members);
    }
    std::vector<QDockWidget*> rest;
    for (QDockWidget* d : docks)
        if (std::find(placed.begin(), placed.end(), d) == placed.end() && !d->windowTitle().trimmed().isEmpty())
            rest.push_back(d);
    section("Other", rest);
}

void MainWindow::buildPanelMenus()
{
    // Maya's viewport panel menus. Lila's views have no menu bar of their own, so these live in the hotbox.
    using namespace menus;
    for (const char* title : {"View", "Shading", "Show", "Renderer", "Panels"})
        addMenu({.id = std::string("panel.") + title, .title = title, .area = Area::Hotbox});
    menus::addActions("panel.View", {"viewport.view.persp", "viewport.view.top", "viewport.view.front", "viewport.view.right", "viewport.view.left",
                              "viewport.view.back", "viewport.view.bottom", "", "viewport.frameSelected", "viewport.frameAll",
                              "viewport.cameraUndo", "viewport.cameraRedo", "", "viewport.toggleResolutionGate", "viewport.toggleFilmGate",
                              "viewport.cameraSettings", "", "viewport.imagePlaneImport"});
    menus::addActions("panel.Shading", {"viewport.wireframe", "viewport.shaded", "viewport.textured", "viewport.wireOnShaded", "", "viewport.toggleAO",
                                 "viewport.aid.twoSidedLighting", "viewport.aid.backfaceTint",
                                 "", "display.smoothOff", "display.smoothCage", "display.smoothOn", "viewport.smoothSubdivisions"});
    menus::addActions("panel.Show", {"display.hide", "display.show", "display.showLastHidden", "display.showAll", "", "viewport.isolate", "viewport.isolateAdd",
                              "viewport.isolateRemove", "viewport.show.curve", "viewport.show.mesh", "viewport.show.joint", "viewport.show.light",
                              "viewport.show.locator", "rig.xrayJoints", "measure.toggleShow",
                              "viewport.toggleImagePlanes", "", "viewport.toggleDebugDraw", "viewport.clearDebugDraw"});
    menus::addActions("panel.Renderer", {"window.renderSettings"});
    menus::addActions("panel.Panels", {"viewport.lookThroughSelected"});
    // Maya's Panels > Perspective / Orthographic: the views plus every camera node, rebuilt as the menu opens.
    addSubmenu("panel.Panels", "Cameras", [this](QMenu* cameras) {
        connect(cameras, &QMenu::aboutToShow, cameras, [this, cameras] { fillCameraMenu(cameras, mViewports->active()); });
    });
    menus::addActions("panel.Panels", {"", "viewport.toggleQuad", "window.toggleMaximizePanel", "", "window.uvEditor", "window.graphEditor",
                                "window.nodeEditor", "window.materialEditor"});
}

void MainWindow::buildStatusBar()
{
    auto* help = new QLabel(this);
    help->setMinimumWidth(1);
    statusBar()->addWidget(help, 1);
    qApp->installEventFilter(new HelpLine(help, this));
    menuSearch::install();
    // Held-key states take the help line while they last, then hand back whatever it said before.
    cursorBadges::setStatusSink([help, saved = std::make_shared<std::optional<QString>>()](const QString& text) {
        if (!text.isEmpty() && !*saved)
            *saved = help->text();
        if (!text.isEmpty())
            help->setText(text);
        else if (*saved)
        {
            help->setText(**saved);
            saved->reset();
        }
    });
    if (QDockWidget* output = findChild<QDockWidget*>("scriptOutput"))
    {
        auto* line = new StatusOutput([output] { revealDock(output); },
                                      [output] { return dockShowing(output); }, this);
        statusBar()->addWidget(line, 1);
        connect(output, &QDockWidget::visibilityChanged, line, qOverload<>(&QWidget::update));
    }
    installSpinScrub(this);
    installWheelGuard(this);

    mWorkspaceLabel = new QLabel(this);
    mWorkspaceLabel->setTextFormat(Qt::RichText);
    mWorkspaceLabel->setContentsMargins(0, 0, 10, 0);
    // The layouts corner is installed later, so the label joins it once the window is built.
    QTimer::singleShot(0, this, [this] { placeWorkspaceLabel(); });
    refreshWorkspaceLabel();

    mSourceLabel = new QLabel(this);
    mSourceLabel->setStyleSheet("color: #9a9a9a;");
    statusBar()->addPermanentWidget(mSourceLabel);
    statusBar()->addPermanentWidget(new QLabel(QString::fromStdString(render::Renderer::get().deviceName()), this));

    statusBar()->addPermanentWidget(modes::createSwitcher(this));

    mMcpButton = new QToolButton(this);
    mMcpButton->setText("MCP");
    mMcpButton->setCheckable(true);
    mMcpButton->setChecked(mcp::Server::get().running());
    mMcpButton->setToolTip("Let AI tools drive Lila over MCP (localhost only). Connection details: <prefs>/mcp.json");
    connect(mMcpButton, &QToolButton::toggled, this, [this](bool on) {
        PrefStore& p = PrefStore::get();
        p.set("general", "/mcp/enabled", on);
        if (on)
            mcp::Server::get().start(p.value<int>("general", "/mcp/port", 8740), p.value<std::string>("general", "/mcp/token", ""));
        else
            mcp::Server::get().stop();
        QSignalBlocker block(mMcpButton);
        mMcpButton->setChecked(mcp::Server::get().running());
    });
    statusBar()->addPermanentWidget(mMcpButton);

    auto* prefs = new QToolButton(this);
    prefs->setIcon(icon("settings"));
    prefs->setToolTip("Preferences");
    prefs->setAutoRaise(true);
    connect(prefs, &QToolButton::clicked, this, [this] { PreferencesDialog::open(this); });
    statusBar()->addPermanentWidget(prefs);
    progressui::install(this);
}

void MainWindow::placeWorkspaceLabel()
{
    QWidget* corner = menuBar()->cornerWidget(Qt::TopRightCorner);
    if (corner == mWorkspaceLabel || mWorkspaceLabel->parentWidget() == corner)
        return;
    if (auto* row = corner ? qobject_cast<QBoxLayout*>(corner->layout()) : nullptr)
        row->insertWidget(0, mWorkspaceLabel);
    else
        menuBar()->setCornerWidget(mWorkspaceLabel, Qt::TopRightCorner);
    mWorkspaceLabel->show();
}

void MainWindow::refreshWorkspaceLabel()
{
    auto& ws = Workspace::get();
    if (ws.isOpen())
    {
        std::filesystem::path root = ws.root();
        QString name = QString::fromStdWString(root.filename().wstring());
        QString parent = root.has_parent_path() && root.parent_path() != root.root_path()
                             ? QString::fromStdWString(root.parent_path().filename().wstring()) + "\\"
                             : QString();
        QString dim = palette().color(QPalette::Disabled, QPalette::WindowText).name();
        mWorkspaceLabel->setText(QString("<span style=\"color:%1\">%2</span>%3").arg(dim, parent.toHtmlEscaped(), name.toHtmlEscaped()));
        QString full = QDir::toNativeSeparators(QString::fromStdWString(root.wstring()));
        mWorkspaceLabel->setToolTip("Workspace: " + full);
        mWorkspaceLabel->setStatusTip(full);
    }
    else
    {
        mWorkspaceLabel->setText("No folder open");
        mWorkspaceLabel->setToolTip("No workspace folder open. File > Open Folder sets one.");
        mWorkspaceLabel->setStatusTip(QString());
    }
    refreshTitle();
}

void MainWindow::refreshRecentFolders()
{
    if (!mRecentMenu)
        return;
    mRecentMenu->clear();
    json recent = PrefStore::get().value("general", "/recentFolders", json::array());
    if (!recent.is_array() || recent.empty())
    {
        mRecentMenu->addAction("(none)")->setEnabled(false);
        return;
    }
    for (const json& r : recent)
    {
        if (!r.is_string())
            continue;
        QString path = QString::fromStdString(r.get<std::string>());
        mRecentMenu->addAction(path, [path] { Workspace::get().open(path.toStdWString()); });
    }
}

void MainWindow::openFolder()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString dir = QFileDialog::getExistingDirectory(this, "Open Folder", start);
    if (!dir.isEmpty())
        Workspace::get().open(dir.toStdWString());
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, "About Lila",
                       "<h2>Lila</h2>"
                       "<p><i>Lila (&#2354;&#2368;&#2354;&#2366;, LEE-laah): Sanskrit for divine play, or creation.</i></p>"
                       "<p>Version 0.1.0</p>"
                       "<p>Copyright &copy; 2026 Jared Taylor. All rights reserved.</p>");
}

void MainWindow::restoreLayout()
{
    mRestoring = true;
    mDefaultState = saveState();
    PrefStore& p = PrefStore::get();
    std::string geometry = p.value<std::string>(kLayoutFile, "/geometry", "");
    std::string state = p.value<int>(kLayoutFile, "/version", 1) >= kLayoutVersion ? p.value<std::string>(kLayoutFile, "/state", "") : "";
    if (!geometry.empty())
        restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
    if (!state.empty())
    {
        std::vector<std::string> tearOffs;
        for (const json& name : p.value(kLayoutFile, "/tearOffs", json::array()))
            if (name.is_string())
                tearOffs.push_back(name.get<std::string>());
        tearoff::recreate(this, tearOffs);
        restoreState(QByteArray::fromBase64(QByteArray::fromStdString(state)));
        toolBox::place(this);
        QStringList collapsed;
        for (auto& name : p.value(kLayoutFile, "/collapsed", json::array()))
            if (name.is_string())
                collapsed << QString::fromStdString(name.get<std::string>());
        mSidebars->restoreCollapsed(collapsed);
        std::map<QString, int> tabSizes;
        json savedSizes = p.value(kLayoutFile, "/tabSizes", json::object());
        for (auto& [name, size] : savedSizes.items())
            if (size.is_number_integer())
                tabSizes[QString::fromStdString(name)] = size.get<int>();
        mSidebars->setTabSizes(std::move(tabSizes));
        for (const json& entry : p.value(kLayoutFile, "/maximizeHidden", json::array()))
            if (auto* d = entry.is_object() ? findChild<QDockWidget*>(QString::fromStdString(entry.value("name", ""))) : nullptr; d && d->isHidden())
                mMaximizeHidden.emplace_back(d, entry.value("showing", false));
    }
    else
        QTimer::singleShot(0, this, [this] {
            applyDefaultDockSizes();
            mSidebars->restoreCollapsed(kDefaultCollapsed);
        });
    QTimer::singleShot(0, this, &MainWindow::fitToScreen);
    mRestoring = false;
}

void MainWindow::applyDefaultDockSizes()
{
    if (auto* channels = findChild<QDockWidget*>("channelBox"))
        resizeDocks({channels}, {kDefaultSidePanelWidth}, Qt::Horizontal);
    auto* outliner = findChild<QDockWidget*>("outliner");
    auto* undoHistory = findChild<QDockWidget*>(kUndoHistoryDock);
    auto* quickSets = findChild<QDockWidget*>(kQuickSelectSetsDock);
    if (outliner && undoHistory && quickSets && outliner->isVisible() && undoHistory->isVisible() && quickSets->isVisible())
        resizeDocks({outliner, quickSets, undoHistory}, {height() / 2, height() / 5, height() * 3 / 10}, Qt::Vertical);
    else if (outliner && undoHistory && outliner->isVisible() && undoHistory->isVisible())
        resizeDocks({outliner, undoHistory}, {height() * 2 / 3, height() / 3}, Qt::Vertical);
    auto* channels = findChild<QDockWidget*>("channelBox");
    auto* layers = findChild<QDockWidget*>(kDisplayLayersDock);
    if (channels && layers && channels->isVisible() && layers->isVisible())
        resizeDocks({channels, layers}, {height() * 3 / 4, height() / 4}, Qt::Vertical);
}

void MainWindow::applyChromePrefs()
{
    PrefStore& p = PrefStore::get();
    menuBar()->setVisible(p.value<bool>("general", "/ui/showMenuBar", true));
    QDockWidget* shelf = findChild<QDockWidget*>("shelf");
    bool showShelf = p.value<bool>("general", "/ui/showShelf", true);
    if (!shelf)
        return;
    shelf->toggleViewAction()->setEnabled(showShelf);
    shelf->toggleViewAction()->setToolTip(showShelf ? QString() : "Hidden by Preferences > Interface > Show shelf. ` still opens the floating shelf.");
    if (showShelf == mShelfShown)
        return;
    mShelfShown = showShelf;
    if (showShelf)
        shelf->show();
    else
        mSidebars->close(shelf);
}

void MainWindow::createCameraFromView()
{
    const ViewportWidget* view = mViewports->active();
    const Camera& c = view->camera();
    const scene::Node* through = scene::Graph::current().node(view->lookThroughCamera());
    auto vec = [](const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); };
    try
    {
        UndoChunk chunk("Create Camera from View");
        std::string name = ops::execute("createNode", {{"args", {"camera"}}}, CallSource::UI).get<std::string>();
        auto set = [&](const char* attr, json value) { ops::execute("setAttr", {{"args", {name + "." + attr, std::move(value)}}}, CallSource::UI); };
        if (through)
        {
            for (int attr = scene::Camera::kFocalLength; attr < scene::Camera::kCount; ++attr)
                if (attr != scene::Camera::kLocked)
                    set(through->type().attrs[size_t(attr)].name.c_str(), scene::toJson(through->value(attr)));
        }
        else
        {
            set("focalLength", c.focalLength);
            set("horizontalFilmAperture", c.horizontalAperture / 25.4);
            set("verticalFilmAperture", c.verticalAperture / 25.4);
            set("filmFit", int(c.filmFit));
            set("nearClipPlane", c.nearClip);
            if (c.ortho)
            {
                set("orthographic", true);
                set("orthographicWidth", c.orthoWidth);
            }
        }
        // Orthographic views sit far out along their axis; the camera only needs to be clear of the scene.
        glm::dvec3 eye = c.ortho && !through ? c.target - c.forward() * 1000.0 : c.eye();
        ops::execute("viewPlace", {{"args", {name}}, {"eye", vec(eye)}, {"lookAt", vec(c.target)}, {"up", vec(c.viewUp())}},
                      CallSource::UI);
        ops::execute("selectMode", {{"args", {"object"}}}, CallSource::UI);
        ops::execute("select", {{"args", {name}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void MainWindow::renameSelection()
{
    json selection = ops::execute("ls", {{"selection", true}});
    if (!selection.is_array() || selection.empty())
        return;
    std::vector<std::string> names;
    for (const json& n : selection)
        names.push_back(n.get<std::string>());

    auto* edit = new QLineEdit(this);
    edit->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    edit->setAttribute(Qt::WA_DeleteOnClose);
    edit->setText(QString::fromStdString(names.front()));
    edit->selectAll();
    edit->setToolTip(names.size() > 1 ? "Renames every selected object; Lila numbers duplicates" : "Enter to rename, Esc to cancel");
    edit->setMinimumWidth(220);
    edit->move(QCursor::pos() - QPoint(20, edit->sizeHint().height() / 2));
    edit->installEventFilter(new EscapeCloses(edit, "edit.rename"));
    connect(edit, &QLineEdit::returnPressed, this, [this, edit, names] {
        std::string to = edit->text().trimmed().toStdString();
        edit->close();
        if (to.empty() || to == names.front())
            return;
        UndoChunk chunk("Rename");
        for (const std::string& from : names)
            run("rename", {{"args", {from, to}}});
    });
    edit->show();
    edit->setFocus();
}

void MainWindow::resetLayout()
{
    mMaximizeHidden.clear();
    mSidebars->expandAll();
    restoreState(mDefaultState);
    toolBox::place(this);
    for (QDockWidget* d : findChildren<QDockWidget*>())
        if (tearoff::parseDockName(d->objectName().toStdString()))
            d->hide();
    applyDefaultDockSizes();
    mSidebars->restoreCollapsed(kDefaultCollapsed);
    fitToScreen();
    scheduleLayoutSave();
}

void MainWindow::fitToScreen()
{
    QRect avail = screen()->availableGeometry();
    if (geometry().width() <= avail.width() && geometry().height() <= avail.height())
        return;
    if (isMaximized())
    {
        // Re-maximizing is what makes Windows drop the oversized frame; resizing a maximized window does nothing.
        showNormal();
        showMaximized();
        return;
    }
    QMargins frame(geometry().left() - frameGeometry().left(), geometry().top() - frameGeometry().top(),
                   frameGeometry().right() - geometry().right(), frameGeometry().bottom() - geometry().bottom());
    setGeometry(avail.marginsRemoved(frame));
}

void MainWindow::scheduleLayoutSave()
{
    if (!mRestoring && mLayoutTimer)
        mLayoutTimer->start();
}

void MainWindow::saveLayout()
{
    PrefStore& p = PrefStore::get();
    p.set(kLayoutFile, "/geometry", saveGeometry().toBase64().toStdString());
    p.set(kLayoutFile, "/state", saveState().toBase64().toStdString());
    json collapsed = json::array();
    for (const QString& name : mSidebars->collapsedNames())
        collapsed.push_back(name.toStdString());
    p.set(kLayoutFile, "/collapsed", collapsed);
    json tabSizes = json::object();
    for (auto& [name, size] : mSidebars->tabSizes())
        tabSizes[name.toStdString()] = size;
    p.set(kLayoutFile, "/tabSizes", tabSizes);
    json tearOffs = json::array();
    for (QDockWidget* d : findChildren<QDockWidget*>())
        if (tearoff::parseDockName(d->objectName().toStdString()) && (d->toggleViewAction()->isChecked() || mSidebars->isCollapsed(d)))
            tearOffs.push_back(d->objectName().toStdString());
    p.set(kLayoutFile, "/tearOffs", tearOffs);
    json maximizeHidden = json::array();
    for (auto& [d, showing] : mMaximizeHidden)
        if (d)
            maximizeHidden.push_back({{"name", d->objectName().toStdString()}, {"showing", showing}});
    p.set(kLayoutFile, "/maximizeHidden", maximizeHidden);
    p.set(kLayoutFile, "/version", kLayoutVersion);
}

void MainWindow::resizeEvent(QResizeEvent* e)
{
    QMainWindow::resizeEvent(e);
    scheduleLayoutSave();
}

void MainWindow::moveEvent(QMoveEvent* e)
{
    QMainWindow::moveEvent(e);
    scheduleLayoutSave();
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (!confirmDiscard())
    {
        e->ignore();
        return;
    }
    saveLayout();
    QMainWindow::closeEvent(e);
}
} // namespace lila::ui
