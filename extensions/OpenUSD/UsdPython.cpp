// Live stages for Python: lila.usd.stage() hands a script the UsdStage a usdStage node composes, through pxr's own
// bindings, so the script and the node share one stage and one set of Tf and Sdf registries.

#include "UsdBackend.h"
#include "UsdCommon.h"
#include "UsdStage.h"

#include <pxr/base/tf/pyLock.h>
#include <pxr/external/boost/python/object.hpp>
#include <pxr/usd/usd/stage.h>

#include <exception>

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
/** With Python running, Tf imports the bindings of every library it loads, so they have to be importable from then on. */
void preparePython(const fs::path& root)
{
    if (!Py_IsInitialized())
        return;
    const fs::path py = root / "python";
    std::error_code ec;
    if (!fs::is_directory(py / "pxr", ec))
        return;
#if defined(_WIN32)
    // Bindings imported later load libraries nothing has loaded yet, from beside OpenUSDCore.
    AddDllDirectory(root.wstring().c_str());
#endif
    pxr::TfPyLock lock;
    PyObject* path = PySys_GetObject("path");
    const std::string dir = utf8(py);
    PyObject* item = PyUnicode_FromStringAndSize(dir.data(), Py_ssize_t(dir.size()));
    if (path && item && PySequence_Contains(path, item) == 0)
        PyList_Append(path, item);
    Py_XDECREF(item);
    PyErr_Clear();
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
