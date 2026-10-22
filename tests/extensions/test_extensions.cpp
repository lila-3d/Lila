#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "extensions/ExtensionManager.h"
#include "python/Python.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>
#include <nanobind/nanobind.h>

#include <cstdlib>
#include <random>

namespace nb = nanobind;
using namespace lila;
using lila::extensions::ExtensionManager;

namespace lila::python
{
void bindExtensions(nb::module_& m);
}

// Stands in for _lila.extension until Bindings.cpp calls bindExtensions itself.
NB_MODULE(_lila_extension_test, m)
{
    lila::python::bindExtensions(m);
}

namespace
{
/** Keeps the tests off the user's real config: no userSetup.py, no prefs. */
void isolate()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    std::random_device rd;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("lila_extensions_test_" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    static std::filesystem::path cleanup = dir;
    std::atexit([] {
        std::error_code ec;
        std::filesystem::remove_all(cleanup, ec);
    });
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", dir.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", dir.c_str(), 1);
#endif
    mainq::init();
    cmds::registerBuiltins();
    scene::types::registerBuiltins();
}

json run(const char* cmd, json call = json::object())
{
    return cmds::execute(cmd, call);
}
} // namespace

TEST_CASE("native extension: load, use, unload, reload")
{
    isolate();
    extensions::registerCommands();
    ExtensionManager& pm = ExtensionManager::get();

    CHECK(run("loadExtension", {{"args", {LILA_TEST_HELLO_EXTENSION}}}) == "hello");
    CHECK(pm.isLoaded("hello"));
    const CommandDef* cmd = cmds::find("helloExtension");
    REQUIRE(cmd);
    CHECK(cmd->owner == "hello");
    CHECK(run("helloExtension", {{"args", {"Bob"}}, {"loud", true}}) == "HELLO, BOB");

    const scene::NodeType* type = scene::types::find("helloMultiply");
    REQUIRE(type);
    CHECK(type->owner == "hello");

    json info = run("extensionInfo", {{"args", {"hello"}}});
    CHECK(info["kind"] == "native");
    CHECK(info["commands"] == json::array({"helloExtension"}));
    CHECK(info["types"] == json::array({"helloMultiply"}));

    // compute() runs code from the library.
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*type, "mult");
    REQUIRE(n);
    std::string name = n->name();
    g.setValue(g.plug(name + ".input"), 3.0);
    CHECK(std::get<double>(g.value(g.plug(name + ".output"))) == doctest::Approx(6.0));

    CHECK_THROWS_WITH_AS(run("unloadExtension", {{"args", {"hello"}}}), doctest::Contains("node(s) of its types"), CommandError);
    g.deleteNode(n->id());

    run("unloadExtension", {{"args", {"hello"}}});
    CHECK_FALSE(pm.isLoaded("hello"));
    CHECK_FALSE(cmds::find("helloExtension"));
    CHECK_FALSE(scene::types::find("helloMultiply"));

    // Loading by name through a search path, then reload.
    pm.addSearchPath(std::filesystem::path(LILA_TEST_HELLO_EXTENSION).parent_path());
    json listed = run("listExtensions");
    bool found = false;
    for (const json& p : listed)
        found |= p["name"] == "hello" && p["kind"] == "native" && !p["loaded"].get<bool>();
    CHECK(found);
    CHECK(run("loadExtension", {{"args", {"hello"}}}) == "hello");
    CHECK(run("loadExtension", {{"args", {"hello"}}}) == "hello");
    CHECK(run("reloadExtension", {{"args", {"hello"}}}) == "hello");
    CHECK(cmds::find("helloExtension"));
    CHECK(run("listExtensions", {{"loaded", true}}).size() == 1);
    pm.unload("hello");
    CHECK_FALSE(cmds::find("helloExtension"));

    CHECK_THROWS_WITH_AS(run("loadExtension", {{"args", {"noSuchExtension"}}}), doctest::Contains("no extension named"), CommandError);
    CHECK_THROWS_AS(run("unloadExtension", {{"args", {"hello"}}}), CommandError);
}

TEST_CASE("python extension: load, call, errors, unload")
{
    isolate();
    PyImport_AppendInittab("_lila_extension_test", PyInit__lila_extension_test);
    REQUIRE(python::init({"lila_extensions_tests"}));
    REQUIRE(python::run("import _lila, _lila_extension_test\n"
                        "if not hasattr(_lila, 'extension'):\n"
                        "    _lila.extension = _lila_extension_test.extension\n")
                .ok);

    ExtensionManager& pm = ExtensionManager::get();
    pm.addSearchPath(LILA_TEST_EXAMPLES_DIR);

    bool listed = false;
    for (const extensions::ExtensionInfo& p : pm.available())
        listed |= p.name == "hello_py" && p.kind == extensions::ExtensionKind::Python;
    CHECK(listed);

    CHECK(pm.load("hello_py") == "hello_py");
    const CommandDef* cmd = cmds::find("helloPy");
    REQUIRE(cmd);
    CHECK(cmd->owner == "hello_py");
    CHECK(run("helloPy", {{"args", {"world"}}, {"loud", true}}) == "HELLO, WORLD");
    CHECK(run("helloPy", {{"args", {"a", "b"}}}) == "Hello, a, b");
    CHECK_THROWS_WITH_AS(run("helloPy"), doctest::Contains("give someone to greet"), CommandError);

    // Visible to scripts, as lila.cmds is refreshed on load.
    python::Result r = python::run("lila.cmds.helloPy('x')");
    CHECK(r.ok);
    CHECK(r.value == "'Hello, x'");

    pm.unload("hello_py");
    CHECK_FALSE(cmds::find("helloPy"));
    CHECK(python::run("assert not hasattr(lila.cmds, 'helloPy')").ok);

    // Direct registration, a dict flag with a short name, and a non-CommandError exception.
    REQUIRE(python::run("from lila import extension\n"
                        "def _add(a=1, b=2):\n"
                        "    return a + b\n"
                        "def _boom():\n"
                        "    raise ValueError('kaput')\n"
                        "extension.register_command('pyAdd', _add, 'Add.', flags=[{'name': 'a', 'short': 'x', 'type': int},"
                        " ('b', int, 10, 'Second.')], owner='tests')\n"
                        "extension.register_command('pyBoom', _boom)\n")
                .ok);
    CHECK(run("pyAdd", {{"x", 5}}) == 15);
    CHECK(cmds::find("pyAdd")->owner == "tests");
    CHECK_THROWS_WITH_AS(run("pyBoom"), doctest::Contains("kaput"), CommandError);
    CHECK(python::run("assert extension.unregister_command('pyAdd')").ok);
    CHECK_FALSE(cmds::find("pyAdd"));

    // An extension can't take over a built-in.
    CHECK_FALSE(python::run("extension.register_command('undo', _add)").ok);
    CHECK(cmds::find("undo")->owner.empty());

    pm.unloadAll();
    python::shutdown();
}
