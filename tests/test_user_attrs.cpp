#include <doctest/doctest.h>

#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Graph.h"

using namespace lila;
using namespace lila::scene;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

json getAttr(const std::string& plug) { return run("getAttr", {{"args", {plug}}}); }
} // namespace

TEST_CASE("addAttr: values and arrays on one node, undoable, saved with the scene")
{
    run("newScene");
    run("spaceLocator", {{"name", "Prop"}});
    run("spaceLocator", {{"name", "Other"}});
    CHECK(run("addAttr", {{"args", {"Prop"}}, {"longName", "mass"}, {"defaultValue", 2.5}, {"keyable", true}}) == json::array({"Prop.mass"}));
    run("addAttr", {{"args", {"Prop"}}, {"longName", "tags"}, {"dataType", "stringArray"}});
    run("addAttr", {{"args", {"Prop"}}, {"longName", "samples"}, {"dataType", "doubleArray"}});
    run("addAttr", {{"args", {"Prop"}}, {"longName", "mode"}, {"attributeType", "enum"}, {"enumName", "off:soft:hard"}});
    CHECK(getAttr("Prop.mass").get<double>() == doctest::Approx(2.5));
    CHECK(run("listAttr", {{"args", {"Prop"}}, {"userDefined", true}}) == json::array({"mass", "tags", "samples", "mode"}));
    CHECK_FALSE(run("objExists", {{"args", {"Other.mass"}}}).get<bool>());
    CHECK_THROWS(getAttr("Other.mass"));

    run("setAttr", {{"args", {"Prop.tags", json::array({"metal", "heavy"})}}});
    run("setAttr", {{"args", {"Prop.samples[4]", 1.5}}});
    run("setAttr", {{"args", {"Prop.samples[1]", 0.5}}});
    CHECK(getAttr("Prop.tags") == json::array({"metal", "heavy"}));
    CHECK(getAttr("Prop.samples") == json::array({0.5, 1.5}));
    CHECK(run("getAttr", {{"args", {"Prop.samples"}}, {"multiIndices", true}}) == json::array({1, 4}));
    run("removeMultiInstance", {{"args", {"Prop.samples[1]"}}});
    CHECK(getAttr("Prop.samples") == json::array({1.5}));
    run("undo");
    CHECK(getAttr("Prop.samples") == json::array({0.5, 1.5}));

    // Later user attrs keep their connections when an earlier one goes.
    run("connectAttr", {{"args", {"Prop.mode", "Other.visibility"}}});
    run("deleteAttr", {{"args", {"Prop.tags"}}});
    CHECK_THROWS(getAttr("Prop.tags"));
    CHECK(getAttr("Prop.samples") == json::array({0.5, 1.5}));
    Graph& g = Graph::current();
    auto src = g.source(g.plug("Other.visibility"));
    REQUIRE(src);
    CHECK(g.plugName(*src) == "Prop.mode");
    run("undo");
    CHECK(getAttr("Prop.tags") == json::array({"metal", "heavy"}));
    CHECK_THROWS(run("deleteAttr", {{"args", {"Prop.translate"}}}));

    json doc = io::serialize();
    run("newScene");
    io::deserialize(doc);
    CHECK(getAttr("Prop.mass").get<double>() == doctest::Approx(2.5));
    CHECK(getAttr("Prop.tags") == json::array({"metal", "heavy"}));
    CHECK(run("getAttr", {{"args", {"Prop.samples"}}, {"multiIndices", true}}) == json::array({1, 4}));
    CHECK(io::serialize() == doc);

    // Duplicates carry user attrs and array values.
    std::string copy = run("duplicate", {{"args", {"Prop"}}})[0];
    CHECK(getAttr(copy + ".tags") == json::array({"metal", "heavy"}));
    CHECK(getAttr(copy + ".samples") == json::array({0.5, 1.5}));
    run("delete", {{"args", {"Prop"}}});
    CHECK(getAttr(copy + ".mass").get<double>() == doctest::Approx(2.5));
}

TEST_CASE("constraints take any number of targets, and read the numbered slots of older scenes")
{
    run("newScene");
    json targets = json::array();
    for (int i = 0; i < 12; ++i)
    {
        std::string t = run("spaceLocator", {{"name", "T" + std::to_string(i)}});
        run("setAttr", {{"args", {t + ".translate", json::array({double(i), 0, 0})}}});
        targets.push_back(t);
    }
    run("spaceLocator", {{"name", "Follower"}});
    json args = targets;
    args.push_back("Follower");
    std::string c = run("pointConstraint", {{"args", args}, {"maintainOffset", false}});
    CHECK(run("getAttr", {{"args", {c + ".targets.matrix"}}, {"multiIndices", true}}).size() == 12);
    auto followerX = [] { return run("xform", {{"args", {"Follower"}}, {"q", true}, {"ws", true}, {"t", true}})[0].get<double>(); };
    CHECK(followerX() == doctest::Approx(5.5));
    for (int i = 0; i < 11; ++i)
        run("setAttr", {{"args", {c + ".targets[" + std::to_string(i) + "].weight", 0.0}}});
    CHECK(followerX() == doctest::Approx(11.0));
    // Scripts written for the numbered slots still reach the same plugs.
    run("setAttr", {{"args", {c + ".target0Weight", 1.0}}});
    CHECK(getAttr(c + ".targets[0].weight").get<double>() == doctest::Approx(1.0));
    CHECK(followerX() == doctest::Approx(5.5));

    // The same scene as an older file wrote it: target0Matrix ... connections and a target0Weight value.
    json doc = io::serialize();
    for (json& conn : doc["connections"])
        for (json& end : conn)
        {
            std::string s = end.get<std::string>();
            if (size_t at = s.find(".targets["); at != std::string::npos)
            {
                size_t close = s.find(']', at);
                std::string k = s.substr(at + 9, close - at - 9);
                end = s.substr(0, at) + ".target" + k + "Matrix";
            }
        }
    for (json& n : doc["nodes"])
        if (n.contains("attrs") && n["attrs"].contains("targets.weight"))
        {
            json weights = n["attrs"]["targets.weight"];
            n["attrs"].erase("targets.weight");
            for (auto& [k, v] : weights.items())
                n["attrs"]["target" + k + "Weight"] = v;
            n["attrs"]["targetCount"] = 12;
        }
    run("newScene");
    io::deserialize(doc);
    CHECK(run("getAttr", {{"args", {c + ".targets.matrix"}}, {"multiIndices", true}}).size() == 12);
    CHECK(getAttr(c + ".targets[3].weight").get<double>() == doctest::Approx(0.0));
    CHECK(followerX() == doctest::Approx(5.5));
}

TEST_CASE("spaces have no fixed limit")
{
    run("newScene");
    run("spaceLocator", {{"name", "Hand"}});
    for (int i = 0; i < 20; ++i)
    {
        std::string t = run("spaceLocator", {{"name", "S" + std::to_string(i)}});
        run("addSpace", {{"args", {"Hand"}}, {"space", t}});
    }
    CHECK(run("getAttr", {{"args", {"Hand.space"}}}).get<int64_t>() == 0);
    json doc = io::serialize();
    run("newScene");
    io::deserialize(doc);
    CHECK(io::serialize() == doc);
}
