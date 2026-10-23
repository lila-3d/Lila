#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Progress.h"
#include "core/Workspace.h"
#include "scene/SceneData.h"
#include "python/Python.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <memory>

namespace nb = nanobind;
using lila::json;

namespace lila::python
{
void bindMath(nb::module_& m);
void bindScene(nb::module_& m);
void bindExtensions(nb::module_& m);
void bindRegistries(nb::module_& m);
}

namespace
{
json toJson(nb::handle h)
{
    if (h.is_none())
        return nullptr;
    if (nb::isinstance<nb::bool_>(h))
        return nb::cast<bool>(h);
    if (nb::isinstance<nb::int_>(h))
        return nb::cast<int64_t>(h);
    if (nb::isinstance<nb::float_>(h))
        return nb::cast<double>(h);
    if (nb::isinstance<nb::str>(h))
        return nb::cast<std::string>(h);
    if (nb::isinstance<nb::dict>(h))
    {
        json o = json::object();
        for (auto [k, v] : nb::borrow<nb::dict>(h))
            o[nb::cast<std::string>(nb::str(k))] = toJson(v);
        return o;
    }
    if (nb::isinstance<nb::list>(h) || nb::isinstance<nb::tuple>(h) || nb::hasattr(h, "__iter__"))
    {
        json a = json::array();
        for (nb::handle item : h)
            a.push_back(toJson(item));
        return a;
    }
    // Objects with a name (future node wrappers) pass by name.
    return nb::cast<std::string>(nb::str(h));
}

nb::object toPy(const json& j)
{
    switch (j.type())
    {
    case json::value_t::null: return nb::none();
    case json::value_t::boolean: return nb::bool_(j.get<bool>());
    case json::value_t::number_integer:
    case json::value_t::number_unsigned: return nb::int_(j.get<int64_t>());
    case json::value_t::number_float: return nb::float_(j.get<double>());
    case json::value_t::string: return nb::str(j.get_ref<const std::string&>().c_str());
    case json::value_t::array:
    {
        nb::list l;
        for (const json& x : j)
            l.append(toPy(x));
        return l;
    }
    case json::value_t::object:
    {
        nb::dict d;
        for (auto& [k, v] : j.items())
            d[k.c_str()] = toPy(v);
        return d;
    }
    default: return nb::none();
    }
}

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
    lila::python::bindScene(m);
    lila::python::bindExtensions(m);
    lila::python::bindRegistries(m);

    static nb::exception<lila::CommandError> commandError(m, "CommandError", PyExc_RuntimeError);

    m.def("execute", [](const std::string& name, nb::dict call, bool echo) {
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
                flags.append(fd);
            }
            nb::dict cd;
            cd["name"] = d->name;
            cd["doc"] = d->doc;
            cd["positional"] = d->positionalDoc;
            cd["flags"] = flags;
            cd["undoable"] = d->undoable;
            cd["owner"] = d->owner;
            out.append(cd);
        }
        return out;
    });

    m.def("write", [](int level, const std::string& text) { lila::log::write(lila::LogLevel(level), text); });

    // A scope only while the with-block runs, so Python's garbage collection can't unbalance the nesting.
    struct PyProgress
    {
        std::string title;
        double total = 0.0;
        bool cancellable = false;
        std::unique_ptr<lila::progress::Scope> scope;
    };
    nb::class_<PyProgress>(m, "Progress",
                           "Progress of a long script, shown like a scene load: with lila.Progress(\"Baking\", total=n) as p: p.step()")
        .def(
            "__init__",
            [](PyProgress* p, std::string title, double total, bool cancellable) {
                new (p) PyProgress{std::move(title), total, cancellable, nullptr};
            },
            nb::arg("title"), nb::arg("total") = 0.0, nb::arg("cancellable") = false)
        .def(
            "__enter__",
            [](PyProgress& p) -> PyProgress& {
                p.scope = std::make_unique<lila::progress::Scope>(p.title, p.total, p.cancellable ? lila::progress::Cancellable : lila::progress::None);
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
