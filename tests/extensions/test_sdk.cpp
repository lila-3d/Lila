#include "Isolate.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/ExtensionManifest.h"
#include "core/Registry.h"
#include "scene/Node.h"

#include <doctest/doctest.h>

using namespace lila;

TEST_CASE("sdk version: major must match, minor no newer than the host")
{
    CHECK(ext::SdkVersion::parse("1.0")->majorVersion == 1);
    CHECK(ext::SdkVersion::parse("2")->str() == "2.0");
    CHECK(ext::SdkVersion::parse("1.12")->minorVersion == 12);
    CHECK_FALSE(ext::SdkVersion::parse("one"));
    CHECK_FALSE(ext::SdkVersion::parse("1.x"));
    CHECK_FALSE(ext::SdkVersion::parse(""));

    const ext::SdkVersion host{1, 3};
    CHECK(ext::sdkMismatch({1, 0}, host).empty());
    CHECK(ext::sdkMismatch({1, 3}, host).empty());
    CHECK(ext::sdkMismatch({1, 4}, host).find("newer") != std::string::npos);
    CHECK(ext::sdkMismatch({2, 0}, host).find("major") != std::string::npos);
    CHECK(ext::sdkMismatch({0, 9}, host).find("major") != std::string::npos);
    CHECK(ext::sdkMismatch(ext::kSdkVersion).empty());

    const auto packed = ext::SdkVersion::pack(3, 7);
    CHECK(ext::SdkVersion::unpack(packed).majorVersion == 3);
    CHECK(ext::SdkVersion::unpack(packed).minorVersion == 7);
}

TEST_CASE("manifest: parse, defaults and every problem at once")
{
    const std::filesystem::path root = std::filesystem::path("C:/exts") / "Rig.lilaext";
    json j = {{"id", "Rig"},
              {"name", "Rig Templates"},
              {"version", "1.2.3"},
              {"vendor", "Lila"},
              {"sdk", "1.0"},
              {"entry", {{"windows", "bin/Rig.dll"}, {"linux", "bin/libRig.so"}}},
              {"python", "rig"},
              {"dependencies", json::array({"Base", {{"id", "Picker"}, {"version", "2.1"}}})},
              {"enabledByDefault", true},
              {"license", {{"kind", "none"}}}};
    ext::Manifest m = ext::Manifest::parse(j, root);
    CHECK(m.id == "Rig");
    CHECK(m.name == "Rig Templates");
    CHECK(m.sdk.majorVersion == 1);
    CHECK(m.enabledByDefault);
    CHECK(m.resources == "resources");
    REQUIRE(m.dependencies.size() == 2);
    CHECK(m.dependencies[0].id == "Base");
    CHECK(m.dependencies[0].minVersion.empty());
    CHECK(m.dependencies[1].id == "Picker");
    CHECK(m.dependencies[1].minVersion == "2.1");
    CHECK(m.license["kind"] == "none");
#ifdef _WIN32
    REQUIRE(m.library());
    CHECK(*m.library() == (root / "bin/Rig.dll").lexically_normal());
#endif
    CHECK(*m.pythonModule() == (root / "rig").lexically_normal());
    CHECK(ext::Manifest::parse(m.toJson(), root).toJson() == m.toJson());

    // Name defaults to the id; a single entry string is this platform's.
    ext::Manifest small = ext::Manifest::parse({{"id", "s"}, {"version", "1"}, {"sdk", "1"}, {"entry", "s.dll"}}, "C:/x");
    CHECK(small.name == "s");
    CHECK(small.library());

    try
    {
        ext::Manifest::parse({{"id", "9bad"}, {"sdk", "one"}, {"entry", "../out.dll"}, {"dependencies", {"9bad"}}, {"enabledByDefault", "yes"}},
                             std::filesystem::path("C:/exts") / "Other.lilaext");
        FAIL("expected a ManifestError");
    }
    catch (const ext::ManifestError& e)
    {
        std::string what = e.what();
        CHECK(what.find("'id' must start with a letter") != std::string::npos);
        CHECK(what.find("'version' is missing") != std::string::npos);
        CHECK(what.find("'sdk' must be") != std::string::npos);
        CHECK(what.find("inside the package") != std::string::npos);
        CHECK(what.find("depend on itself") != std::string::npos);
        CHECK(what.find("'enabledByDefault' must be") != std::string::npos);
        CHECK(what.find("must be named 9bad.lilaext") != std::string::npos);
    }
    CHECK_THROWS_WITH_AS(ext::Manifest::parse({{"id", "a"}, {"version", "1"}, {"sdk", "1.0"}}, "C:/x"), doctest::Contains("give an 'entry'"),
                         ext::ManifestError);

    ext::Manifest pack = ext::Manifest::parse({{"id", "a"}, {"version", "1"}, {"sdk", "1.0"}, {"resources", "resources"}}, "C:/x");
    CHECK(pack.resourcesOnly());
    CHECK_FALSE(pack.library());
    CHECK_FALSE(pack.pythonModule());
}

