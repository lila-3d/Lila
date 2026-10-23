#include "app/Host.h"

#include "cmds/Command.h"
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
#include "retopo/RetopoCommands.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "sculpt/SculptSession.h"
#include "ui/Layouts.h"
#include "uv/UvCommands.h"
#include "vcolor/VertexColors.h"
#include "vcs/SourceControl.h"
#include "vcs/VcsCommands.h"

#include <algorithm>
#include <cstdio>
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
    io::translators::registerBuiltins();
    io::translators::registerCommands();
    ui::layouts::registerCommands();
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
    crash::startWatchdog(crashOptions(false));
    installConsoleProgress();
    if (!pythonOk)
        return 2;
    int rc = 0;
    if (!o.command.empty() && !python::run(o.command, "<command>").ok)
        rc = 1;
    if (!o.batchScript.empty() && !python::runFile(o.batchScript).ok)
        rc = 1;
    mainq::drain();
    return rc;
}

void shutdown()
{
    events::send({.type = events::Event::Shutdown});
    extensions::ExtensionManager::get().unloadAll();
    vcs::SourceControl::get().shutdown();
    link::Link::get().shutdown();
    python::shutdown();
    PrefStore::get().stop();
    crash::shutdown();
}
} // namespace lila::host
