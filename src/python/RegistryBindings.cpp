#include "core/ExtensionContext.h"
#include "extensions/ExtensionManager.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "link/Exporters.h"
#include "mcp/McpServer.h"
#include "python/Python.h"
#include "stream/ChangeStream.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <memory>

namespace nb = nanobind;
using lila::json;

namespace
{
/** Owns a reference without needing the GIL to copy; the last release takes it. */
std::shared_ptr<PyObject> hold(nb::handle h)
{
    return std::shared_ptr<PyObject>(h.inc_ref().ptr(), [](PyObject* o) {
        if (!Py_IsInitialized())
            return;
        lila::python::Gil gil;
        Py_DECREF(o);
    });
}

/** The context to register under: the extension's when owner names one, else null for a plain registration. */
lila::ext::Context* contextFor(const std::string& owner)
{
    if (owner.empty())
        return nullptr;
    lila::ext::Context* ctx = lila::extensions::contextOf(owner);
    if (!ctx)
        throw nb::value_error(("no extension '" + owner + "' is loaded or loading").c_str());
    return ctx;
}

json parse(const std::string& text, const char* what)
{
    if (text.empty())
        return json::object();
    try
    {
        return json::parse(text);
    }
    catch (const std::exception& e)
    {
        throw nb::value_error((std::string(what) + ": " + e.what()).c_str());
    }
}

/** Calls fn with the GIL held; a Python exception becomes a std::runtime_error carrying its message. */
template <class F> auto withGil(const char* what, F&& f)
{
    lila::python::Gil gil;
    try
    {
        return f();
    }
    catch (nb::python_error& e)
    {
        std::string msg = nb::cast<std::string>(nb::str(e.value()));
        throw lila::CommandError(std::string(what) + ": " + msg);
    }
}

lila::io::translators::Operation operation(const std::string& command, const std::string& flags)
{
    lila::io::translators::Operation o;
    o.command = command;
    if (!command.empty())
        o.flags = parse(flags, "translator flags");
    return o;
}
} // namespace

