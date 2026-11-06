#include "ops/Command.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Progress.h"
#include "core/Version.h"
#include "core/Workspace.h"
#include "scene/SceneData.h"
#include "python/Convert.h"
#include "python/Python.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <memory>

namespace nb = nanobind;
using lila::json;
using lila::python::toJson;
using lila::python::toPy;

namespace lila::python
{
void bindMath(nb::module_& m);
void bindDebugDraw(nb::module_& m);
void bindScene(nb::module_& m);
void bindMeshArrays(nb::module_& m);
void bindExtensions(nb::module_& m);
void bindRegistries(nb::module_& m);
}

namespace
{
const char* argTypeName(lila::ArgType t)
{
    using T = lila::ArgType;
    switch (t)
    {
    case T::Bool: return "bool";
    case T::Int: return "int";
    case T::Float: return "float";
    case T::String: return "str";
    case T::Vec3: return "tuple[float, float, float]";
    case T::List: return "list";
    case T::Any: return "Any";
    }
    return "Any";
}
} // namespace

NB_MODULE(_lila, m)
{
    m.doc() = "Lila's native core. Use the lila package instead.";
    // The embedded interpreter is torn down with types still referenced by the app; that isn't a leak.
    nb::set_leak_warnings(false);
    nb::module_ math = m.def_submodule("math", "UE-flavoured vector math.");
    lila::python::bindMath(math);
    lila::python::bindDebugDraw(m);
    lila::python::bindScene(m);
    lila::python::bindMeshArrays(m);
    lila::python::bindExtensions(m);
    lila::python::bindRegistries(m);

    static nb::exception<lila::CommandError> commandError(m, "CommandError", PyExc_RuntimeError);

    m.def("version", [] { return std::string(lila::version::number()); });
    m.def("build_id", [] { return std::string(lila::version::buildId()); });

    m.def("execute", [](const std::string& name, nb::dict call, bool echo) {
        if (!lila::mainq::isMainThread())
            throw lila::CommandError("lila.ops." + name + " was called from a worker thread; commands run on the main thread only "
                                                         "(lila.tasks.call_on_main runs code there)");
        json c = toJson(call);
        json result = lila::ops::execute(name, c, echo ? lila::CallSource::UI : lila::CallSource::Script);
        return toPy(result);
    }, nb::arg("name"), nb::arg("call"), nb::arg("echo") = false);

    m.def("commands", [] {
        nb::list out;
        for (const lila::CommandDef* d : lila::ops::all())
        {
            nb::list flags;
            for (const lila::FlagDef& f : d->flags)
            {
                nb::dict fd;
                fd["name"] = f.name;
                fd["short"] = f.shortName;
                fd["type"] = argTypeName(f.type);
                fd["default"] = toPy(f.defaultValue);
                fd["doc"] = f.doc;
                fd["choices"] = toPy(json(f.choices));
                fd["example"] = f.example;
                if (!f.nodeType.empty())
                    fd["nodeType"] = f.nodeType;
                if (f.min)
                    fd["min"] = *f.min;
                if (f.max)
                    fd["max"] = *f.max;
                flags.append(fd);
            }
            nb::dict cd;
            cd["name"] = d->name;
            cd["doc"] = d->doc;
            cd["positional"] = d->positionalDoc;
            cd["flags"] = flags;
            cd["undoable"] = d->undoable;
            cd["owner"] = d->owner;
            cd["examples"] = toPy(json(d->examples));
            out.append(cd);
        }
        return out;
    });

    m.def("write", [](int level, const std::string& text) { lila::log::write(lila::LogLevel(level), text); });

    m.def("is_main_thread", [] { return lila::mainq::isMainThread(); });
    m.def(
        "post_to_main",
        [](nb::callable fn) {
            // A raw reference: the queue copies and destroys its functions without the GIL.
            PyObject* f = fn.release().ptr();
            lila::mainq::post([f] {
                if (!Py_IsInitialized())
                    return;
                nb::gil_scoped_acquire gil;
                nb::object call = nb::steal(f);
                try
                {
                    call();
                }
                catch (nb::python_error& e)
                {
                    e.restore();
                    PyErr_Print();
                }
                catch (const std::exception& e)
                {
                    lila::log::error("{}", e.what());
                }
            });
        },
        nb::arg("fn"), "Calls fn() on the main thread the next time it is idle. Safe from any thread.");
    m.def(
        "drain_main",
        [] {
            if (lila::mainq::isMainThread())
                lila::mainq::drain();
        },
        "Runs what post_to_main queued, now. Main thread only; does nothing elsewhere.");

    // A scope only while the with-block runs, so Python's garbage collection can't unbalance the nesting.
    struct PyProgress
    {
        std::string title;
        double total = 0.0;
        bool cancellable = false;
        bool background = false;
        std::unique_ptr<lila::progress::Scope> scope;
    };
    nb::class_<PyProgress>(m, "Progress",
                           "Progress of a long script, shown like a scene load: with lila.Progress(\"Baking\", total=n) as p: p.step(). "
                           "background=True shows it as a small indicator instead, for work on a worker thread (lila.tasks).")
        .def(
            "__init__",
            [](PyProgress* p, std::string title, double total, bool cancellable, bool background) {
                new (p) PyProgress{std::move(title), total, cancellable, background, nullptr};
            },
            nb::arg("title"), nb::arg("total") = 0.0, nb::arg("cancellable") = false, nb::arg("background") = false)
        .def(
            "__enter__",
            [](PyProgress& p) -> PyProgress& {
                uint32_t flags = (p.cancellable ? lila::progress::Cancellable : lila::progress::None) | (p.background ? lila::progress::Silent : lila::progress::None);
                p.scope = std::make_unique<lila::progress::Scope>(p.title, p.total, flags);
                return p;
            },
            nb::rv_policy::reference)
        .def("__exit__", [](PyProgress& p, nb::args) { p.scope.reset(); })
        .def("step", [](PyProgress& p, double n) { if (p.scope) p.scope->step(n); }, nb::arg("n") = 1.0, "Advance by n units.")
        .def("set_status", [](PyProgress& p, const std::string& text) { if (p.scope) p.scope->setStatus(text); }, nb::arg("text"))
        .def_prop_ro("cancelled", [](PyProgress& p) { return p.scope && p.scope->cancelled(); }, "True once the user pressed Cancel.");

    m.def("pref", [](const std::string& file, const std::string& key, nb::handle fallback) {
        return toPy(lila::PrefStore::get().value(file, key, toJson(fallback)));
    }, nb::arg("file"), nb::arg("key"), nb::arg("default") = nb::none());

    m.def("set_pref", [](const std::string& file, const std::string& key, nb::handle value, bool workspace) {
        lila::PrefStore::get().set(file, key, toJson(value), workspace ? lila::PrefScope::Workspace : lila::PrefScope::User);
    }, nb::arg("file"), nb::arg("key"), nb::arg("value"), nb::arg("workspace") = false);

    m.def("pref_locked", [](const std::string& file, const std::string& key) { return lila::PrefStore::get().isLocked(file, key); },
          nb::arg("file"), nb::arg("key"), "True when a studio folder locks the pref.");

    m.def("scene_data", [](const std::string& key, nb::handle fallback) { return toPy(lila::scene::data::get(key, toJson(fallback))); },
          nb::arg("key"), nb::arg("default") = nb::none(), "A value saved in the scene file by set_scene_data.");

    m.def("set_scene_data", [](const std::string& key, nb::handle value) { lila::scene::data::set(key, toJson(value)); },
          nb::arg("key"), nb::arg("value").none(), "Saves any JSON-compatible value in the scene file, undoably. None removes the key.");

    m.def("workspace", []() -> nb::object {
        auto& ws = lila::Workspace::get();
        return ws.isOpen() ? nb::object(nb::str(ws.root().generic_string().c_str())) : nb::none();
    });

    m.def("run_action", [](const std::string& id) {
        auto& h = lila::python::uiHooks();
        if (!h.runAction)
            throw lila::CommandError("UI actions aren't available in batch mode");
        return h.runAction(id);
    });

    m.def("actions", [] {
        nb::list out;
        if (auto& h = lila::python::uiHooks(); h.listActions)
            for (auto& [id, label, category] : h.listActions())
                out.append(nb::make_tuple(id, label, category));
        return out;
    });

    m.attr("LOG_DEBUG") = int(lila::LogLevel::Debug);
    m.attr("LOG_INFO") = int(lila::LogLevel::Info);
    m.attr("LOG_WARNING") = int(lila::LogLevel::Warning);
    m.attr("LOG_ERROR") = int(lila::LogLevel::Error);
    m.attr("LOG_OUTPUT") = int(lila::LogLevel::Output);
}
