// Python first: its headers use `slots` as a member name, which Qt defines as a macro.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "ui/editor/Interpreter.h"

#include "core/Profile.h"
#include "python/Python.h"
#include "ui/editor/TextUtil.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFileInfo>

namespace lila::ui::interp
{
namespace
{
using json = nlohmann::json;

const char* kHelper = R"py(
import builtins, inspect, json, linecache, sys
import __main__

def _resolve(expr):
    parts = expr.split(".")
    ns = __main__.__dict__
    if parts[0] in ns:
        obj = ns[parts[0]]
    elif hasattr(builtins, parts[0]):
        obj = getattr(builtins, parts[0])
    else:
        raise LookupError(expr)
    for p in parts[1:]:
        obj = inspect.getattr_static(obj, p)
        if isinstance(obj, (staticmethod, classmethod)):
            obj = obj.__func__
        elif isinstance(obj, property):
            raise LookupError(expr)
    return obj

def _kind(v):
    if inspect.ismodule(v): return 9
    if inspect.isclass(v): return 7
    if isinstance(v, property): return 10
    if inspect.isroutine(v) or isinstance(v, (staticmethod, classmethod)): return 3
    return 6

def _detail(v):
    if isinstance(v, (staticmethod, classmethod)):
        v = v.__func__
    if inspect.isroutine(v) or inspect.isclass(v):
        try:
            return str(inspect.signature(v))
        except (TypeError, ValueError):
            return "(...)"
    if inspect.ismodule(v):
        return "module"
    return type(v).__name__

def complete(expr):
    try:
        if expr:
            obj = _resolve(expr)
            items = [(n, inspect.getattr_static(obj, n, None)) for n in dir(obj)]
        else:
            items = list(__main__.__dict__.items())
    except Exception:
        return "[]"
    out = []
    for name, v in items:
        try:
            out.append([name, _kind(v), _detail(v)])
        except Exception:
            out.append([name, 6, ""])
    return json.dumps(out)

def describe(expr):
    try:
        v = _resolve(expr)
    except Exception:
        return ""
    head = expr.rsplit(".", 1)[-1]
    if inspect.isroutine(v) or inspect.isclass(v):
        head += _detail(v)
    return head + "\n\n" + (inspect.getdoc(v) or "")

def qualified_name(expr):
    try:
        v = _resolve(expr)
    except Exception:
        return ""
    if inspect.ismodule(v):
        return v.__name__
    mod, q = getattr(v, "__module__", None), getattr(v, "__qualname__", None)
    if isinstance(mod, str) and isinstance(q, str) and mod != "builtins":
        return mod + "." + q
    return expr

def has_global(name):
    return "1" if name in __main__.__dict__ or hasattr(builtins, name) else ""

def register_source(name, text):
    linecache.cache[name] = (len(text), None, text.splitlines(True), name)
    return ""

def base_python():
    for root in (getattr(sys, "_base_executable", ""), sys.base_prefix):
        if root and root.lower().endswith("python.exe"):
            return root
    return sys.base_prefix
)py";

PyObject* helperModule()
{
    static PyObject* module = nullptr;
    if (module)
        return module;
    PyObject* m = PyModule_New("_lila_editor");
    PyObject* dict = PyModule_GetDict(m);
    PyDict_SetItemString(dict, "__builtins__", PyEval_GetBuiltins());
    PyObject* r = PyRun_String(kHelper, Py_file_input, dict, dict);
    if (!r)
    {
        PyErr_Clear();
        Py_DECREF(m);
        return nullptr;
    }
    Py_DECREF(r);
    module = m;
    return module;
}

QString call(const char* fn, const QString& a, const QString& b = QString(), bool two = false)
{
    if (!python::initialized())
        return {};
    python::Gil gil;
    PyObject* m = helperModule();
    if (!m)
        return {};
    QByteArray ua = a.toUtf8();
    QByteArray ub = b.toUtf8();
    PyObject* r = two ? PyObject_CallMethod(m, fn, "s#s#", ua.constData(), Py_ssize_t(ua.size()), ub.constData(), Py_ssize_t(ub.size()))
                      : PyObject_CallMethod(m, fn, "s#", ua.constData(), Py_ssize_t(ua.size()));
    if (!r)
    {
        PyErr_Clear();
        return {};
    }
    QString out;
    if (PyUnicode_Check(r))
    {
        Py_ssize_t n = 0;
        const char* s = PyUnicode_AsUTF8AndSize(r, &n);
        if (s)
            out = QString::fromUtf8(s, n);
        else
            PyErr_Clear();
    }
    Py_DECREF(r);
    return out;
}
} // namespace

QList<CompletionEntry> complete(const QString& base)
{
    LILA_ZONE();

    QList<CompletionEntry> out;
    if (!base.isEmpty() && !text::isDottedName(base))
        return out;
    json items = json::parse(call("complete", base).toStdString(), nullptr, false);
    if (!items.is_array())
        return out;
    for (const json& it : items)
    {
        if (!it.is_array() || it.size() < 3)
            continue;
        CompletionEntry e;
        e.label = QString::fromStdString(it[0].get<std::string>());
        e.kind = CompletionKind(it[1].get<int>());
        e.detail = QString::fromStdString(it[2].get<std::string>());
        e.insertText = e.label;
        e.sortText = e.label.startsWith('_') ? "z" + e.label : "m" + e.label;
        e.fromInterpreter = true;
        out.append(e);
    }
    return out;
}

QString describe(const QString& dotted)
{
    if (!text::isDottedName(dotted))
        return {};
    return call("describe", dotted).trimmed();
}

QString qualifiedName(const QString& dotted)
{
    if (!text::isDottedName(dotted))
        return {};
    return call("qualified_name", dotted);
}

bool hasGlobal(const QString& name)
{
    return !call("has_global", name).isEmpty();
}

void registerSource(const QString& filename, const QString& text)
{
    call("register_source", filename, text, true);
}

QString basePython()
{
    QString fromEnv = qEnvironmentVariable("LILA_PYTHON_HOME");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv + "/python.exe"))
        return QDir::cleanPath(fromEnv + "/python.exe");
    QString root = call("base_python", QString());
    if (!root.isEmpty())
    {
        if (root.endsWith("python.exe", Qt::CaseInsensitive) && QFileInfo::exists(root))
            return QDir::cleanPath(root);
        for (const char* name : {"/python.exe", "/bin/python3", "/python3"})
            if (QFileInfo::exists(root + name))
                return QDir::cleanPath(root + name);
    }
#ifdef LILA_PYTHON_HOME
    for (const char* name : {"/python.exe", "/bin/python3"})
        if (QFileInfo::exists(QString(LILA_PYTHON_HOME) + name))
            return QDir::cleanPath(QString(LILA_PYTHON_HOME) + name);
#endif
    return {};
}
} // namespace lila::ui::interp