namespace lila::python
{
/**
 * _lila.registries: the scene-side registries lila/extension.py wraps (file translators, MCP tools, link exporters,
 * scene ui sections), and the change stream lila/stream.py wraps. Values cross as JSON text. With an owner (a loaded or loading extension) a registration is that
 * extension's and goes when it unloads.
 */
void bindRegistries(nb::module_& m)
{
    nb::module_ r = m.def_submodule("registries", "Scene-side registries for Python extensions. Use lila.extension instead.");

    r.def(
        "register_file_translator",
        [](const std::string& owner, const std::string& id, const std::string& label, const std::string& doc, const std::vector<std::string>& extensions,
           const std::string& openCommand, const std::string& openFlags, const std::string& importCommand, const std::string& importFlags,
           const std::string& exportCommand, const std::string& exportFlags, int priority) {
            io::translators::FileTranslator t{.id = id,
                                              .label = label,
                                              .doc = doc,
                                              .extensions = extensions,
                                              .open = operation(openCommand, openFlags),
                                              .import = operation(importCommand, importFlags),
                                              .exportSelection = operation(exportCommand, exportFlags),
                                              .priority = priority};
            if (ext::Context* ctx = contextFor(owner))
            {
                if (!io::translators::add(*ctx, std::move(t)))
                    throw nb::value_error(("file translator '" + id + "' is already registered").c_str());
                return;
            }
            try
            {
                io::translators::add(std::move(t));
            }
            catch (const std::invalid_argument& e)
            {
                throw nb::value_error(e.what());
            }
        },
        nb::arg("owner"), nb::arg("id"), nb::arg("label"), nb::arg("doc"), nb::arg("extensions"), nb::arg("open_command"),
        nb::arg("open_flags"), nb::arg("import_command"), nb::arg("import_flags"), nb::arg("export_command"), nb::arg("export_flags"),
        nb::arg("priority"));
    r.def("unregister_file_translator", [](const std::string& id) { return io::translators::remove(id); });
    r.def("file_translators", [] {
        json out = json::array();
        for (const io::translators::FileTranslator* t : io::translators::registry().all())
            out.push_back(io::translators::describe(*t));
        return out.dump();
    });

    r.def(
        "register_mcp_tool",
        [](const std::string& owner, const std::string& name, const std::string& description, const std::string& schema, nb::callable fn) {
            mcp::Tool tool{.name = name, .description = description, .inputSchema = parse(schema, "input schema")};
            if (tool.inputSchema.empty())
                tool.inputSchema = {{"type", "object"}, {"properties", json::object()}};
            tool.run = [fn = hold(fn), name](const json& args) {
                return withGil(name.c_str(), [&] { return nb::cast<std::string>(nb::str(nb::handle(fn.get())(args.dump()))); });
            };
            if (ext::Context* ctx = contextFor(owner))
            {
                if (!mcp::Server::get().addTool(*ctx, std::move(tool)))
                    throw nb::value_error(("MCP tool '" + name + "' is already registered").c_str());
                return;
            }
            mcp::Server::get().addTool(std::move(tool));
        },
        nb::arg("owner"), nb::arg("name"), nb::arg("description"), nb::arg("schema"), nb::arg("fn"));
    r.def("unregister_mcp_tool", [](const std::string& name) { return mcp::Server::get().removeTool(name); });

    r.def(
        "register_link_exporter",
        [](const std::string& owner, const std::string& format, nb::callable fn) {
            link::ExportFn run = [fn = hold(fn), format](const std::vector<scene::NodeId>& nodes, const std::filesystem::path& path, std::string* err) {
                try
                {
                    return withGil(format.c_str(), [&] {
                        std::u8string u8 = path.generic_u8string();
                        return nb::cast<bool>(nb::handle(fn.get())(nodes, std::string(u8.begin(), u8.end())));
                    });
                }
                catch (const std::exception& e)
                {
                    if (err)
                        *err = e.what();
                    return false;
                }
            };
            if (ext::Context* ctx = contextFor(owner))
            {
                if (!link::registerExporter(*ctx, format, std::move(run)))
                    throw nb::value_error(("link exporter '" + format + "' is already registered").c_str());
                return;
            }
            link::registerExporter(format, std::move(run));
        },
        nb::arg("owner"), nb::arg("format"), nb::arg("fn"));
    r.def("unregister_link_exporter", [](const std::string& format) { link::unregisterExporter(format); });

    r.def(
        "register_scene_ui_section",
        [](const std::string& owner, const std::string& key, nb::callable save, nb::callable load) {
            auto saveFn = [save = hold(save), key]() -> json {
                return withGil(key.c_str(), [&] {
                    nb::object text = nb::handle(save.get())();
                    return text.is_none() ? json(nullptr) : json::parse(nb::cast<std::string>(text));
                });
            };
            auto loadFn = [load = hold(load), key](const json& value) {
                withGil(key.c_str(), [&] {
                    nb::handle(load.get())(value.dump());
                    return 0;
                });
            };
            if (ext::Context* ctx = contextFor(owner))
            {
                if (!io::registerUiSection(*ctx, key, std::move(saveFn), std::move(loadFn)))
                    throw nb::value_error(("scene ui section '" + key + "' is already registered").c_str());
                return;
            }
            io::registerUiSection(key, std::move(saveFn), std::move(loadFn));
        },
        nb::arg("owner"), nb::arg("key"), nb::arg("save"), nb::arg("load"));
    r.def("unregister_scene_ui_section", [](const std::string& key) { return io::unregisterUiSection(key); });

    auto filter = [](const std::string& text) {
        try
        {
            return stream::Filter::fromJson(parse(text, "filter"));
        }
        catch (const std::invalid_argument& e)
        {
            throw nb::value_error(e.what());
        }
    };
    r.def(
        "stream_subscribe",
        [filter](const std::string& owner, nb::callable fn, const std::string& filterJson) {
            stream::Handler handler = [fn = hold(fn)](const json& message) {
                withGil("change stream", [&] {
                    nb::handle(fn.get())(message.dump(-1, ' ', false, json::error_handler_t::replace));
                    return 0;
                });
            };
            if (ext::Context* ctx = contextFor(owner))
                return stream::subscribe(*ctx, std::move(handler), filter(filterJson));
            return stream::subscribe(std::move(handler), filter(filterJson));
        },
        nb::arg("owner"), nb::arg("fn"), nb::arg("filter"));
    r.def("stream_unsubscribe", [](uint64_t id) { return stream::unsubscribe(id); });
    r.def("stream_snapshot", [filter](const std::string& filterJson) {
        return stream::snapshot(filter(filterJson)).dump(-1, ' ', false, json::error_handler_t::replace);
    });
}
} // namespace lila::python
