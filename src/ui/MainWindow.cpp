#include "ui/MainWindowDetail.h"

namespace lila::ui
{
using namespace mainWindowDetail;

MainWindow::MainWindow()
{
    setWindowTitle(QString("Lila ") + version::number());
    resize(1600, 900);
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

    panels::add({.id = "shelf", .title = "Shelf", .create = make([](QWidget* p) { return new ShelfPanel(p); }), .area = Qt::TopDockWidgetArea, .icon = "apps",
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
                 .area = Qt::LeftDockWidgetArea, .icon = "list-tree", .docs = "essentials?id=filter-the-outliner"});
    panels::add({.id = "sourceControl", .title = "Source Control", .create = make([](QWidget* p) { return new SourceControlPanel(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "git-branch"});
    panels::add({.id = kReferenceEditorDock, .title = "Reference Editor", .create = make([](QWidget* p) { return new ReferenceEditor(p); }),
                 .area = Qt::LeftDockWidgetArea, .icon = "file-symlink"});
    panels::add({.id = kDisplayLayersDock, .title = "Display Layers", .create = make([](QWidget* p) { return new DisplayLayersPanel(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "stack-2"});
    registerAnimLayersPanel();
    registerOffsetsPanel();
    registerNotesPanel();
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
                 .area = Qt::BottomDockWidgetArea, .icon = "file-text"});
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
                 .area = Qt::BottomDockWidgetArea, .icon = "code", .docs = "python/"});
    panels::add({.id = kApiReferenceDock, .title = "API Reference", .create = make([](QWidget* p) { return new ApiReference(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "api", .open = false, .place = [this, dock](QDockWidget* apiDock) {
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
                 .floatFirst = true, .docs = "python/"});
    panels::add({.id = kTerminalDock, .title = "Terminal", .create = make([](QWidget* p) { return new TerminalPanel(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "terminal-2"});
    // The UV editor opens as its own column left of the channel box.
    panels::add({.id = kUvEditorDock, .title = "UV Editor", .create = make([](QWidget* p) { return new UvEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "cube-unfolded"});
    panels::add({.id = kChannelBoxDock, .title = "Channel Box", .create = make([](QWidget* p) { return new ChannelBox(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "list", .place = [this, dock](QDockWidget* channels) {
                     // Back to its own width when its tab is picked after a wider one, such as the modeling toolkit.
                     channels->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth);
                     splitDockWidget(dock(kUvEditorDock), channels, Qt::Horizontal);
                     // Split before the tabs join, so the layer editor sits under the whole channel box tab group as in Maya.
                     splitDockWidget(channels, dock(kDisplayLayersDock), Qt::Vertical);
                 }});
    panels::add({.id = "attributeEditor", .title = "Attribute Editor", .create = make([](QWidget* p) { return new AttributeEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "file-description", .place = [this, dock](QDockWidget* d) { tabifyDockWidget(dock(kChannelBoxDock), d); }});
    auto tabWithAttributes = [this, dock](QDockWidget* d) { tabifyDockWidget(dock("attributeEditor"), d); };
    panels::add({.id = kModelingToolkitDock, .title = "Modeling Toolkit", .create = make([](QWidget* p) { return new ModelingToolkit(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "tool", .open = false, .place = [tabWithAttributes](QDockWidget* toolkit) {
                     tabWithAttributes(toolkit);
                     toolkit->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 5 / 4);
                 }});
    panels::add({.id = kCurveToolkitDock, .title = "Curve Toolkit", .create = make([](QWidget* p) { return new CurveToolkit(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "vector-bezier-2", .open = false, .place = [tabWithAttributes](QDockWidget* toolkit) {
                     tabWithAttributes(toolkit);
                     toolkit->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 5 / 4);
                 }});
    panels::add({.id = kTopologyToolkitDock, .title = "Topology Toolkit", .create = make([](QWidget* p) { return new TopologyToolkit(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "grid-dots", .open = false, .place = [tabWithAttributes](QDockWidget* toolkit) {
                     tabWithAttributes(toolkit);
                     toolkit->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 2);
                 }, .docs = "topology"});
    panels::add({.id = kLevelToolkitDock, .title = "Level Toolkit", .create = make([](QWidget* p) { return new LevelToolkit(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "map", .open = false, .place = [tabWithAttributes](QDockWidget* toolkit) {
                     tabWithAttributes(toolkit);
                     toolkit->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 2);
                 }, .toggleAction = "window.levelToolkit"});
    panels::add({.id = kModifierStackDock, .title = "Modifier Stack", .create = make([](QWidget* p) { return new ModifierStackWidget(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "stack-front", .place = tabWithAttributes});
    panels::add({.id = kMaterialEditorDock, .title = "Material Editor", .create = make([](QWidget* p) { return new MaterialEditor(p); }),
                 .area = Qt::RightDockWidgetArea, .icon = "color-swatch", .place = [tabWithAttributes, dock](QDockWidget* materials) {
                     tabWithAttributes(materials);
                     // Swatches and texture slots need more room than the channel box's width.
                     materials->setProperty(kDefaultTabExtent, kDefaultSidePanelWidth * 5 / 4);
                     dock(kChannelBoxDock)->raise();
                 }});
    panels::add({.id = kTimeSliderDock, .title = "Time Slider", .create = make([](QWidget* p) { return new TimeSlider(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "timeline", .place = [](QDockWidget* timeSlider) {
                     timeSlider->setTitleBarWidget(new QWidget(timeSlider));
                     // Fixed height, so a bottom area left with only the slider shrinks to it instead of handing it the space.
                     timeSlider->widget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
                 }});
    panels::add({.id = kGraphEditorDock, .title = "Graph Editor", .create = make([](QWidget* p) { return new GraphEditor(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "chart-line", .open = false});
    panels::add({.id = kNodeEditorDock, .title = "Node Editor", .create = make([](QWidget* p) { return new NodeEditor(p); }),
                 .area = Qt::BottomDockWidgetArea, .icon = "schema", .open = false, .place = [this, dock](QDockWidget* nodeDock) {
                     arrangeBottomDocks(this, {dock("scriptOutput"), dock(kScriptEditorDock), dock(kTerminalDock), dock(kGraphEditorDock), nodeDock},
                                        dock(kScriptEditorDock), dock(kTimeSliderDock));
                 }, .docs = "node-editor"});
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
    if (!p.docs.empty())
        setDockDocs(d, p.docs);
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

} // namespace lila::ui
