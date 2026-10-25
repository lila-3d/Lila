#include "app/Host.h"

#include "ops/Command.h"
#include "bake/Bake.h"
#include "bake/BakePipeline.h"
#include "batch/BatchCommands.h"
#include "batch/BatchRunner.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"
#include "fbx/Fbx.h"
#include "gn/Gn.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "math/MathCore.h"
#include "maya/Maya.h"
#include "python/Python.h"
#include "curve/CurveNodes.h"
#include "retopo/RetopoCommands.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "sculpt/SculptSession.h"
#include "seq/MeshSequence.h"
#include "tex/GraphFiles.h"
#include "tex/Interop.h"
#include "tex/Library.h"
#include "tex/TextureSet.h"
#include "ui/Layouts.h"
#include "ui/ThemeModel.h"
#include "uv/UvCommands.h"
#include "vcolor/VertexColors.h"
#include "vcs/SourceControl.h"
#include "vcs/VcsCommands.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#include <io.h>
#endif

namespace fs = std::filesystem;

namespace lila::host
{
namespace
{
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
} // namespace

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--batch-run")
        {
            o.batch = true;
            o.batchRunCli = true;
            o.batchRun.assign(argv + i + 1, argv + argc);
            break;
        }
        if (a == "--batch-run-worker" && i + 2 < argc)
        {
            o.batch = true;
            o.workerJob = argv[i + 1];
            o.workerIndex = size_t(std::strtoull(argv[i + 2], nullptr, 10));
            break;
        }
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

void startCore()
{
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
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    gn::registerTypes();
    curve::registerTypes();
    tex::registerTypes();
    scene::registerCommands();
    io::registerCommands();
    gn::registerCommands();
    seq::registerCommands();
    bake::registerCommands();
    tex::registerCommands();
    tex::registerFileCommands();
    tex::library::registerCommands();
    uv::registerCommands();
    retopo::registerCommands();
    curve::registerCommands();
    sculpt::registerCommands();
    vcolor::registerCommands();
    vcs::registerCommands();
    extensions::registerCommands();
    link::registerCommands();
    fbx::registerCommands();
    maya::registerCommands();
    batch::registerCommands();
    io::translators::registerBuiltins();
    io::translators::registerCommands();
    ui::layouts::registerCommands();
    ui::themes::registerCommands();
    link::registerExporter("fbx", [](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
        return fbx::exportForUnreal(nodes, path, "ue5", err);
    });
    for (std::string kind : {"skeletal", "animation"})
        link::registerExporter("fbx:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind);
        });
    for (std::string kind : {"skeletal", "animation"})
        link::registerTakeExporter("fbx:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const fs::path& path,
                                                         const std::string& take, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind, true, take);
        });
    for (std::string kind : {"static", "skeletal", "animation"})
        link::registerExporter("fbx:set:" + kind, [kind](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string* err) {
            return fbx::exportForUnreal(nodes, path, "ue5", err, kind, false);
        });
    // Rebakes first, so a mapped mesh's sync to Unreal picks up fresh textures.
    io::onSaved([](const fs::path&) { bake::onSceneSaved(); });
    io::onSaved([](const fs::path& p) { tex::onSceneSaved(p); });
    io::onSaved([](const fs::path&) { link::onSceneSaved(); });
}

fs::path openStartupWorkspace(const Options& o)
{
    if (!o.path.empty())
    {
        fs::path p = fs::absolute(o.path);
        if (fs::is_directory(p))
            Workspace::get().open(p);
        else if (auto ws = Workspace::detect(p))
            Workspace::get().open(*ws);
        const bool scene = io::translators::forPath(io::translators::Op::Open, p) != nullptr;
        if (scene && !o.batch)
            return p;
        if (scene)
        {
            try
            {
                io::translators::run(io::translators::Op::Open, p);
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

bool startPython(const Options& o)
{
    std::vector<std::string> pyArgs{o.batchScript.empty() ? "lila" : o.batchScript};
    bool ok = python::init(pyArgs);
    if (ok)
        extensions::autoload();
    crash::addSecret(PrefStore::get().value<std::string>("general", "/mcp/token", ""));
    crash::setContext("app", "mode", o.batch ? "batch" : "gui");
    return ok;
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

int runBatch(const Options& o, bool pythonOk)
{
    // A worker's crash is its run's to report, and its output is already captured to a log.
    if (!o.workerJob.empty())
    {
        int rc = batch::runWorker(o.workerJob, o.workerIndex, pythonOk);
        mainq::drain();
        return rc;
    }
    crash::startWatchdog(crashOptions(false));
    installConsoleProgress();
    if (!pythonOk)
        return 2;
    int rc = 0;
    if (!o.command.empty() && !python::run(o.command, "<command>").ok)
        rc = 1;
    if (!o.batchScript.empty() && !python::runFile(o.batchScript).ok)
        rc = 1;
    if (o.batchRunCli)
        rc = std::max(rc, batch::runCli(o.batchRun));
    mainq::drain();
    return rc;
}

void shutdown()
{
    events::send({.type = events::Event::Shutdown});
    batch::shutdown();
    extensions::ExtensionManager::get().unloadAll();
    vcs::SourceControl::get().shutdown();
    link::Link::get().shutdown();
    python::shutdown();
    PrefStore::get().stop();
    crash::shutdown();
}
} // namespace lila::host
