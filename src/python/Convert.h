#pragma once

#include "ops/Command.h"
#include "python/Python.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <memory>
#include <string>

/** Conversions the binding files share between Python objects and Lila's JSON values. */
namespace lila::python
{
/** None, bool, int, float, str, dict and any iterable map to their JSON counterparts; anything else to its str(). */
inline json toJson(nanobind::handle h)
{
    namespace nb = nanobind;
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
    return nb::cast<std::string>(nb::str(h));
}

/** The Python value of a JSON value: None, bool, int, float, str, list or dict. */
inline nanobind::object toPy(const json& j)
{
    namespace nb = nanobind;
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

/** Owns a reference without needing the GIL to copy; the last release takes it. */
inline std::shared_ptr<PyObject> hold(nanobind::handle h)
{
    return std::shared_ptr<PyObject>(h.inc_ref().ptr(), [](PyObject* o) {
        // After shutdown the interpreter is gone and the reference with it.
        if (!Py_IsInitialized())
            return;
        Gil gil;
        Py_DECREF(o);
    });
}
} // namespace lila::python
