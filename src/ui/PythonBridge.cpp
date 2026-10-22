// Python first: its headers use `slots` as a member name, which Qt defines as a macro.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "ui/PythonBridge.h"

#include "core/Profile.h"
#include "python/Python.h"

#include <cstdlib>

namespace lila::ui::pybridge
{
namespace
{
/** Calls module.fn() and returns its str result; on failure prints the traceback and returns empty. */
std::string callForString(const char* module, const char* fn)
{
    python::Gil gil;
    std::string out;
    PyObject* mod = PyImport_ImportModule(module);
    PyObject* result = mod ? PyObject_CallMethod(mod, fn, nullptr) : nullptr;
    if (result && PyUnicode_Check(result))
    {
        Py_ssize_t size = 0;
        if (const char* text = PyUnicode_AsUTF8AndSize(result, &size))
            out.assign(text, size_t(size));
    }
    if (PyErr_Occurred())
        PyErr_Print();
    Py_XDECREF(result);
    Py_XDECREF(mod);
    return out;
}
} // namespace

std::string apiDocJson()
{
    LILA_ZONE();

    if (!python::initialized())
        return {};
    return callForString("lila._apidoc", "collect_json");
}

std::filesystem::path pythonHome()
{
    if (python::initialized())
    {
        python::Gil gil;
        if (PyObject* prefix = PySys_GetObject("base_prefix"); prefix && PyUnicode_Check(prefix))
            if (const char* text = PyUnicode_AsUTF8(prefix))
                return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text)));
        PyErr_Clear();
    }
    if (const char* env = std::getenv("LILA_PYTHON_HOME"); env && *env)
        return env;
    return {};
}
} // namespace lila::ui::pybridge
