#include "python/Python.h"

#include "core/CrashHandler.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "ops/Undo.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>

namespace nb = nanobind;

extern "C" PyObject* PyInit__lila();

namespace lila::python
{
namespace
{
bool gInitialized = false;
PyThreadState* gMainState = nullptr;

std::wstring widen(const std::filesystem::path& p)
{
    return p.wstring();
}

/** Closes undo chunks a script opened and never closed, so later edits don't keep landing in its step. */
struct ChunkGuard
{
    int depth = UndoStack::get().chunkDepth();
    ~ChunkGuard()
    {
        UndoStack& undo = UndoStack::get();
        if (undo.chunkDepth() > depth)
            log::warn("python: closed {} undo chunk(s) the script left open", undo.chunkDepth() - depth);
        while (undo.chunkDepth() > depth)
            undo.closeChunk();
    }
};
} // namespace

UiHooks& uiHooks()
{
    static UiHooks h;
    return h;
}

Gil::Gil() : mState(int(PyGILState_Ensure())) {}
Gil::~Gil() { PyGILState_Release(PyGILState_STATE(mState)); }

bool init(const std::vector<std::string>& argv)
{
    LILA_ZONE();

    if (gInitialized)
        return true;
    PyImport_AppendInittab("_lila", PyInit__lila);

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.install_signal_handlers = 0;
    config.user_site_directory = 1;
    config.use_environment = 0;
    config.write_bytecode = 1;

    // An installed build carries its own interpreter; dev builds use the one they were configured against.
    std::filesystem::path home = paths::executableDir() / "python-runtime";
    if (!std::filesystem::exists(home / "Lib"))
        home = LILA_PYTHON_HOME;
    if (const char* env = std::getenv("LILA_PYTHON_HOME"); env && *env)
        home = env;
    PyConfig_SetString(&config, &config.home, widen(home).c_str());

    std::vector<std::wstring> wargs;
    for (const std::string& a : argv)
        wargs.emplace_back(a.begin(), a.end());
    std::vector<wchar_t*> ptrs;
    for (auto& w : wargs)
        ptrs.push_back(w.data());
    PyConfig_SetArgv(&config, Py_ssize_t(ptrs.size()), ptrs.data());

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status))
    {
        log::error("python: failed to start ({})", status.err_msg ? status.err_msg : "unknown");
        return false;
    }
    gInitialized = true;
    crash::setContext("app", "python", Py_GetVersion());
    if (std::filesystem::path tracebacks = crash::pythonTracebackFile(); !tracebacks.empty())
    {
        // faulthandler writes every Python thread's traceback on a fatal fault, into the crash report.
        std::u8string u8 = tracebacks.generic_u8string();
        std::string code = "import faulthandler as _fh\n_fh.enable(file=open(r'''" + std::string(u8.begin(), u8.end()) +
                           "''', 'w', encoding='utf-8'), all_threads=True)\ndel _fh\n";
        PyRun_SimpleString(code.c_str());
    }

    // nanobind's internals are created by the first NB_MODULE init; no nb:: calls work before this.
    if (PyObject* core = PyImport_ImportModule("_lila"))
        Py_DECREF(core);
    else
        PyErr_Print();

    bool ok = true;
    {
        nb::gil_scoped_acquire gil;
        try
        {
            nb::module_ sys = nb::module_::import_("sys");
            sys.attr("path").attr("insert")(0, paths::pythonDir().string());
            nb::module_::import_("lila._startup").attr("boot")();
        }
        catch (const nb::python_error& e)
        {
            log::error("python: startup failed\n{}", e.what());
            ok = false;
        }
    }
    gMainState = PyEval_SaveThread();
    return ok;
}

void shutdown()
{
    if (!gInitialized)
        return;
    PyEval_RestoreThread(gMainState);
    Py_FinalizeEx();
    gInitialized = false;
}

bool initialized()
{
    return gInitialized;
}

Result run(std::string_view code, std::string_view filename)
{
    LILA_ZONE();

    if (!gInitialized)
        return {false, "python not initialized"};
    ChunkGuard chunks;
    nb::gil_scoped_acquire gil;
    try
    {
        auto [ok, value] = nb::cast<std::tuple<bool, std::string>>(
            nb::module_::import_("lila._runner").attr("run")(nb::str(code.data(), code.size()), nb::str(filename.data(), filename.size())));
        return {ok, value};
    }
    catch (const nb::python_error& e)
    {
        log::error("{}", e.what());
        return {false, e.what()};
    }
}

Result runFile(const std::filesystem::path& path)
{
    if (!gInitialized)
        return {false, "python not initialized"};
    ChunkGuard chunks;
    nb::gil_scoped_acquire gil;
    try
    {
        auto [ok, value] = nb::cast<std::tuple<bool, std::string>>(
            nb::module_::import_("lila._runner").attr("run_file")(path.string()));
        return {ok, value};
    }
    catch (const nb::python_error& e)
    {
        log::error("{}", e.what());
        return {false, e.what()};
    }
}
} // namespace lila::python
