#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <algorithm>

using namespace lila;
namespace opt = lila::ops::options;

namespace
{
const opt::Field* field(const std::vector<opt::Field>& fs, std::string_view flag)
{
    auto it = std::find_if(fs.begin(), fs.end(), [&](const opt::Field& f) { return f.flag == flag; });
    return it == fs.end() ? nullptr : &*it;
}

json lastCall;

void registerProbe()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "optionsProbe",
        .doc = "Records its flags.",
        .positionalDoc = "objects...",
        .flags = {
            {"segments", "s", T::Int, 2, "How many."},
            {"offset", "", T::Float, 0.5, "How far."},
            {"clamp", "", T::Bool, true, "Stop at neighbours."},
            {"mode", "", T::String, "closestPoint", "closestPoint | pointIndex"},
            {"axis", "", T::String, "x", "Mirror axis.", {"x", "y", "z"}},
            {"label", "", T::String, nullptr, "Free text."},
            {"center", "", T::Vec3, json::array({0, 0, 0}), "Where."},
            {"guides", "", T::List, nullptr, "Curves."},
            {"name", "n", T::String, nullptr, "New object name."},
            {"target", "", T::Any, nullptr, "Some object."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            lastCall = a.flags;
            return nullptr;
        },
    });
}
} // namespace

TEST_CASE("option box fields come from the command's typed flags and docs")
{
    if (!ops::find("optionsProbe"))
        registerProbe();
    std::vector<opt::Field> fs = opt::fields("optionsProbe");

    REQUIRE(field(fs, "segments"));
    CHECK(field(fs, "segments")->kind == opt::FieldKind::Int);
    CHECK(field(fs, "segments")->defaultValue == 2);
    CHECK(field(fs, "offset")->kind == opt::FieldKind::Float);
    CHECK(field(fs, "clamp")->kind == opt::FieldKind::Check);
    CHECK(field(fs, "center")->kind == opt::FieldKind::Vec3);
    CHECK(field(fs, "label")->kind == opt::FieldKind::Text);
    CHECK(field(fs, "label")->defaultValue == "");

    // Choices spelled out in the doc, or declared on the flag.
    REQUIRE(field(fs, "mode")->kind == opt::FieldKind::Choice);
    CHECK(field(fs, "mode")->choices == std::vector<std::string>{"closestPoint", "pointIndex"});
    CHECK(field(fs, "mode")->choiceLabels == std::vector<std::string>{"Closest Point", "Point Index"});
    CHECK(field(fs, "axis")->choices == std::vector<std::string>{"x", "y", "z"});

    // Inputs rather than settings stay out: lists, names, untyped values.
    CHECK_FALSE(field(fs, "guides"));
    CHECK_FALSE(field(fs, "name"));
    CHECK_FALSE(field(fs, "target"));

    // An action's own call fixes some flags, so its dialog leaves them out.
    CHECK_FALSE(field(opt::fields("optionsProbe", {{"axis", "y"}}), "axis"));
    CHECK_FALSE(field(opt::fields("optionsProbe", {{"s", 4}}), "segments"));

    CHECK(opt::prettyLabel("maxDistance") == "Max Distance");
    CHECK(opt::prettyLabel("sourceUvSet") == "Source UV Set");
    CHECK(opt::prettyLabel("uvs") == "UVs");
    CHECK(opt::choicesFromDoc("world, object, component or uv.") == std::vector<std::string>{"world", "object", "component", "uv"});
    CHECK(opt::choicesFromDoc("Mirror the lookup across the source's x, y or z axis.").empty());
    CHECK(opt::choicesFromDoc("closest or normal.", "nearest").empty());
}

TEST_CASE("each choice's doc comes out of a doc describing them all")
{
    using V = std::vector<std::string>;
    CHECK(opt::choiceDocs("slerp: constant speed along the shortest arc. smooth: cubic through the keys.", V{"slerp", "smooth"}) ==
          V{"Constant speed along the shortest arc.", "Cubic through the keys."});
    CHECK(opt::choiceDocs("How each end is filled: quads (a grid joined by a ring), fan or ngon.", V{"quads", "fan", "ngon"}) ==
          V{"A grid joined by a ring.", "", ""});
    CHECK(opt::choiceDocs("linear steps each copy by offset; radial turns copies about the pivot; grid fills rows.", V{"linear", "radial", "grid"}) ==
          V{"Linear steps each copy by offset.", "Radial turns copies about the pivot.", "Grid fills rows."});
    CHECK(opt::choiceDocs("Neutral keeps colours true (the viewport's own); AgX rolls them off.", V{"neutral", "agx"}) ==
          V{"Neutral keeps colours true (the viewport's own)", "AgX rolls them off."});
    CHECK(opt::choiceDocs("Mirror across the x, y or z axis.", V{"x", "y", "z"}) == V{"", "", ""});
    CHECK(opt::choiceDocs("Where the up direction comes from.", V{"scene", "none"}) == V{"", ""});
}