TEST_CASE("manifest: dependency order and versions")
{
    std::map<std::string, std::vector<std::string>> deps = {{"a", {"b", "c"}}, {"b", {"c"}}, {"c", {}}, {"d", {"missing"}}};
    auto depsOf = [&](const std::string& id) { return deps[id]; };
    CHECK(ext::dependencyOrder({"a", "d", "b", "c"}, depsOf) == std::vector<std::string>{"c", "b", "a", "d"});

    deps["c"] = {"a"};
    std::vector<std::string> cycle;
    std::vector<std::string> order = ext::dependencyOrder({"a", "b", "c"}, depsOf, &cycle);
    CHECK(order.size() == 3);
    CHECK_FALSE(cycle.empty());

    CHECK(ext::compareVersions("1.10", "1.9") > 0);
    CHECK(ext::compareVersions("1.2", "1.2.0") == 0);
    CHECK(ext::compareVersions("0.9.1", "1") < 0);
}

TEST_CASE("registry: owner-tagged add, replace, removal and change signal")
{
    Registry<int> r;
    std::vector<std::string> log;
    int listener = r.addListener([&](RegistryChange c, const std::string& id, const std::string& owner) {
        log.push_back((c == RegistryChange::Added ? "+" : "-") + id + "@" + owner);
    });
    r.add("one", 1);
    r.add("two", 2, "ext");
    r.add("three", 3, "ext");
    CHECK_THROWS_AS(r.add("two", 22), std::invalid_argument);
    CHECK(*r.find("two") == 2);
    CHECK(*r.ownerOf("two") == "ext");
    CHECK(r.owned("ext") == std::vector<std::string>{"two", "three"});
    r.put("one", 11, "other");
    CHECK(*r.find("one") == 11);
    CHECK(r.ids() == std::vector<std::string>{"one", "three", "two"});

    log.clear();
    std::vector<std::unique_ptr<int>> taken;
    CHECK(r.removeOwned("ext", &taken) == 2);
    CHECK(log == std::vector<std::string>{"-three@ext", "-two@ext"});
    CHECK(taken.size() == 2);
    CHECK(r.size() == 1);
    r.removeListener(listener);
    CHECK(r.remove("one"));
    CHECK_FALSE(r.remove("one"));
    CHECK(log.size() == 2);
}

TEST_CASE("context: unloading removes everything the owner registered, newest first")
{
    isolate();
    const size_t commandsBefore = ops::all().size();
    const size_t typesBefore = scene::types::all().size();
    Registry<std::string> panels;
    panels.add("outliner", "built in");

    std::vector<std::string> order;
    {
        ext::Context ctx("sample", "C:/exts/sample.lilaext");
        CHECK(ctx.resources() == std::filesystem::path("C:/exts/sample.lilaext") / "resources");
        CHECK(ctx.registerCommand({.name = "sampleCmd", .doc = "Sample.", .undoable = false, .run = [](const Args&) { return json(1); }}));
        CHECK(ops::find("sampleCmd")->owner == "sample");
        // Lila's own command can't be taken over.
        CHECK_FALSE(ctx.registerCommand({.name = "undo", .run = [](const Args&) { return json(); }}));
        CHECK(ops::find("undo")->owner.empty());

        scene::NodeType t;
        t.name = "sampleNode";
        t.doc = "Sample.";
        t.create = [] { return std::unique_ptr<scene::Node>(); };
        REQUIRE(scene::types::registerType(ctx, std::move(t)));
        CHECK(scene::types::find("sampleNode")->owner == "sample");

        ctx.add(panels, "panel", "samplePanel", std::string("from sample"));
        CHECK(*panels.ownerOf("samplePanel") == "sample");

        int heard = 0;
        ctx.subscribe(events::Event::SceneSaved, [&](const events::EventData&) { ++heard; });
        events::send({.type = events::Event::SceneSaved});
        CHECK(heard == 1);

        ctx.track("custom:first", [&] { order.push_back("first"); });
        ext::Context::Handle second = ctx.track("custom:second", [&] { order.push_back("second"); });
        ctx.track("custom:third", [&] { order.push_back("third"); });
        CHECK(ctx.remove(second));
        CHECK_FALSE(ctx.remove(second));
        CHECK(order == std::vector<std::string>{"second"});

        CHECK(ctx.registrations() == std::vector<std::string>{"command:sampleCmd", "nodeType:sampleNode", "panel:samplePanel", "event:sceneSaved",
                                                               "custom:first", "custom:third"});
        ctx.unloadAll();
        CHECK(ctx.registrationCount() == 0);
        CHECK(order == std::vector<std::string>{"second", "third", "first"});

        events::send({.type = events::Event::SceneSaved});
        CHECK(heard == 1);
    }
    CHECK_FALSE(ops::find("sampleCmd"));
    CHECK_FALSE(scene::types::find("sampleNode"));
    CHECK_FALSE(panels.contains("samplePanel"));
    CHECK(panels.contains("outliner"));
    CHECK(ops::all().size() == commandsBefore);
    CHECK(scene::types::all().size() == typesBefore);
    CHECK(events::subscriptionCount("sample") == 0);
}

