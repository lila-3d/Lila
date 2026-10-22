#include "cmds/Command.h"
#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "fbx/Fbx.h"
#include "maya/Maya.h"
#include "gn/Gn.h"
#include "io/Recovery.h"
#include "io/SceneFile.h"
#include "retopo/RetopoCommands.h"
#include "sculpt/SculptSession.h"
#include "vcolor/VertexColors.h"
#include "uv/UvCommands.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "math/MathCore.h"
#include "mcp/McpServer.h"
#include "extensions/ExtensionManager.h"
#include "python/Python.h"
#include "render/Renderer.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/Hotkeys.h"
#include "ui/Layouts.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Editors.h"
#include "ui/Animation.h"
#include "ui/MaterialEditor.h"
#include "ui/ModelingToolkit.h"
#include "ui/ModifierStack.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/QuadDrawTool.h"
#include "ui/SculptPanel.h"
#include "ui/UvEditor.h"
#include "ui/VertexPaintTool.h"
#include "ui/MeasureTool.h"
#include "ui/AutoBevelUi.h"
#include "ui/RigViewport.h"
#include "ui/DeformUi.h"
#include "ui/Shelf.h"
#include "ui/Tools.h"
#include "ui/UiTools.h"
#include "ui/AgentCapture.h"
#include "ui/AgentSetup.h"
#include "vcs/SourceControl.h"
#include "vcs/VcsCommands.h"
#include "scene/Components.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QTimer>

#include <cstring>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif
#include <filesystem>

namespace fs = std::filesystem;
using namespace lila;

namespace
{
struct Options
{
    std::string batchScript;
    std::string command;
    std::string path;
    bool batch = false;
};

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--batch" || a == "-b")
        {
            o.batch = true;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                o.batchScript = argv[++i];
        }
        else if ((a == "-c" || a == "--command") && i + 1 < argc)
            o.command = argv[++i];
        else if (a[0] != '-')
            o.path = a;
    }
    return o;
}

/** Opens the workspace the command line names. A scene file it names is loaded here in batch mode; the GUI gets it back to open once its window is up, under the progress overlay. */
fs::path openStartupWorkspace(const Options& o)
{
    if (!o.path.empty())
    {
        fs::path p = fs::absolute(o.path);
        if (fs::is_directory(p))
            Workspace::get().open(p);
        else if (auto ws = Workspace::detect(p))
            Workspace::get().open(*ws);
        const bool scene = p.extension() == ".lila" || p.extension() == ".lilaa" || maya::isMayaFile(p);
        if (scene && !o.batch)
            return p;
        if (p.extension() == ".lila" || p.extension() == ".lilaa")
        {
            try
            {
                io::load(p);
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        }
        else if (maya::isMayaFile(p))
        {
            try
            {
                std::u8string u8 = p.generic_u8string();
                cmds::execute("mayaOpen", {{"args", {std::string(u8.begin(), u8.end())}}, {"force", true}});
            }
            catch (const std::exception& e)
            {
                log::error("{}", e.what());
            }
        }
        return {};
    }
    if (o.batch)
        return {};
    if (PrefStore::get().value<bool>("general", "/reopenLastFolder", true))
    {
        json recent = PrefStore::get().value("general", "/recentFolders", json::array());
        if (recent.is_array() && !recent.empty() && recent[0].is_string() && fs::is_directory(recent[0].get<std::string>()))
            Workspace::get().open(recent[0].get<std::string>());
    }
    return {};
}

crash::WatchOptions crashOptions(bool gui)
{
    crash::WatchOptions o;
    o.ui = gui;
    o.registerWer = gui;
    o.keepReports = size_t(std::max(1, PrefStore::get().value<int>("general", "/crash/keepReports", 20)));
    o.fullDump = PrefStore::get().value<bool>("general", "/crash/fullDump", false);
    return o;
}

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
    crash::setContext("context", "tool", ui::tools::name(ui::tools::current()));
    ui::ViewportTool* vt = ui::viewportTools::active();
    crash::setContext("context", "viewportTool", vt ? vt->name() : "none");
    crash::setContext("context", "componentMode", scene::components::modeName(scene::components::mode()));
    std::string loaded;
    for (const std::string& name : extensions::ExtensionManager::get().loaded())
        loaded += (loaded.empty() ? "" : ", ") + name;
    crash::setContext("context", "extensions", loaded.empty() ? "none" : loaded);
}

/** lilabatch on a console: long operations as one line on stderr, rewritten in place. */
void installConsoleProgress()
{
    if (!_isatty(_fileno(stderr)))
        return;
    progress::setReporter(
        [](const std::vector<progress::Snapshot>& roots) {
            static size_t shown = 0;
            std::vector<progress::Snapshot> visible;
            for (const progress::Snapshot& s : roots)
                if (s.elapsed >= 0.3)
                    visible.push_back(s);
            const std::string line = progress::consoleLine(visible);
            if (line.empty() && shown == 0)
                return;
            // Pad over the longer previous line, then park the cursor at the end of this one.
            std::string out = "\r" + line;
            if (line.size() < shown)
                out += std::string(shown - line.size(), ' ') + "\r" + line;
            shown = line.size();
            std::fputs(out.c_str(), stderr);
            std::fflush(stderr);
        },
        150);
}

