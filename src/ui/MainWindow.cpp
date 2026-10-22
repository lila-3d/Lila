#include "ui/MainWindow.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "io/SceneFile.h"
#include "io/SceneHistory.h"
#include "mcp/McpServer.h"
#include "render/Renderer.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/RenderSettings.h"
#include "ui/Actions.h"
#include "ui/Hotbox.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/ImagePlaneUi.h"
#include "ui/Layouts.h"
#include "ui/LayoutsUi.h"
#include "ui/Animation.h"
#include "ui/ApiReference.h"
#include "ui/GraphEditor.h"
#include "ui/SpinScrub.h"
#include "ui/TimeSlider.h"
#include "ui/AttributeEditor.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/DockSidebar.h"
#include "ui/Environments.h"
#include "ui/ChannelBox.h"
#include "ui/CursorBadges.h"
#include "ui/Outliner.h"
#include "ui/MaterialEditor.h"
#include "ui/ModelingToolkit.h"
#include "ui/ModifierStack.h"
#include "ui/OptionBox.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/UvEditor.h"
#include "ui/PreferencesDialog.h"
#include "ui/ProgressOverlay.h"
#include "ui/ReferenceEditor.h"
#include "ui/RenderSettingsDialog.h"
#include "ui/Shelf.h"
#include "ui/StatusOutput.h"
#include "ui/SceneDialogs.h"
#include "ui/SourceControlPanel.h"
#include "ui/TearOff.h"
#include "ui/TearOffLogic.h"
#include "vcs/SourceControl.h"
#include "ui/Terminal.h"
#include "ui/Theme.h"
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
#include <QDesktopServices>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QStatusBar>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <optional>

#ifdef _WIN32
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
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

    mViewports = new ViewportLayout(this);
    setCentralWidget(mViewports);

    QDockWidget* shelf = addDock("Shelf", "shelf", new ShelfPanel(this), Qt::TopDockWidgetArea);
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
    QDockWidget* outliner = addDock("Outliner", "outliner", new OutlinerPanel(this), Qt::LeftDockWidgetArea);
    addDock("Source Control", "sourceControl", new SourceControlPanel(this), Qt::LeftDockWidgetArea);
    addDock("Reference Editor", "referenceEditor", new ReferenceEditor(this), Qt::LeftDockWidgetArea);
    QDockWidget* layers = addDock("Display Layers", kDisplayLayersDock, new DisplayLayersPanel(this), Qt::RightDockWidgetArea);
    layers->setWindowIcon(icon("stack-2"));
    QDockWidget* undoHistory = addDock("Undo History", kUndoHistoryDock, new UndoHistoryPanel(this), Qt::LeftDockWidgetArea);
    undoHistory->setWindowIcon(icon("history"));
    splitDockWidget(outliner, undoHistory, Qt::Vertical);
    QDockWidget* output = addDock("Script Output", "scriptOutput", new ScriptPanel(this), Qt::BottomDockWidgetArea);
    auto* scriptEditor = new ScriptEditor(this);
    scriptEditor->setEditorFont(monoFont());
    scriptEditor->setIconProvider([](const QString& name) { return icon(name); });
    QDockWidget* editorDock = addDock("Script Editor", kScriptEditorDock, scriptEditor, Qt::BottomDockWidgetArea);

    auto* apiRef = new ApiReference(this);
    QDockWidget* apiDock = addDock("API Reference", kApiReferenceDock, apiRef, Qt::RightDockWidgetArea);
    QDockWidget* termDock = addDock("Terminal", kTerminalDock, new TerminalPanel(this), Qt::BottomDockWidgetArea);
    apiDock->hide();
    setShowDockHook([this](const char* name) { revealDock(findChild<QDockWidget*>(name)); });
    setToggleDockHook([this](const char* name) { toggleDock(findChild<QDockWidget*>(name)); });
    tearoff::setDockHook([this](const QString& title, const QString& name, QWidget* content) {
        return addDock(title, name, content, Qt::RightDockWidgetArea);
    });
    connect(scriptEditor, &ScriptEditor::showDocs, this, [apiRef, apiDock](const QString& symbol) {
        apiDock->show();
        apiDock->raise();
        apiRef->showSymbol(symbol);
    });
    connect(apiRef, &ApiReference::insertCode, this, [scriptEditor, editorDock](const QString& code) {
        editorDock->show();
        editorDock->raise();
        CodeEditor* ed = scriptEditor->currentEditor();
        if (!ed)
            ed = scriptEditor->newTab();
        ed->insertPlainText(code);
        ed->setFocus();
    });
    // The UV editor opens as its own column left of the channel box.
    QDockWidget* uvDock = addDock("UV Editor", kUvEditorDock, new UvEditor(this), Qt::RightDockWidgetArea);
    QDockWidget* channels = addDock("Channel Box", kChannelBoxDock, new ChannelBox(this), Qt::RightDockWidgetArea);
    splitDockWidget(uvDock, channels, Qt::Horizontal);
    // Split before the tabs join, so the layer editor sits under the whole channel box tab group as in Maya.
    splitDockWidget(channels, layers, Qt::Vertical);
    QDockWidget* attrs = addDock("Attribute Editor", "attributeEditor", new AttributeEditor(this), Qt::RightDockWidgetArea);
    tabifyDockWidget(channels, attrs);
    QDockWidget* toolkit = addDock("Modeling Toolkit", kModelingToolkitDock, new ModelingToolkit(this), Qt::RightDockWidgetArea);
    tabifyDockWidget(attrs, toolkit);
    toolkit->hide();
    QDockWidget* modStack = addDock("Modifier Stack", kModifierStackDock, new ModifierStackWidget(this), Qt::RightDockWidgetArea);
    tabifyDockWidget(attrs, modStack);
    QDockWidget* materials = addDock("Material Editor", kMaterialEditorDock, new MaterialEditor(this), Qt::RightDockWidgetArea);
    tabifyDockWidget(attrs, materials);
    channels->raise();

    setUvEditorOpener([uvDock] { toggleDock(uvDock); });

    QDockWidget* timeSlider = addDock("Time Slider", kTimeSliderDock, new TimeSlider(this), Qt::BottomDockWidgetArea);
    timeSlider->setTitleBarWidget(new QWidget(timeSlider));
    // Fixed height, so a bottom area left with only the slider shrinks to it instead of handing it the space.
    timeSlider->widget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    QDockWidget* graphEditor = addDock("Graph Editor", kGraphEditorDock, new GraphEditor(this), Qt::BottomDockWidgetArea);
    graphEditor->hide();
    QDockWidget* nodeDock = addDock("Node Editor", kNodeEditorDock, new NodeEditor(this), Qt::BottomDockWidgetArea);
    nodeDock->hide();
    arrangeBottomDocks(this, {output, editorDock, termDock, graphEditor, nodeDock}, editorDock, timeSlider);
    setNodeEditorShowHook([this] { revealDock(findChild<QDockWidget*>(kNodeEditorDock)); });

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
    connect(shelf, &QDockWidget::visibilityChanged, this, [this, shelf](bool visible) {
        // Layouts and Reset Layout reopen the shelf; the pref keeps it closed.
        if (visible && !mShelfShown)
            QTimer::singleShot(0, this, [this, shelf] {
                if (!mShelfShown && shelf->isVisible())
                    mSidebars->close(shelf);
            });
    });

    io::onStateChanged([this] { refreshTitle(); });

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