TEST_CASE("context: removal leaves a name someone else took over alone")
{
    isolate();
    ext::Context ctx("first");
    REQUIRE(ctx.registerCommand({.name = "sharedName", .undoable = false, .run = [](const Args&) { return json(1); }}));
    // Replaced from outside the context (Python reloading a script, say): unloading first must not remove it.
    ops::registerCommand({.name = "sharedName", .undoable = false, .run = [](const Args&) { return json(2); }, .owner = "second"});
    ctx.unloadAll();
    REQUIRE(ops::find("sharedName"));
    CHECK(ops::find("sharedName")->owner == "second");
    ops::unregisterCommand("sharedName");
}

TEST_CASE("events: subscribe, emit, unsubscribe and owner removal")
{
    using events::Event;
    std::vector<std::string> heard;
    CHECK(events::subscribe(Event::Count, [](const events::EventData&) {}) == 0);
    CHECK(events::subscribe(Event::Undo, nullptr) == 0);

    events::SubscriptionId a = events::subscribe(Event::Undo, [&](const events::EventData& e) { heard.push_back("a:" + e.name); });
    events::SubscriptionId b = events::subscribe(Event::Undo, [&](const events::EventData& e) { heard.push_back("b:" + e.name); }, "ext");
    events::subscribe(Event::Redo, [&](const events::EventData&) { heard.push_back("redo"); }, "ext");
    CHECK(events::hasSubscribers(Event::Undo));
    CHECK(events::subscriberCount(Event::Undo) == 2);
    CHECK(events::subscriptionCount("ext") == 2);
    CHECK_FALSE(events::hasSubscribers(Event::NodeRenamed));

    events::send({.type = Event::Undo, .name = "move"});
    CHECK(heard == std::vector<std::string>{"a:move", "b:move"});

    CHECK(events::unsubscribe(b));
    CHECK_FALSE(events::unsubscribe(b));
    heard.clear();
    events::send({.type = Event::Undo, .name = "x"});
    CHECK(heard == std::vector<std::string>{"a:x"});

    CHECK(events::unsubscribeOwned("ext") == 1);
    heard.clear();
    events::send({.type = Event::Redo});
    CHECK(heard.empty());
    CHECK_FALSE(events::hasSubscribers(Event::Redo));

    // A handler that unsubscribes a later one stops it hearing this same event; one that throws doesn't stop the rest.
    events::SubscriptionId later = 0;
    events::subscribe(Event::LayoutChanged, [&](const events::EventData&) {
        events::unsubscribe(later);
        throw std::runtime_error("handler failure");
    }, "ext2");
    later = events::subscribe(Event::LayoutChanged, [&](const events::EventData&) { heard.push_back("later"); }, "ext2");
    events::subscribe(Event::LayoutChanged, [&](const events::EventData&) { heard.push_back("last"); }, "ext2");
    events::send({.type = Event::LayoutChanged});
    CHECK(heard == std::vector<std::string>{"last"});
    events::unsubscribeOwned("ext2");
    events::unsubscribe(a);

    CHECK(std::string(events::name(Event::SceneBeforeSave)) == "sceneBeforeSave");
    CHECK(events::fromName("shutdown") == Event::Shutdown);
    CHECK_FALSE(events::fromName("nope"));
}
