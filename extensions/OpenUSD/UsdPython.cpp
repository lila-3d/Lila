// Live stages for Python: lila.usd.stage() hands a script the UsdStage a usdStage node composes, through pxr's own
// bindings, so the script and the node share one stage and one set of Tf and Sdf registries.

#include "UsdBackend.h"
#include "UsdCommon.h"
#include "UsdStage.h"

#include <lila/Log.h>

#include <pxr/base/tf/pyExceptionState.h>
#include <pxr/base/tf/pyLock.h>
#include <pxr/base/tf/scriptModuleLoader.h>
#include <pxr/external/boost/python/object.hpp>
#include <pxr/usd/usd/stage.h>

#include <algorithm>
#include <exception>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace bp = PXR_BOOST_PYTHON_NAMESPACE;

namespace lila::usd
{
namespace
{
/** Imports pxr.<name>, logging why when it fails. Call with the GIL held. */
void importBindings(const std::string& name)
{
    PyObject* m = PyImport_ImportModule(("pxr." + name).c_str());
    if (m)
    {
        Py_DECREF(m);
        return;
    }
    log::warn("OpenUSD: can't import pxr.{}: {}", name, pxr::TfPyExceptionState::Fetch().GetExceptionString());
}
} // namespace

/**
 * With Python running, Tf imports the bindings of every library as it loads, taking the GIL on whatever thread loaded
 * it: a TBB worker composing a stage, while the script that started the composition holds the GIL and waits for it.
 * Importing every module of the package's pxr now, on this thread, loads each library that has bindings, so a later
 * load never has any to import.
 */
void preparePython(const fs::path& root)
{
    if (!Py_IsInitialized())
        return;
    const fs::path py = root / "python";
    std::error_code ec;
    if (!fs::is_directory(py / "pxr", ec))
        return;
#if defined(_WIN32)
    // The bindings load their libraries from beside OpenUSDCore, and a few from the module plugins.
    AddDllDirectory(root.wstring().c_str());
    AddDllDirectory((root / "plugin" / "usd").wstring().c_str());
#endif
    pxr::TfPyLock lock;
    PyObject* path = PySys_GetObject("path");
    const std::string dir = utf8(py);
    PyObject* item = PyUnicode_FromStringAndSize(dir.data(), Py_ssize_t(dir.size()));
    if (path && item && PySequence_Contains(path, item) == 0)
        PyList_Append(path, item);
    Py_XDECREF(item);
    PyErr_Clear();

    std::vector<std::string> modules;
    for (const fs::directory_entry& e : fs::directory_iterator(py / "pxr", ec))
        if (e.is_directory(ec) && fs::is_regular_file(e.path() / "__init__.py", ec))
            modules.push_back(utf8(e.path().filename()));
    std::sort(modules.begin(), modules.end());
    for (const std::string& m : modules)
        importBindings(m);
    pxr::TfScriptModuleLoader::GetInstance().LoadModules();
}
} // namespace lila::usd

USD_BACKEND_EXPORT void* lilaUsdStagePython(const char* node)
{
    pxr::UsdStagePtr stage;
    try
    {
        stage = lila::usd::stage::liveStage(node ? node : "");
    }
    catch (const std::exception& e)
    {
        PyErr_SetString(PyExc_LookupError, e.what());
        return nullptr;
    }
    try
    {
        return bp::incref(bp::object(stage).ptr());
    }
    catch (const bp::error_already_set&)
    {
        return nullptr;
    }
}