namespace
{
/** Shown on screen: for a tabbed dock, its tab is the current one. */
bool showingTab(QDockWidget* d)
{
    return d && d->isVisible() && !d->visibleRegion().isEmpty();
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
        cmds::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

bool MainWindow::confirmDiscard()
{
    if (!io::modified())
        return true;
    QString name = io::displayName() == "untitled" ? QString("untitled scene") : QString::fromStdString(io::displayName());
    auto choice = QMessageBox::question(this, "Unsaved changes", "Save changes to " + name + "?",
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
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
        cmds::execute("saveScene", {{"args", {path.generic_string()}}}, CallSource::UI);
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
    if (!confirmDiscard())
        return;
    std::filesystem::path path = given;
    if (path.empty())
    {
        QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
        QString chosen = QFileDialog::getOpenFileName(this, "Open Scene", start,
                                                      "Scenes (*.lila *.lilaa *.ma *.mb);;Lila scenes (*.lila *.lilaa);;Maya scenes (*.ma *.mb)");
        if (chosen.isEmpty())
            return;
        path = chosen.toStdWString();
    }
    try
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        // A Maya file opens as a new untitled scene; saving it makes the .lila.
        if (ext == ".ma" || ext == ".mb")
            cmds::execute("mayaOpen", {{"args", {path.generic_string()}}, {"force", true}}, CallSource::UI);
        else
            cmds::execute("openScene", {{"args", {path.generic_string()}}}, CallSource::UI);
        addRecentFile(path);
        refreshSourceStatus();
        mViewports->active()->frameSelection(true);
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
                                           mViewports->active()->frameSelection(true);
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
    std::vector<io::recovery::Candidate> candidates = io::recovery::findRecoverable(stale);
    io::recovery::forget(stale);
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
                mViewports->active()->frameSelection(true);
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
    if (!showStartup || !PrefStore::get().value<bool>("general", "/startup/showDialog", true))
        return;
    auto* dialog = new StartupDialog({[this](const std::filesystem::path& p) { openScene(p); }}, this);
    dialog->present();
}

void MainWindow::importFile()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString file = QFileDialog::getOpenFileName(this, "Import", start,
                                                "Scenes and meshes (*.fbx *.obj *.ma *.mb);;FBX (*.fbx);;OBJ (*.obj);;Maya (*.ma *.mb)");
    if (file.isEmpty())
        return;
    const bool maya = file.endsWith(".ma", Qt::CaseInsensitive) || file.endsWith(".mb", Qt::CaseInsensitive);
    run(maya ? "mayaImport" : file.endsWith(".obj", Qt::CaseInsensitive) ? "importObj" : "importFbx", {{"args", {file.toStdString()}}});
}

void MainWindow::convertMayaFolder()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString dir = QFileDialog::getExistingDirectory(this, "Convert Maya Files in Folder", start);
    if (dir.isEmpty() || !confirmDiscard())
        return;
    try
    {
        json results = cmds::execute("mayaConvert", {{"source", dir.toStdString()}, {"force", true}}, CallSource::UI);
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
    QString file = QFileDialog::getSaveFileName(this, "Export Selection", start, "FBX for Unreal (*.fbx);;OBJ (*.obj)");
    if (file.isEmpty())
        return;
    if (file.endsWith(".obj", Qt::CaseInsensitive))
        run("exportObj", {{"args", {file.toStdString()}}});
    else
        run("exportFbx", {{"args", {file.toStdString()}}, {"profile", "ue5"}});
}

void MainWindow::sendToUnreal()
{
    // First send asks where each object goes; after that, saving the scene re-exports and reimports it.
    json selection = cmds::execute("ls", {{"selection", true}});
    if (selection.empty())
    {
        QMessageBox::information(this, "Send to Unreal", "Select the objects to send first.");
        return;
    }
    json existing = cmds::execute("unrealMappings", json::object());
    for (const json& name : selection)
    {
        bool mapped = false;
        for (const json& m : existing)
            mapped = mapped || m.value("node", "") == name.get<std::string>();
        if (mapped)
            continue;
        bool ok = false;
        QString path = QInputDialog::getText(this, "Send to Unreal", QString::fromStdString("Content path for " + name.get<std::string>()),
                                             QLineEdit::Normal, QString::fromStdString("/Game/Meshes/SM_" + name.get<std::string>()), &ok);
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
        title += " - " + QString::fromStdWString(ws.root().filename().wstring());
    setWindowTitle(title + " - Lila");
}

void MainWindow::registerActions()
{
    actions::add({"file.new", "New Scene", "File", [this] {
                      if (confirmDiscard())
                          actions::run(json{{"command", "newScene"}});
                  }, nullptr, "Clears the scene and starts an empty one, asking to save changes first."});
    actions::add({"file.open", "Open Scene...", "File", [this] { openScene({}); }, nullptr, "Opens a Lila or Maya scene, replacing the current one."});
    actions::add({"file.save", "Save Scene", "File", [this] { saveScene(false); }, nullptr, "Saves the scene to its file, asking for a name the first time."});
    actions::add({"file.saveAs", "Save Scene As...", "File", [this] { saveScene(true); }, nullptr, "Saves the scene under a new name and keeps working in that file."});
    actions::add({"file.incrementAndSave", "Increment and Save", "File", [this] { incrementAndSave(); }, nullptr,
                  "Save as the next free scene_v###, then keep working in it", "incrementAndSave()"});
    actions::add({"file.versions", "Versions...", "File", [this] { showVersions(); }, nullptr,
                  "Backups, autosaves, emergency saves, increments and source control revisions of this scene",
                  "sceneVersions()"});
    actions::add({"file.openFolder", "Open Folder...", "File", [this] { openFolder(); }, nullptr,
                  "Opens a project folder: file browsing, source control and workspace layouts work from it."});
    actions::add({"file.closeFolder", "Close Folder", "File", [] { Workspace::get().close(); }, nullptr, "Closes the project folder; the scene stays open."});
    actions::add({"file.exit", "Exit", "File", [this] { close(); }, nullptr, "Quits Lila, asking to save changes first."});
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
                          run("lookThru", {{"args", {n->name()}}});
                  },
                  [selectedCamera] { return selectedCamera() != nullptr; }, "The active view looks through the selected camera; navigating it then moves the camera.",
                  "lila.cmds.lookThru(\"camera1\")"});
    actions::add({"create.cameraFromView", "Camera from View", "Create", [this] { createCameraFromView(); }, nullptr,
                  "Creates a camera where the active view's camera is, with its lens, and selects it."});
    cmds::registerCommand({
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
                      resetLayout();
                      layouts::setCurrent("");
                  }, nullptr, "Puts every panel back in its default place and size."});
    actions::add({"window.toggleChannelBox", "Toggle Channel Box", "Windows", [this] { toggleChannelBox(); }, nullptr,
                  "Brings the channel box to the front of its tabs; again to go back to the tab shown before it."});
    actions::add({"window.toggleUvEditor", "Toggle UV Editor", "Windows", [this] { toggleUvEditor(); }, nullptr,
                  "Shows the UV editor, or tucks it back into its sidebar when it is showing."});
    actions::add({"window.expandPanels", "Expand Collapsed Panels", "Windows", [this] { mSidebars->expandAll(); }, nullptr,
                  "Opens every panel collapsed to a sidebar tab."});
    actions::add({"window.preferences", "Preferences...", "Windows", [this] { PreferencesDialog::open(this); }, nullptr,
                  "Settings for the viewport, manipulators, files, Python and the rest of Lila."});
    actions::add({"viewport.isolate", "Isolate Select", "View", [this] { mViewports->active()->toggleIsolate(); },
                  [this] { return mViewports->active()->isolating() || actions::hasSelection(); },
                  "Shows only the selected objects in the active view; again to show everything."});
    imagePlanes::install([this] { return mViewports->active(); });
    hud::registerActions([this] { return mViewports->active(); });
    cursorBadges::registerDefaults();
    actions::add({"viewport.toggleQuad", "Toggle Four Views", "View", [this] { mViewports->toggleQuad(); }, nullptr,
                  "Switches between one view and the top, front, side and perspective views together."});
    auto toggleGate = [this](const char* flag, const char* key) {
        std::string cam = mViewports->active()->viewName();
        bool on = scene::renderSettings().camera(cam).toJson()[key].get<bool>();
        try
        {
            cmds::execute("cameraSettings", {{"args", json::array({cam})}, {flag, !on}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    };
    actions::add({"viewport.toggleResolutionGate", "Resolution Gate", "View", [toggleGate] { toggleGate("gate", "resolutionGate"); },
                  nullptr, "Shows the render resolution's frame in the active view, darkening outside it.",
                  "lila.cmds.cameraSettings(\"persp\", gate=True)"});
    actions::add({"viewport.toggleFilmGate", "Film Gate", "View", [toggleGate] { toggleGate("filmGate", "filmGate"); }, nullptr,
                  "Shows the camera's film back in the active view.", "lila.cmds.cameraSettings(\"persp\", filmGate=True)"});
    actions::add({"viewport.cameraSettings", "Camera Settings...", "View",
                  [this] { RenderSettingsDialog::open(this, mViewports->active()->viewName()); }, nullptr,
                  "The active view's camera gates, film fit, overscan and gate mask."});
    actions::add({"window.renderSettings", "Render Settings...", "Windows", [this] { RenderSettingsDialog::open(this); }, nullptr,
                  "Render resolution and per-camera settings.", "lila.cmds.setRenderResolution(width=1920, height=1080)"});
    actions::add({"window.toggleMaximizePanel", "Maximize Viewport", "Windows", [this] {
                      bool hide = false;
                      for (QDockWidget* d : findChildren<QDockWidget*>())
                          hide |= d->isVisible() && !d->isFloating();
                      for (QDockWidget* d : findChildren<QDockWidget*>())
                          if (!d->isFloating())
                              d->setVisible(!hide);
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

QAction* MainWindow::addActionItem(QMenu* menu, const std::string& id)
{
    const Action* a = actions::find(id);
    QAction* qa = menu->addAction(QString::fromStdString(a ? a->label : id));
    qa->setData(QString::fromStdString(id));
    qa->setToolTip(QString::fromStdString(actions::tooltip(id)));
    if (a)
        qa->setStatusTip(QString::fromStdString(a->doc));
    connect(qa, &QAction::triggered, this, [id] { actions::run(id); });
    return qa;
}

QMenu* MainWindow::addMenu(const QString& title)
{
    return refreshKeysOnShow(tearoff::enable(menuBar()->addMenu(title)));
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
            const Action* a = actions::find(id);
            QString key = Hotkeys::get().keyFor(id);
            qa->setText(QString::fromStdString(a ? a->label : id) + (key.isEmpty() ? "" : "	" + key));
            qa->setToolTip(QString::fromStdString(actions::tooltip(id)));
            qa->setEnabled(actions::isEnabled(json(id)));
        }
    });
    optionBox::decorateMenu(menu);
    return menu;
}

void MainWindow::buildMenus()
{
    QMenu* file = addMenu("File");
    for (const char* id : {"file.new", "file.open"})
        addActionItem(file, id);
    mRecentFilesMenu = file->addMenu("Recent Scenes");
    connect(mRecentFilesMenu, &QMenu::aboutToShow, this, [this] {
        mRecentFilesMenu->clear();
        json recent = PrefStore::get().value("general", "/recentFiles", json::array());
        for (const json& r : recent.is_array() ? recent : json::array())
            if (r.is_string())
            {
                std::string p = r.get<std::string>();
                mRecentFilesMenu->addAction(QString::fromStdString(p), [this, p] { openScene(std::filesystem::path(p)); });
            }
        if (mRecentFilesMenu->isEmpty())
            mRecentFilesMenu->addAction("(none)")->setEnabled(false);
    });
    for (const char* id : {"file.save", "file.saveAs", "file.incrementAndSave", "file.versions"})
        addActionItem(file, id);
    file->addSeparator();
    addActionItem(file, "file.import");
    addActionItem(file, "file.convertMaya");
    addActionItem(file, "file.exportSelection");
    addActionItem(file, "file.createReference");
    file->addSeparator();
    for (const char* id : {"unreal.send", "unreal.sync", "unreal.connect"})
        addActionItem(file, id);
    file->addSeparator();
    addActionItem(file, "file.openFolder");
    mRecentMenu = file->addMenu("Recent Folders");
    addActionItem(file, "file.closeFolder");
    file->addSeparator();
    addActionItem(file, "file.exit");
    refreshRecentFolders();

    QMenu* edit = addMenu("Edit");
    for (const char* id : {"edit.undo", "edit.redo", "edit.flushUndo", "edit.flushRedo", "edit.flushAll"})
        addActionItem(edit, id);
    edit->addSeparator();
    for (const char* id : {"edit.selectAll", "edit.deselect", "edit.delete"})
        addActionItem(edit, id);
    edit->addSeparator();
    for (const char* id : {"edit.duplicate", "edit.group", "edit.parent", "edit.parentChain", "edit.unparent"})
        addActionItem(edit, id);
    edit->addSeparator();
    addActionItem(edit, "edit.rename");
    edit->addSeparator();
    addActionItem(edit, "window.preferences");

    QMenu* modify = addMenu("M&odify");
    for (const char* id : {"modify.freezeTransforms", "modify.resetTransforms", "modify.centerPivot", "modify.resetPivot"})
        addActionItem(modify, id);

    QMenu* create = addMenu("Create");
    QMenu* polys = create->addMenu("Polygon Primitives");
    for (const char* id : {"create.cube", "create.sphere", "create.cylinder", "create.cone", "create.plane", "create.torus", "create.disc",
                           "create.pipe", "create.quadSphere", "create.icoSphere"})
        addActionItem(polys, id);
    addActionItem(create, "create.locator");
    addActionItem(create, "create.camera");
    addActionItem(create, "create.cameraFromView");
    addActionItem(create, "create.emptyGroup");
    addActionItem(create, "create.imagePlane");

    QMenu* select = addMenu("Select");
    for (const char* id : {"select.objectMode", "select.vertexMode", "select.edgeMode", "select.faceMode"})
        addActionItem(select, id);
    select->addSeparator();
    for (const char* id : {"select.allComponents", "select.grow", "select.loop", "select.ring"})
        addActionItem(select, id);
    select->addSeparator();
    for (const char* id : {"select.hierarchy", "select.pickWalkUp", "select.pickWalkDown", "select.pickWalkLeft", "select.pickWalkRight"})
        addActionItem(select, id);
    select->addSeparator();
    for (const char* id : {"select.toVertices", "select.toEdges", "select.toFaces"})
        addActionItem(select, id);

    QMenu* mesh = addMenu("Mesh");
    for (const char* id : {"mesh.extrude", "mesh.inset", "mesh.bevel", "mesh.insertLoop", "mesh.bridge", "mesh.fillHole"})
        addActionItem(mesh, id);
    mesh->addSeparator();
    for (const char* id : {"mesh.merge", "mesh.mergeCenter", "mesh.collapse", "mesh.dissolve", "mesh.poke", "mesh.spin"})
        addActionItem(mesh, id);
    mesh->addSeparator();
    for (const char* id : {"mesh.flip", "mesh.conform", "mesh.triangulate", "mesh.quadrangulate", "mesh.subdivide"})
        addActionItem(mesh, id);
    mesh->addSeparator();
    for (const char* id : {"mesh.mirror", "mesh.symmetryX", "mesh.symmetryOff", "mesh.combine", "mesh.separate", "mesh.deleteHistory"})
        addActionItem(mesh, id);
    QMenu* symmetry = mesh->addMenu("Symmetry");
    for (const char* id : {"mesh.toggleSymmetry", "mesh.symmetryX", "mesh.symmetryY", "mesh.symmetryZ", "mesh.symmetryOff"})
        addActionItem(symmetry, id);
    symmetry->addSeparator();
    for (const char* id : {"mesh.symmetryCheck", "mesh.symmetrizePositive", "mesh.symmetrizeNegative", "mesh.symmetryRebuild",
                           "mesh.symmetryRebuildTopological"})
        addActionItem(symmetry, id);
    mesh->addSeparator();
    for (const char* id : {"mesh.booleanDifference", "mesh.booleanUnion", "mesh.booleanIntersection", "mesh.transferAttributes"})
        addActionItem(mesh, id);
    QMenu* vertexColorMenu = mesh->addMenu("Vertex Colors");
    for (const char* id : {"vcolor.paint", "vcolor.createSet", "", "vcolor.fill", "vcolor.gradient", "vcolor.invert", "vcolor.levels", "vcolor.blur", "",
                           "vcolor.bakeAO", "vcolor.bakeCavity", "vcolor.bakeCurvature", "vcolor.bakeHeight"})
    {
        if (*id)
            addActionItem(vertexColorMenu, id);
        else
            vertexColorMenu->addSeparator();
    }
    mesh->addSeparator();
    for (const char* id : {"retopo.quadDraw", "mesh.quadRemesh", "mesh.voxelRemesh", "mesh.repair", "mesh.selectIssues", "mesh.relax"})
        addActionItem(mesh, id);
    QMenu* sculptMenu = mesh->addMenu("Sculpt");
    for (const char* id : {"sculpt.tool", "sculpt.enter", "sculpt.exit", "sculpt.cancel", "window.sculpt", "mesh.relaxKeepForm", "mesh.reproject",
                           "mesh.transferShape"})
        addActionItem(sculptMenu, id);
    addActionItem(mesh, "window.nodeEditor");
    fillAddModifierMenu(mesh->addMenu("Add Modifier"));
    QMenu* autoBevel = mesh->addMenu("Auto Bevel");
    for (const char* id : {"modifier.autoBevel", "select.foldEdges", "bevel.editEdges", "bevel.applyEdges", "bevel.cancelEdges",
                           "bevel.measureThin", "bevel.clearOverrides", "bevel.showEdges"})
        addActionItem(autoBevel, id);

    QMenu* uvMenu = addMenu("UV");
    for (const char* id : {"window.uvEditor", "uv.auto", "uv.autoSeams", "uv.unwrap", "uv.projectPlanar", "uv.projectBox", "uv.projectCylindrical",
                           "uv.projectSpherical", "uv.cut", "uv.sew", "uv.straighten", "uv.orient", "uv.pack", "uv.lightmap"})
        addActionItem(uvMenu, id);
    uvMenu->addSeparator();
    for (const char* id : {"uv.autoSeams2", "uv.bff", "uv.bffDisk", "uv.bffRectangle", "uv.bffCones", "uv.unwrapSymmetric", "uv.unwrapSymmetricStack"})
        addActionItem(uvMenu, id);
    QMenu* packMenu = uvMenu->addMenu("Pack");
    for (const char* id : {"uv.pack", "uv.packAroundSelected", "uv.packSelectionOnly", "uv.packStacked", "uv.packUdimByMaterial", "uv.selectStacks"})
        addActionItem(packMenu, id);
    QMenu* liveMenu = uvMenu->addMenu("Live");
    for (const char* id : {"uv.liveMode", "uv.liveRelax", "uv.liveFlatten", "uv.liveSettle", "uv.pin", "uv.unpin", "uv.weld", "uv.brush3d",
                           "uv.checker3d", "uv.seams3d", "uv.heatmapTwoColour"})
        addActionItem(liveMenu, id);
    QMenu* optimizeMenu = uvMenu->addMenu("Optimize");
    for (const char* id : {"uv.optimizeConformal", "uv.optimizeAuthalic", "uv.optimizeBlend", "uv.optimizeIsometric", "uv.mosaic"})
        addActionItem(optimizeMenu, id);
    QMenu* alignMenu = uvMenu->addMenu("Align");
    for (const char* id : {"uv.straightenEdges", "uv.alignU", "uv.alignV", "uv.orientByEdge"})
        addActionItem(alignMenu, id);

    QMenu* rig = addMenu("Rig");
    for (const char* id : {"rig.jointTool", "rig.orientJoint", "rig.mirrorJoints", "rig.insertJoint", "rig.rerootSkeleton"})
        addActionItem(rig, id);
    rig->addSeparator();
    for (const char* id : {"rig.bindSkin", "rig.bindSkinHeatmap", "rig.unbindSkin", "rig.goToBindPose"})
        addActionItem(rig, id);
    rig->addSeparator();
    for (const char* id : {"rig.paintWeights", "rig.mirrorSkinWeights", "rig.copySkinWeights", "rig.normalizeWeights", "rig.pruneWeights"})
        addActionItem(rig, id);
    rig->addSeparator();
    for (const char* id : {"rig.parentConstraint", "rig.pointConstraint", "rig.orientConstraint", "rig.aimConstraint", "rig.removeConstraint"})
        addActionItem(rig, id);
    rig->addSeparator();
    addActionItem(rig, "rig.xrayJoints");

    QMenu* deform = addMenu("Deform");
    for (const char* id : {"deform.blendShape", "deform.addBlendTarget", "deform.extractBlendTarget"})
        addActionItem(deform, id);
    deform->addSeparator();
    addActionItem(deform, "deform.lattice");
    QMenu* nonlinear = deform->addMenu("Nonlinear");
    for (const char* id : {"deform.bend", "deform.twist", "deform.taper", "deform.squash", "deform.sine", "deform.wave"})
        addActionItem(nonlinear, id);
    for (const char* id : {"deform.softMod", "deform.deltaMush", "deform.wrap", "deform.cage"})
        addActionItem(deform, id);
    deform->addSeparator();
    addActionItem(deform, "deform.rebind");

    QMenu* display = addMenu("Display");
    // Toggles show their state, read from the active view when they are per view.
    auto syncChecks = [this](QMenu* menu) {
        connect(menu, &QMenu::aboutToShow, menu, [this, menu] {
            for (QAction* qa : menu->actions())
                if (std::optional<bool> on = viewportActionState(qa->data().toString().toStdString(), mViewports->active()))
                {
                    qa->setCheckable(true);
                    qa->setChecked(*on);
                }
        });
    };
    syncChecks(display);
    for (const char* id : {"viewport.wireframe", "viewport.shaded", "viewport.textured", "viewport.wireOnShaded", "viewport.defaultMaterial",
                           "viewport.toggleXray", "viewport.toggleBackfaceCulling"})
        addActionItem(display, id);
    display->addSeparator();
    QMenu* lighting = display->addMenu("Lighting");
    syncChecks(lighting);
    for (const char* id : {"viewport.lighting.environment", "viewport.lighting.studio", "viewport.lighting.flat"})
        addActionItem(lighting, id);
    QMenu* vertexColors = display->addMenu("Vertex Colors");
    syncChecks(vertexColors);
    for (const char* id : {"viewport.vertexColors.off", "viewport.vertexColors.shaded", "viewport.vertexColors.unlit", "",
                           "viewport.colorChannel.rgb", "viewport.colorChannel.r", "viewport.colorChannel.g", "viewport.colorChannel.b",
                           "viewport.colorChannel.a", "", "viewport.grayscaleWeights"})
    {
        if (*id)
            addActionItem(vertexColors, id);
        else
            vertexColors->addSeparator();
    }
    for (const char* id : {"viewport.toggleShadows", "viewport.toggleAO", "viewport.toggleAA"})
        addActionItem(display, id);
    QMenu* samples = display->addMenu("Antialiasing Samples");
    syncChecks(samples);
    for (const char* id : {"viewport.aaSamples2", "viewport.aaSamples4", "viewport.aaSamples8"})
        addActionItem(samples, id);
    QMenu* envMenu = display->addMenu("Environment");
    connect(envMenu, &QMenu::aboutToShow, envMenu, [envMenu] { fillEnvironmentMenu(envMenu); });
    addActionItem(display, "viewport.toggleEnvBackground");
    display->addSeparator();
    for (const char* id : {"display.toggleVisibility", "display.hide", "display.show", "display.showAll", "viewport.isolate"})
        addActionItem(display, id);
    QMenu* measure = display->addMenu("Measure");
    syncChecks(measure);
    for (const char* id : {"measure.selection", "measure.copyDistance", "measure.copyDiameter", "measure.copyLength", "", "measure.distanceTool",
                           "measure.angleTool", "measure.radiusTool", "", "measure.distanceFromSelection", "measure.angleFromSelection",
                           "measure.radiusFromSelection", "", "measure.toggleShow"})
    {
        if (*id)
            addActionItem(measure, id);
        else
            measure->addSeparator();
    }
    display->addSeparator();
    QMenu* layerMenu = display->addMenu("Display Layers");
    for (const char* id : {"window.displayLayers", "layers.create", "layers.createFromSelected", "layers.addSelected", "layers.removeSelected"})
        addActionItem(layerMenu, id);
    display->addSeparator();
    QMenu* views = display->addMenu("Views");
    syncChecks(views);
    for (const char* view : {"persp", "top", "front", "right", "left", "back", "bottom"})
        addActionItem(views, (std::string("viewport.view.") + view).c_str());
    addActionItem(display, "viewport.frameSelected");
    addActionItem(display, "viewport.frameAll");
    addActionItem(display, "viewport.cameraUndo");
    addActionItem(display, "viewport.cameraRedo");
    display->addSeparator();
    for (const char* id : {"viewport.toggleGrid", "viewport.toggleResolutionGate", "viewport.toggleFilmGate"})
        addActionItem(display, id);
    addActionItem(display, "viewport.cameraSettings");
    addActionItem(display, "window.renderSettings");
    display->addSeparator();
    addActionItem(display, "viewport.toggleToolbar");
    addActionItem(display, "viewport.resetExposure");

    QMenu* animation = addMenu("Animation");
    for (const char* id : {"anim.setKey", "anim.setKeyTranslate", "anim.setKeyRotate", "anim.setKeyScale", "anim.autoKeyToggle"})
        addActionItem(animation, id);
    animation->addSeparator();
    for (const char* id : {"anim.play", "anim.playBackward", "anim.stop", "anim.firstFrame", "anim.lastFrame", "anim.nextFrame",
                           "anim.prevFrame", "anim.nextKey", "anim.prevKey"})
        addActionItem(animation, id);
    animation->addSeparator();
    addActionItem(animation, "window.graphEditor");
    addActionItem(animation, "window.timeSlider");

    QMenu* windows = addMenu("Windows");
    for (QDockWidget* dock : findChildren<QDockWidget*>())
        windows->addAction(dock->toggleViewAction());
    windows->addSeparator();
    addActionItem(windows, "window.toggleMaximizePanel");
    addActionItem(windows, "window.toggleChannelBox");
    addActionItem(windows, "window.toggleUvEditor");
    addActionItem(windows, "viewport.frameAll");
    addActionItem(windows, "window.resetLayout");
    windows->addMenu(mLayouts->menu());
    addActionItem(windows, "window.expandPanels");
    windows->addSeparator();
    addActionItem(windows, "window.materialEditor");
    addActionItem(windows, "window.scriptEditor");
    addActionItem(windows, "window.apiReference");
    addActionItem(windows, "window.terminal");
    addActionItem(windows, "window.displayLayers");
    addActionItem(windows, "window.undoHistory");
    addActionItem(windows, "window.hotkeyEditor");
    addActionItem(windows, "window.markingMenuEditor");

    QMenu* help = addMenu("Help");
    addActionItem(help, "help.agentSetup");
    addActionItem(help, "help.prefsFolder");
    addActionItem(help, "help.crashReports");
    addActionItem(help, "help.copyLastCrash");
    addActionItem(help, "help.hotkeyCheatSheet");
    addActionItem(help, "help.about");

    buildPanelMenus();
}

void MainWindow::buildPanelMenus()
{
    // Maya's viewport panel menus. Lila's views have no menu bar of their own, so these live in the hotbox.
    auto panelMenu = [this](const char* title, std::initializer_list<const char*> ids) {
        QMenu* menu = refreshKeysOnShow(new QMenu(title, this));
        for (const char* id : ids)
        {
            if (*id)
                addActionItem(menu, id);
            else
                menu->addSeparator();
        }
        return menu;
    };
    QMenu* view = panelMenu("View", {"viewport.view.persp", "viewport.view.top", "viewport.view.front", "viewport.view.right",
                                     "viewport.view.left", "viewport.view.back", "viewport.view.bottom", "", "viewport.frameSelected",
                                     "viewport.frameAll", "viewport.cameraUndo", "viewport.cameraRedo", "", "viewport.toggleResolutionGate",
                                     "viewport.toggleFilmGate", "viewport.cameraSettings", "", "viewport.imagePlaneImport"});
    QMenu* shading = panelMenu("Shading", {"viewport.wireframe", "viewport.shaded", "viewport.textured", "viewport.wireOnShaded", "",
                                           "viewport.toggleAO", "", "display.smoothOff", "display.smoothCage", "display.smoothOn"});
    QMenu* showMenu = panelMenu("Show", {"display.hide", "display.show", "display.showAll", "", "viewport.isolate", "rig.xrayJoints",
                                         "measure.toggleShow", "viewport.toggleImagePlanes"});
    QMenu* renderer = panelMenu("Renderer", {"window.renderSettings"});
    QMenu* panels = panelMenu("Panels", {"viewport.lookThroughSelected", "", "viewport.toggleQuad", "window.toggleMaximizePanel", "",
                                         "window.uvEditor", "window.graphEditor", "window.nodeEditor", "window.materialEditor"});
    // Maya's Panels > Perspective / Orthographic: the views plus every camera node, rebuilt as the menu opens.
    auto* cameras = new QMenu("Cameras", panels);
    panels->insertMenu(panels->actions().value(1), cameras);
    connect(cameras, &QMenu::aboutToShow, cameras, [this, cameras] { fillCameraMenu(cameras, mViewports->active()); });
    // The layout switcher lives in the menu bar corner, so the hotbox carries it too for when the menu bar is hidden.
    Hotbox::install(this, {view, shading, showMenu, renderer, panels, mLayouts->menu()});
}

void MainWindow::buildStatusBar()
{
    auto* help = new QLabel(this);
    help->setMinimumWidth(1);
    statusBar()->addWidget(help, 1);
    qApp->installEventFilter(new HelpLine(help, this));
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
                                      [output] { return output->isVisible() && !output->visibleRegion().isEmpty(); }, this);
        statusBar()->addWidget(line, 1);
        connect(output, &QDockWidget::visibilityChanged, line, qOverload<>(&QWidget::update));
    }
    installSpinScrub(this);

    mWorkspaceLabel = new QLabel(this);
    mWorkspaceLabel->setStyleSheet("color: #9a9a9a;");
    statusBar()->addPermanentWidget(mWorkspaceLabel);
    refreshWorkspaceLabel();

    mSourceLabel = new QLabel(this);
    mSourceLabel->setStyleSheet("color: #9a9a9a;");
    statusBar()->addPermanentWidget(mSourceLabel);
    statusBar()->addPermanentWidget(new QLabel(QString::fromStdString(render::Renderer::get().deviceName()), this));

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

void MainWindow::refreshWorkspaceLabel()
{
    auto& ws = Workspace::get();
    mWorkspaceLabel->setText(ws.isOpen() ? QString::fromStdWString(ws.root().wstring()) : QString("No folder open"));
    refreshTitle();
}

void MainWindow::refreshRecentFolders()
{
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
                       "<p><i>Lila (&#2354;&#2368;&#2354;&#2366;): Sanskrit for divine play, or creation.</i></p>"
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
    }
    else
        QTimer::singleShot(0, this, [this] {
            applyDefaultDockSizes();
            mSidebars->restoreCollapsed(kDefaultCollapsed);
        });
    mRestoring = false;
}

void MainWindow::applyDefaultDockSizes()
{
    if (auto* channels = findChild<QDockWidget*>("channelBox"))
        resizeDocks({channels}, {kDefaultSidePanelWidth}, Qt::Horizontal);
    auto* outliner = findChild<QDockWidget*>("outliner");
    auto* undoHistory = findChild<QDockWidget*>(kUndoHistoryDock);
    if (outliner && undoHistory && outliner->isVisible() && undoHistory->isVisible())
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
        std::string name = cmds::execute("createNode", {{"args", {"camera"}}}, CallSource::UI).get<std::string>();
        auto set = [&](const char* attr, json value) { cmds::execute("setAttr", {{"args", {name + "." + attr, std::move(value)}}}, CallSource::UI); };
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
        cmds::execute("viewPlace", {{"args", {name}}, {"eye", vec(eye)}, {"lookAt", vec(c.target)}, {"up", vec(c.viewUp())}},
                      CallSource::UI);
        cmds::execute("selectMode", {{"args", {"object"}}}, CallSource::UI);
        cmds::execute("select", {{"args", {name}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void MainWindow::renameSelection()
{
    json selection = cmds::execute("ls", {{"selection", true}});
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
    mSidebars->expandAll();
    restoreState(mDefaultState);
    for (QDockWidget* d : findChildren<QDockWidget*>())
        if (tearoff::parseDockName(d->objectName().toStdString()))
            d->hide();
    applyDefaultDockSizes();
    mSidebars->restoreCollapsed(kDefaultCollapsed);
    scheduleLayoutSave();
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
