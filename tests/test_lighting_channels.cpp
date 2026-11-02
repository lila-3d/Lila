#include <doctest/doctest.h>

#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/LightingChannels.h"

#include <array>

using namespace lila;
using namespace lila::scene;
namespace lc = lila::scene::lightingChannels;

namespace lightingChannelTests
{
json lcCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }
const Node& lcNode(const char* name) { return *Graph::current().find(name); }
} // namespace lightingChannelTests
using namespace lightingChannelTests;

TEST_CASE("Lighting channels: a light lights what shares a channel; names, rename, delete, undo, save")
{
    lcCmd("newScene");
    lcCmd("spaceLocator", {{"name", "Key"}});
    lcCmd("spaceLocator", {{"name", "Hero"}});
    lcCmd("spaceLocator", {{"name", "Prop"}});
    CHECK(lc::names() == std::vector<std::string>{"default"});
    CHECK(lc::lights(lcNode("Key"), lcNode("Hero")));
    CHECK(lc::mask(lcNode("Hero")) == 1u);

    lcCmd("lightingChannels", {{"args", {"Key", "Hero"}}, {"set", {"rim"}}});
    CHECK(lc::names() == std::vector<std::string>{"default", "rim"});
    CHECK(lc::lights(lcNode("Key"), lcNode("Hero")));
    CHECK_FALSE(lc::lights(lcNode("Key"), lcNode("Prop")));
    CHECK(lc::mask(lcNode("Key")) == 2u);
    lcCmd("lightingChannels", {{"args", {"Prop"}}, {"add", {"rim"}}});
    CHECK(lc::of(lcNode("Prop")) == std::vector<std::string>{"default", "rim"});
    CHECK(lc::lights(lcNode("Key"), lcNode("Prop")));
    CHECK(lcCmd("lightingChannels", {{"args", {"Hero"}}})["Hero"] == json::array({"rim"}));

    lcCmd("lightingChannel", {{"args", {"rim"}}, {"rename", "back"}});
    CHECK(lc::of(lcNode("Hero")) == std::vector<std::string>{"back"});
    CHECK_THROWS(lcCmd("lightingChannel", {{"args", {"default"}}, {"delete", true}}));
    CHECK_THROWS(lcCmd("lightingChannel", {{"args", {"back"}}, {"create", true}}));

    json doc = io::serialize();
    lcCmd("lightingChannel", {{"args", {"back"}}, {"delete", true}});
    CHECK(lc::of(lcNode("Hero")) == std::vector<std::string>{"default"});
    CHECK(lc::nodesWithChannels(Graph::current()).empty());
    lcCmd("undo");
    CHECK(lc::of(lcNode("Hero")) == std::vector<std::string>{"back"});
    lcCmd("newScene");
    io::deserialize(doc);
    CHECK(lc::of(lcNode("Key")) == std::vector<std::string>{"back"});
    CHECK(lc::names() == std::vector<std::string>{"default", "back"});
    CHECK(lc::nodesWithChannels(Graph::current()).size() == 3);
}

TEST_CASE("Light links bake into channels: one per distinct set of objects, named after the light")
{
    lcCmd("newScene");
    for (const char* n : {"KeyLight", "FillLight", "RimLight", "A", "B", "C"})
        lcCmd("spaceLocator", {{"name", n}});
    Graph& g = Graph::current();
    auto id = [&](const char* n) { return g.find(n)->id(); };
    std::vector<std::string> made = lc::bakeLinks(g, {{id("KeyLight"), {id("A"), id("B")}}, {id("FillLight"), {id("A"), id("B")}}, {id("RimLight"), {id("C")}}});
    CHECK(made == std::vector<std::string>{"KeyLight", "RimLight"});
    CHECK(lc::lights(lcNode("FillLight"), lcNode("A")));
    CHECK_FALSE(lc::lights(lcNode("RimLight"), lcNode("A")));
    CHECK(lc::lights(lcNode("RimLight"), lcNode("C")));
    CHECK_FALSE(lc::lights(lcNode("KeyLight"), lcNode("C")));
}

TEST_CASE("Lighting channels for export: what nodes carry, Unreal's three slots, the report")
{
    lcCmd("newScene");
    lcCmd("light", {{"name", "Key"}, {"skipSelect", true}});
    for (const char* n : {"A", "B", "C", "D"})
        lcCmd("spaceLocator", {{"name", n}});
    lcCmd("lightingChannels", {{"args", {"Key", "A"}}, {"set", {"default", "rim"}}});
    lcCmd("lightingChannels", {{"args", {"B"}}, {"set", {"back"}}});
    lcCmd("lightingChannels", {{"args", {"C"}}, {"set", {"top", "side"}}});
    Graph& g = Graph::current();
    std::vector<NodeId> all;
    for (const char* n : {"Key", "A", "B", "C", "D"})
        all.push_back(g.find(n)->id());

    const lc::Usage u = lc::usage(g, all);
    CHECK(u.channels == std::vector<std::string>{"rim", "back", "top", "side"});
    CHECK(u.lights == std::vector<NodeId>{g.find("Key")->id()});
    CHECK(u.objects.size() == 3);
    CHECK(lc::usage(g, {g.find("D")->id()}).empty());

    const lc::UnrealChannels un = lc::unrealChannels(u);
    CHECK(un.slots == std::array<std::string, 3>{"default", "rim", "back"});
    CHECK(un.dropped == std::vector<std::string>{"top", "side"});
    CHECK(lc::unrealBits(lcNode("Key"), un) == std::array{true, true, false});
    CHECK(lc::unrealBits(lcNode("B"), un) == std::array{false, false, true});
    CHECK(lc::unrealBits(lcNode("C"), un) == std::array{true, false, false});
    CHECK(lc::unrealBits(lcNode("D"), un) == std::array{true, false, false});

    const json r = lc::report(g, u, true, "note", &un);
    CHECK(r["channels"] == json::array({"rim", "back", "top", "side"}));
    CHECK(r["lights"] == json::array({"Key"}));
    CHECK(r["objects"].size() == 3);
    CHECK(r["written"] == true);
    CHECK(r["note"] == "note");
    CHECK(r["dropped"] == json::array({"top", "side"}));
    CHECK(lc::summary(u) == "rim, back, top, side on 1 light(s) and 3 object(s)");

    g.select({g.find("A")->id()});
    CHECK(lc::describeSelection(g) == "rim on 0 light(s) and 1 object(s) in the selection.");
    CHECK(lc::describeSelection(g, true).find("Unreal channel 0 is default, 1 rim, 2 unused.") != std::string::npos);
    g.select({g.find("D")->id()});
    CHECK(lc::describeSelection(g) == "The selection has no lighting channels.");
}
