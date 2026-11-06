#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "python/Convert.h"
#include "python/Python.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <memory>
#include <set>

namespace nb = nanobind;
using lila::json;
using lila::python::hold;
using lila::python::toJson;
using lila::python::toPy;

namespace
{
lila::ArgType argType(const std::string& s)
{
    using T = lila::ArgType;
    if (s == "bool")
        return T::Bool;
    if (s == "int")
        return T::Int;
    if (s == "float")
        return T::Float;
    if (s == "str" || s == "string")
        return T::String;
    if (s == "vec3")
        return T::Vec3;
    if (s == "list")
        return T::List;
    if (s == "any" || s.empty())
        return T::Any;
    throw nb::value_error(("unknown flag type '" + s + "' (bool, int, float, str, vec3, list, any)").c_str());
}

std::string str(nb::handle d, const char* key)
{
    nb::dict dict = nb::borrow<nb::dict>(d);
    if (!dict.contains(key))
        return {};
    nb::object v = dict[key];
    return v.is_none() ? std::string() : nb::cast<std::string>(nb::str(v));
}

json callPython(const std::shared_ptr<PyObject>& fn, const std::string& name, const lila::Args& a)
{
    LILA_ZONE();

    lila::python::Gil gil;
    try
    {
        nb::list pos;
        for (const json& v : a.positional)
            pos.append(toPy(v));
        nb::dict kw;
        for (auto& [k, v] : a.flags.items())
            kw[k.c_str()] = toPy(v);
        nb::object r = nb::handle(fn.get())(*pos, **kw);
        return toJson(r);
    }
    catch (nb::python_error& e)
    {
        std::string type = nb::cast<std::string>(nb::handle(e.type()).attr("__name__"));
        std::string msg = nb::cast<std::string>(nb::str(e.value()));
        // CommandError is the extension saying no; anything else is a bug the author needs the traceback for.
        if (type != "CommandError")
            lila::log::error("{}: {}", name, e.what());
        throw lila::CommandError(name + ": " + (msg.empty() ? type : msg));
    }
}
} // namespace

namespace lila::python
{
/** _lila.extension: what lila/extension.py builds on. */
void bindExtensions(nb::module_& m)
{
    nb::module_ p = m.def_submodule("extension", "Command registration for Python extensions. Use lila.extension instead.");

    p.def(
        "register_command",
        [](const std::string& name, nb::callable fn, const std::string& doc, const std::string& positional, nb::list flags,
           bool undoable, const std::string& owner, nb::list examples) {
            if (name.empty())
                throw nb::value_error("command name is empty");
            // Re-registering a Python command (a reloaded script) is fine; replacing anything else is not.
            static std::set<std::string> fromPython;
            if (const CommandDef* existing = ops::find(name); existing && (!fromPython.contains(name) || existing->owner != owner))
                throw nb::value_error(("command '" + name + "' already exists" +
                                       (existing->owner.empty() ? std::string() : " (from extension " + existing->owner + ")"))
                                          .c_str());
            CommandDef def;
            def.name = name;
            def.doc = doc;
            def.positionalDoc = positional;
            def.undoable = undoable;
            def.owner = owner;
            for (nb::handle e : examples)
                def.examples.push_back(nb::cast<std::string>(nb::str(e)));
            for (nb::handle f : flags)
            {
                if (!nb::isinstance<nb::dict>(f))
                    throw nb::type_error("each flag must be a dict");
                FlagDef fd;
                fd.name = str(f, "name");
                if (fd.name.empty())
                    throw nb::value_error("flag without a name");
                fd.shortName = str(f, "short");
                fd.type = argType(str(f, "type"));
                fd.doc = str(f, "doc");
                fd.example = str(f, "example");
                fd.nodeType = str(f, "nodeType");
                nb::dict d = nb::borrow<nb::dict>(f);
                if (d.contains("default"))
                    fd.defaultValue = toJson(d["default"]);
                if (d.contains("choices") && !d["choices"].is_none())
                    for (nb::handle c : d["choices"])
                        fd.choices.push_back(nb::cast<std::string>(nb::str(c)));
                if (d.contains("min") && !d["min"].is_none())
                    fd.min = nb::cast<double>(d["min"]);
                if (d.contains("max") && !d["max"].is_none())
                    fd.max = nb::cast<double>(d["max"]);
                def.flags.push_back(std::move(fd));
            }
            def.run = [fn = hold(fn), name](const Args& a) { return callPython(fn, name, a); };
            def.scripted = true;
            ops::registerCommand(std::move(def));
            fromPython.insert(name);
        },
        nb::arg("name"), nb::arg("fn"), nb::arg("doc") = "", nb::arg("positional") = "", nb::arg("flags") = nb::list(),
        nb::arg("undoable") = true, nb::arg("owner") = "", nb::arg("examples") = nb::list());

    p.def("unregister_command", [](const std::string& name) {
        bool had = ops::find(name) != nullptr;
        ops::unregisterCommand(name);
        return had;
    });

    p.def("unregister_owner", [](const std::string& owner) {
        std::vector<std::string> names;
        for (const CommandDef* d : ops::all())
            if (!owner.empty() && d->owner == owner)
                names.push_back(d->name);
        for (const std::string& n : names)
            ops::unregisterCommand(n);
        return int(names.size());
    });
}
} // namespace lila::python
