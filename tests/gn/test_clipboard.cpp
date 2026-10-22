#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "gn/Clipboard.h"
#include "gn/Gn.h"
#include "gn/NodeLib.h"
#include "scene/Graph.h"

#include <string>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

gn::Group group(const char* name) { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), name)); }

/** Grid -> Set Position (offset from a Combine XYZ whose y is exposed as "height") -> output. */
void build(const char* name)
{
    run("gnCreateGroup", args({name}));
    std::string grid = run("gnAddNode", {{"args", {name, "MeshGrid"}}, {"position", {0, 0}}}).get<std::string>();
    std::string sp = run("gnAddNode", {{"args", {name, "SetPosition"}}, {"position", {200, 0}}}).get<std::string>();
    std::string xyz = run("gnAddNode", {{"args", {name, "CombineXYZ"}}, {"position", {0, 200}}}).get<std::string>();
    run("gnSetValue", args({name, grid + ".sizeX", 7}));
    run("gnConnect", args({name, grid + ".mesh", sp + ".geometry"}));
    run("gnConnect", args({name, xyz + ".vector", sp + ".offset"}));
    run("gnConnect", args({name, sp + ".geometry", "GroupOutput.geometry"}));
    run("gnExposeInput", args({name, xyz + ".y", "height"}));
}

std::string mustFail(const std::string& text)
{
    gn::Group g = gn::Group::makeDefault("Target");
    gn::Group before = g;
    try
    {
        gn::pasteNodesText(g, text, {0.0, 0.0});
    }
    catch (const std::invalid_argument& e)
    {
        CHECK(g.toJson() == before.toJson());
        return e.what();
    }
    FAIL("paste should have failed: " << text);
    return {};
}
} // namespace

TEST_CASE("copied nodes paste back as text with values and internal links")
{
    run("newScene");
    build("Src");
    gn::Group src = group("Src");
    std::string text = run("gnCopyNodes", args({"Src", "MeshGrid", "SetPosition", "GroupOutput"})).get<std::string>();
    json j = json::parse(text);
    CHECK(j["format"] == gn::kClipboardFormat);
    CHECK(j["version"] == gn::kClipboardVersion);
    CHECK(j["nodes"].size() == 2); // Group Output stays behind
    CHECK(j["links"].size() == 1); // only the link between copied nodes
    CHECK(!j.contains("inputs"));

    // Into the same group: names are made unique and the links follow.
    json names = run("gnPasteNodes", {{"args", {"Src", text}}, {"position", {1000, 500}}});
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "MeshGrid1");
    CHECK(names[1] == "SetPosition1");
    gn::Group after = group("Src");
    CHECK(after.nodes.size() == src.nodes.size() + 2);
    CHECK(after.node("MeshGrid1")->values["sizeX"] == 7);
    CHECK(std::find(after.links.begin(), after.links.end(), gn::GLink{"MeshGrid1", "mesh", "SetPosition1", "geometry"}) != after.links.end());
    glm::dvec2 centre = (after.node("MeshGrid1")->position + after.node("SetPosition1")->position) * 0.5;
    CHECK(centre.x == doctest::Approx(1000.0));
    CHECK(centre.y == doctest::Approx(500.0));

    // One undo removes the whole paste.
    run("undo");
    CHECK(group("Src").nodes.size() == src.nodes.size());
}

TEST_CASE("text round trip keeps every node, value and link")
{
    gn::Group g = gn::Group::makeDefault("A");
    g.nodes.push_back({"Math", "Math", {10.0, 20.0}, {{"operation", "multiply"}, {"b", 2.5}}});
    g.nodes.push_back({"Val", "Value", {-90.0, 20.0}, json::object()});
    g.links.push_back({"Val", "value", "Math", "a"});
    std::string text = gn::copyNodesText(g, {"Math", "Val"});

    gn::Group h = gn::Group::makeDefault("B");
    std::vector<std::string> names = gn::pasteNodesText(h, text, {0.0, 0.0});
    REQUIRE(names.size() == 2);
    CHECK(h.node("Math")->values == g.node("Math")->values);
    CHECK(h.node("Math")->position.x - h.node("Val")->position.x == doctest::Approx(100.0));
    CHECK(std::find(h.links.begin(), h.links.end(), gn::GLink{"Val", "value", "Math", "a"}) != h.links.end());
    // Copying the paste gives the same text back.
    CHECK(gn::copyNodesText(h, names) == text);
}

TEST_CASE("pasting a group input brings the parameters it feeds")
{
    run("newScene");
    build("Src");
    std::string text = run("gnCopyNodes", args({"Src", "GroupInput", "CombineXYZ"})).get<std::string>();
    CHECK(json::parse(text)["inputs"][0]["name"] == "height");

    run("gnCreateGroup", args({"Dst"}));
    run("gnPasteNodes", args({"Dst", text}));
    gn::Group dst = group("Dst");
    REQUIRE(dst.input("height"));
    CHECK(dst.input("height")->type == gn::SocketType::Float);
    CHECK(std::find(dst.links.begin(), dst.links.end(), gn::GLink{"GroupInput1", "height", "CombineXYZ", "y"}) != dst.links.end());
}