TEST_CASE("option box values round-trip through prefs and fill in menu calls")
{
    if (!ops::find("optionsProbe"))
        registerProbe();
    opt::reset("optionsProbe");
    CHECK(opt::saved("optionsProbe").empty());
    CHECK(opt::values("optionsProbe")["segments"] == 2);

    opt::setValue("optionsProbe", "segments", 5.0);
    opt::setValue("optionsProbe", "mode", "pointIndex");
    opt::setValue("optionsProbe", "clamp", false);
    opt::setValue("optionsProbe", "axis", "w"); // not a choice: ignored
    opt::setValue("optionsProbe", "bogus", 1);  // not a flag: ignored
    CHECK(opt::saved("optionsProbe") == json{{"segments", 5}, {"mode", "pointIndex"}, {"clamp", false}});
    CHECK(PrefStore::get().value(opt::kPrefFile, "/optionsProbe/segments") == 5);

    // Back to the default forgets it, so a changed registry default would apply again.
    opt::setValue("optionsProbe", "clamp", true);
    CHECK_FALSE(opt::saved("optionsProbe").contains("clamp"));

    json values = opt::values("optionsProbe");
    CHECK(values["segments"] == 5);
    CHECK(values["mode"] == "pointIndex");
    CHECK(values["offset"] == 0.5);

    // Running from the menu: remembered values fill in, the action's own flags win.
    json call = opt::callWith("optionsProbe", {{"mode", "closestPoint"}});
    CHECK(call["segments"] == 5);
    CHECK(call["mode"] == "closestPoint");
    ops::execute("optionsProbe", opt::callWith("optionsProbe"));
    CHECK(lastCall["segments"] == 5);
    CHECK(lastCall["mode"] == "pointIndex");

    opt::reset("optionsProbe");
    CHECK(opt::callWith("optionsProbe") == json::object());
}

TEST_CASE("transfer attributes has a full option box")
{
    std::vector<opt::Field> fs = opt::fields("transferAttributes");
    for (const char* f : {"uvs", "normals", "colors", "positions", "skinWeights", "attributes", "sampleSpace", "searchMethod", "maxDistance", "flip",
                          "sourceUvSet", "targetUvSet"})
        CHECK_MESSAGE(field(fs, f), f);
    CHECK(fs.front().flag == "uvs");
    CHECK(field(fs, "sampleSpace")->choices == std::vector<std::string>{"world", "object", "component", "uv", "guided"});
    CHECK(field(fs, "uvs")->section == "Attributes to Transfer");
    CHECK(field(fs, "attributes")->kind == opt::FieldKind::List);
}

TEST_CASE("transfer attributes maps one uv set onto another")
{
    ops::execute("newScene", json::object());
    std::string src = ops::execute("polyPlane", {{"name", "MapSrc"}, {"generator", false}})[0];
    std::string dst = ops::execute("polyPlane", {{"name", "MapDst"}, {"generator", false}})[0];
    ops::execute("transferAttributes", {{"args", {src, dst}}, {"sampleSpace", "component"}, {"sourceUvSet", 0}, {"targetUvSet", 1}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const geo::Mesh& m = *g.find(dst)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    auto uv0 = m.cornerAttrs().get<glm::vec2>("uv0");
    auto uv1 = m.cornerAttrs().get<glm::vec2>("uv1");
    REQUIRE(!uv1.empty());
    REQUIRE(uv0.size() == uv1.size());
    CHECK(std::equal(uv0.begin(), uv0.end(), uv1.begin()));
}

TEST_CASE("a string flag with choices rejects a value outside them")
{
    if (!ops::find("optionsProbe"))
        registerProbe();
    CHECK_NOTHROW(ops::execute("optionsProbe", {{"axis", "y"}}));
    CHECK_THROWS_AS(ops::execute("optionsProbe", {{"axis", "w"}}), CommandError);
    // Free-text and doc-listed flags take anything.
    CHECK_NOTHROW(ops::execute("optionsProbe", {{"mode", "other"}, {"label", "anything"}}));
}
