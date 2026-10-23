#include "ui/AppMain.h"

#include "core/CrashHandler.h"
#include "core/Events.h"
#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "io/Recovery.h"
#include "io/SceneFile.h"
#include "core/Workspace.h"
#include "mcp/McpServer.h"
#include "python/Python.h"
#include "render/Renderer.h"
#include "scene/Components.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/AgentCapture.h"
#include "ui/AgentSetup.h"
#include "ui/Animation.h"
#include "ui/AutoBevelUi.h"
#include "ui/DeformUi.h"
#include "ui/Editors.h"
#include "ui/ExtensionUi.h"
#include "ui/Ghosting.h"
#include "ui/Hotkeys.h"
#include "ui/KnifeTool.h"
#include "ui/MainWindow.h"
#include "ui/MaterialEditor.h"
#include "ui/MeasureTool.h"
#include "ui/ModelingToolkit.h"
#include "ui/Modes.h"
#include "ui/ModifierStack.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/PoseLibrary.h"
#include "ui/ControlPicker.h"
#include "ui/QuadDrawTool.h"
#include "ui/RigViewport.h"
#include "ui/SculptPanel.h"
#include "ui/Shelf.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/UiTools.h"
#include "ui/UvEditor.h"
#include "ui/VertexPaintTool.h"
#include "ui/ViewportTool.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QTimer>

namespace fs = std::filesystem;

namespace lila::ui
{
namespace
{
std::string utf8(const fs::path& p)
{
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

/** What the crash report says Lila was doing; polled, so a crash costs nothing to describe. */
void updateCrashContext()
{
    crash::setContext("context", "scene", io::currentPath().empty() ? "untitled" : utf8(io::currentPath()));
    crash::setContext("context", "scene.modified", io::modified() ? "true" : "false");
    crash::setContext("context", "workspace", Workspace::get().isOpen() ? utf8(Workspace::get().root()) : "none");
    crash::setContext("context", "tool", tools::name(tools::current()));
    ViewportTool* vt = viewportTools::active();
    crash::setContext("context", "viewportTool", vt ? vt->name() : "none");
    crash::setContext("context", "componentMode", scene::components::modeName(scene::components::mode()));
    std::string loaded;
    for (const std::string& name : extensions::ExtensionManager::get().loaded())
        loaded += (loaded.empty() ? "" : ", ") + name;
    crash::setContext("context", "extensions", loaded.empty() ? "none" : loaded);
}

void registerUi()
{
    actions::registerBuiltins();
    tools::registerActions();
    registerShelfActions();
    registerEditorActions();
    registerPanelActions();
    registerMaterialUi();
    registerNodeEditorActions();
    registerUvActions();
    registerModelingToolkitActions();
    registerModifierStackActions();
    adjustLastOp::install();
    registerRetopoActions();
    registerKnifeActions();
    registerSculptUi();
    registerVertexPaintUi();
    registerAnimationUi();
    registerGhostingUi();
    registerPoseLibraryUi();
    registerControlPickerUi();
    modes::registerBuiltins();
    extensionUi::install();
    python::uiHooks().runAction = [](const std::string& id) { return actions::run(json(id)); };
    python::uiHooks().listActions = [] {
        std::vector<std::array<std::string, 3>> out;
        for (const Action* a : actions::all())
            out.push_back({a->id, a->label, a->category});
        return out;
    };
}
} // namespace

void registerBatchCommands()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    capture::registerCommands();
    modes::registerBuiltins();
    // captureView starts the renderer on demand in batch mode.
    events::subscribe(events::Event::Shutdown, [](const events::EventData&) { render::Renderer::get().stop(); });
}

int runApp(int argc, char** argv, const host::Options& opts, const fs::path& startupScene)
{
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QApplication::setApplicationName("Lila");
    QApplication::setOrganizationName("Lila");
    applyTheme(app);

    mainq::setWakeup([&app] { QMetaObject::invokeMethod(&app, [] { mainq::drain(); }, Qt::QueuedConnection); });
    mainq::drain();

    registerUi();
    Hotkeys::get().install();
    render::Renderer::get().start();
    registerUiTools();
    capture::registerMcpTools();
    agentSetup::registerActions();
    registerRigViewport();
    registerDeformActions();
    registerAutoBevelUi();
    registerMeasureUi();
    mcp::startFromPrefs();
    crash::addSecret(mcp::Server::get().token());
    crash::setContext("app", "qt", qVersion());
    crash::startWatchdog(host::crashOptions(true));
    updateCrashContext();
    QTimer crashContext;
    QObject::connect(&crashContext, &QTimer::timeout, [] { updateCrashContext(); });
    crashContext.start(1000);
    // The event loop is about to wait: AppIdle, only while someone listens.
    QObject::connect(QAbstractEventDispatcher::instance(), &QAbstractEventDispatcher::aboutToBlock, [] {
        if (events::hasSubscribers(events::Event::AppIdle))
            events::send({.type = events::Event::AppIdle});
    });

    int rc = 0;
    // Read before our own session file exists, so it is never mistaken for a crashed one.
    std::vector<io::recovery::Session> stale = io::recovery::staleSessions(io::recovery::sessionsDir(), io::recovery::currentPid());
    io::recovery::beginSession();
    io::recovery::installCrashSave();
    {
        MainWindow window;
        window.show();
        // One callback, in order: a -c command must not run inside the scene load's event pumping.
        QTimer::singleShot(0, &window, [&window, &opts, stale, startupScene] {
            window.startup(stale, io::currentPath().empty() && startupScene.empty());
            if (!startupScene.empty())
                window.openFile(startupScene);
            if (!opts.command.empty())
                python::run(opts.command, "<command>");
        });
        rc = app.exec();
    }
    io::recovery::endSession();

    mcp::Server::get().stop();
    render::Renderer::get().stop();
    mainq::setWakeup(nullptr);
    return rc;
}
} // namespace lila::ui

void lilaUiRegisterBatchCommands()
{
    lila::ui::registerBatchCommands();
}