int runBatch(const Options& o)
{
    int rc = 0;
    if (!o.command.empty() && !python::run(o.command, "<command>").ok)
        rc = 1;
    if (!o.batchScript.empty() && !python::runFile(o.batchScript).ok)
        rc = 1;
    mainq::drain();
    return rc;
}
} // namespace

int main(int argc, char** argv)
{
    crash::install();
    Options opts = parse(argc, argv);
#if defined(_WIN32) && defined(LILA_GUI_SUBSYSTEM)
    // The GUI build has no console of its own; batch output goes to the shell that launched it, if any.
    if (opts.batch && AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif

    mainq::init();
    PrefStore::get().start();
    auto syncUpAxis = [] {
        bool z = PrefStore::get().value<std::string>("general", "/scene/upAxis", "y") == "z";
        math::setUpAxis(z ? math::UpAxis::Z : math::UpAxis::Y);
    };
    syncUpAxis();
    PrefStore::get().onChanged([syncUpAxis](const std::string& file) {
        if (file == "general")
            syncUpAxis();
    });
    cmds::registerBuiltins();
    scene::types::registerBuiltins();
    gn::registerTypes();
    scene::registerCommands();
    io::registerCommands();
    gn::registerCommands();
    uv::registerCommands();
    retopo::registerCommands();
    sculpt::registerCommands();
    vcolor::registerCommands();
    vcs::registerCommands();
    extensions::registerCommands();
    link::registerCommands();
    fbx::registerCommands();
    maya::registerCommands();
    ui::layouts::registerCommands();
    ui::capture::registerCommands();
    link::registerExporter("fbx", [](const std::vector<scene::NodeId>& nodes, const std::filesystem::path& path, std::string* err) {
        return fbx::exportForUnreal(nodes, path, "ue5", err);
    });
    for (std::string kind : {"skeletal", "animation"})
        link::registerExporter("fbx:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const std::filesystem::path& path, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind);
        });
    io::onSaved([](const std::filesystem::path&) { link::onSceneSaved(); });
    const fs::path startupScene = openStartupWorkspace(opts);

    std::vector<std::string> pyArgs{opts.batchScript.empty() ? "lila" : opts.batchScript};
    bool pythonOk = python::init(pyArgs);
    if (pythonOk)
        extensions::autoload();
    crash::addSecret(PrefStore::get().value<std::string>("general", "/mcp/token", ""));
    crash::setContext("app", "mode", opts.batch ? "batch" : "gui");

    int rc = 0;
    if (opts.batch)
    {
        crash::startWatchdog(crashOptions(false));
        installConsoleProgress();
        rc = pythonOk ? runBatch(opts) : 2;
        // captureView starts the renderer on demand in batch mode.
        render::Renderer::get().stop();
    }
    else
    {
        QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
        QApplication app(argc, argv);
        QApplication::setApplicationName("Lila");
        QApplication::setOrganizationName("Lila");
        ui::applyTheme(app);

        mainq::setWakeup([&app] { QMetaObject::invokeMethod(&app, [] { mainq::drain(); }, Qt::QueuedConnection); });
        mainq::drain();

        ui::actions::registerBuiltins();
        ui::tools::registerActions();
        ui::registerShelfActions();
        ui::registerEditorActions();
        ui::registerPanelActions();
        ui::registerMaterialUi();
        ui::registerNodeEditorActions();
        ui::registerUvActions();
        ui::registerModelingToolkitActions();
        ui::registerModifierStackActions();
        ui::adjustLastOp::install();
        ui::registerRetopoActions();
        ui::registerSculptUi();
        ui::registerVertexPaintUi();
        ui::registerAnimationUi();
        python::uiHooks().runAction = [](const std::string& id) { return ui::actions::run(json(id)); };
        python::uiHooks().listActions = [] {
            std::vector<std::array<std::string, 3>> out;
            for (const ui::Action* a : ui::actions::all())
                out.push_back({a->id, a->label, a->category});
            return out;
        };
        ui::Hotkeys::get().install();
        render::Renderer::get().start();
        ui::registerUiTools();
        ui::capture::registerMcpTools();
        ui::agentSetup::registerActions();
        ui::registerRigViewport();
        ui::registerDeformActions();
        ui::registerAutoBevelUi();
        ui::registerMeasureUi();
        mcp::startFromPrefs();
        crash::addSecret(mcp::Server::get().token());
        crash::setContext("app", "qt", qVersion());
        crash::startWatchdog(crashOptions(true));
        updateCrashContext();
        QTimer crashContext;
        QObject::connect(&crashContext, &QTimer::timeout, [] { updateCrashContext(); });
        crashContext.start(1000);

        // Read before our own session file exists, so it is never mistaken for a crashed one.
        std::vector<io::recovery::Session> stale = io::recovery::staleSessions(io::recovery::sessionsDir(), io::recovery::currentPid());
        io::recovery::beginSession();
        io::recovery::installCrashSave();
        {
            ui::MainWindow window;
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
    }

    extensions::ExtensionManager::get().unloadAll();
    vcs::SourceControl::get().shutdown();
    link::Link::get().shutdown();
    python::shutdown();
    PrefStore::get().stop();
    crash::shutdown();
    return rc;
}