TEST_CASE("paste takes text from a forum post and a saved group")
{
    gn::Group g = gn::Group::makeDefault("A");
    g.nodes.push_back({"Val", "Value", {0.0, 0.0}, json::object()});
    std::string post = "Try this setup:\n```json\n" + gn::copyNodesText(g, {"Val"}) + "\n```\nworks for me";
    gn::Group h = gn::Group::makeDefault("B");
    CHECK(gn::pasteNodesText(h, post, {0.0, 0.0}) == std::vector<std::string>{"Val"});

    // A whole saved group: its Group Output is skipped with its links.
    gn::Group saved = gn::Group::makeDefault("Saved");
    saved.nodes.push_back({"Grid", "MeshGrid", {0.0, 0.0}, json::object()});
    saved.links = {{"Grid", "mesh", "GroupOutput", "geometry"}};
    gn::Group k = gn::Group::makeDefault("C");
    std::vector<std::string> names = gn::pasteNodesText(k, saved.toJson().dump(), {0.0, 0.0});
    CHECK(names == std::vector<std::string>{"GroupInput1", "Grid"});
    CHECK(k.links.size() == 1);
}

TEST_CASE("bad paste text is rejected with a reason and changes nothing")
{
    const std::string head = R"({"format": "lila.geometryNodes", "version": 1, )";
    CHECK(mustFail("hello there").find("no node data") != std::string::npos);
    CHECK(mustFail("{ not json }").find("valid JSON") != std::string::npos);
    CHECK(mustFail(R"({"format": "blender.nodes", "nodes": []})").find("not Lila") != std::string::npos);
    CHECK(mustFail(R"({"nodes": []})").find("header") != std::string::npos);
    CHECK(mustFail(R"({"format": "lila.geometryNodes", "version": 99, "nodes": []})").find("newer") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": []})").find("no nodes") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "X", "type": "NoSuchNode"}]})").find("unknown type 'NoSuchNode'") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "1bad", "type": "Value"}]})").find("valid node name") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "V", "type": "Value"}, {"name": "V", "type": "Value"}]})").find("two nodes") !=
          std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "M", "type": "Math", "values": {"operation": "explode"}}]})").find("explode") !=
          std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "M", "type": "Math", "values": {"a": "text"}}]})").find("takes a float") !=
          std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "M", "type": "Math", "values": {"zzz": 1}}]})").find("no input or property 'zzz'") !=
          std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "V", "type": "Value", "position": "here"}]})").find("position") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "V", "type": "Value"}], "links": [["V", "value", "Ghost", "a"]]})")
              .find("isn't pasted") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "V", "type": "Value"}, {"name": "M", "type": "Math"}], "links": [["V", "nope", "M", "a"]]})")
              .find("no output 'nope'") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "G", "type": "MeshGrid"}, {"name": "M", "type": "Math"}], "links": [["G", "mesh", "M", "a"]]})")
              .find("can't link") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "A", "type": "Math"}, {"name": "B", "type": "Math"}],
                             "links": [["A", "value", "B", "a"], ["B", "value", "A", "a"]]})")
              .find("cycle") != std::string::npos);
    CHECK(mustFail(head + R"("nodes": [{"name": "V", "type": "Value"}], "inputs": [{"name": "inMesh", "type": "float"}]})")
              .find("inMesh") != std::string::npos);

    // Every problem is listed, not just the first.
    std::string many = mustFail(head + R"("nodes": [{"name": "X", "type": "Nope1"}, {"name": "Y", "type": "Nope2"}]})");
    CHECK(many.find("Nope1") != std::string::npos);
    CHECK(many.find("Nope2") != std::string::npos);

    // Through the command it fails as a command error and leaves the group alone.
    run("newScene");
    run("gnCreateGroup", args({"G"}));
    json before = group("G").toJson();
    CHECK_THROWS_AS(run("gnPasteNodes", args({"G", "nonsense"})), CommandError);
    CHECK(group("G").toJson() == before);
}

TEST_CASE("node types filtered by a dragged socket name the socket to link")
{
    run("newScene");
    run("gnCreateGroup", args({"G"}));
    json fromGeometry = run("gnNodeTypes", {{"linkType", "geometry"}, {"group", "G"}});
    bool setPosition = false, math = false;
    for (const json& t : fromGeometry)
    {
        if (t["type"] == "SetPosition")
            setPosition = t["socket"] == "geometry";
        math |= t["type"] == "Math";
        CHECK(t["type"] != "GroupOutput");
    }
    CHECK(setPosition);
    CHECK(!math);

    // From an input: the node needs an output that can feed it.
    json intoFloat = run("gnNodeTypes", {{"linkType", "float"}, {"linkInput", true}});
    bool value = false;
    for (const json& t : intoFloat)
        if (t["type"] == "Value")
            value = t["socket"] == "value";
    CHECK(value);
    CHECK(intoFloat.size() < run("gnNodeTypes").size());

    CHECK(gn::linkable(gn::SocketType::Int, gn::SocketType::Float));
    CHECK(!gn::linkable(gn::SocketType::Geometry, gn::SocketType::Float));
    CHECK(!gn::linkable(gn::SocketType::String, gn::SocketType::Float));
}
