// Lighting channels through USD: written as UsdLux light-link collections, read back from Lila's own files exactly and
// from anyone's collections as baked channels.

#include "UsdTestUtil.h"

#include "scene/LightingChannels.h"

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;
namespace lc = lila::scene::lightingChannels;

namespace
{
bool usdLcHas(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void usdLcMesh(const char* name) { addMesh(name, geo::makeCube({.width = 10, .height = 10, .depth = 10})); }
} // namespace

TEST_CASE("usd lighting channels: exported as light links, the report lists them, and they import again")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    usdLcMesh("Hero");
    usdLcMesh("Prop");
    run("light", {{"name", "Rim"}, {"skipSelect", true}});
    run("light", {{"name", "Key"}, {"skipSelect", true}});
    run("lightingChannels", {{"args", {"Rim", "Hero"}}, {"set", {"rim"}}});

    const fs::path file = tempDir() / "channels.usda";
    json report = run("exportUsd", {{"args", {file.generic_string()}}, {"selectionOnly", false}, {"report", true}});
    CHECK(report["lightingChannels"]["channels"] == json::array({"rim"}));
    CHECK(report["lightingChannels"]["lights"] == json::array({"Rim"}));
    CHECK(report["lightingChannels"]["objects"] == json::array({"Hero"}));
    CHECK(report["lightingChannels"]["written"] == true);
    const std::string text = readAll(file);
    CHECK(usdLcHas(text, "CollectionAPI:lightLink"));
    CHECK(usdLcHas(text, "collection:lightLink:includes = </channels/Hero>"));
    CHECK(usdLcHas(text, "collection:lightLink:includes = </channels/Prop>"));

    fresh();
    json imported = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    CHECK(imported["lightingChannels"] == json::array({"rim"}));
    CHECK(lc::of(node("Hero")) == std::vector<std::string>{"rim"});
    CHECK(lc::lights(node("Rim"), node("Hero")));
    CHECK_FALSE(lc::lights(node("Key"), node("Hero")));
    CHECK(lc::lights(node("Key"), node("Prop")));

    // Off, nothing is written but the report still says what was left out.
    const fs::path plain = tempDir() / "channels_off.usda";
    report = run("exportUsd", {{"args", {plain.generic_string()}}, {"selectionOnly", false}, {"lightingChannels", false}, {"report", true}});
    CHECK(report["lightingChannels"]["written"] == false);
    CHECK(report["lightingChannels"]["objects"] == json::array({"Hero"}));
    CHECK_FALSE(usdLcHas(readAll(plain), "lightLink"));
}

TEST_CASE("usd lighting channels: another app's light-link collections bake into channels")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path file = tempDir() / "linked.usda";
    const char* tri = "        int[] faceVertexCounts = [3]\n        int[] faceVertexIndices = [0, 1, 2]\n"
                      "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n";
    std::ofstream(file) << "#usda 1.0\n(\n    upAxis = \"Z\"\n    metersPerUnit = 0.01\n)\n\n"
                        << "def Xform \"World\"\n{\n"
                        << "    def Mesh \"A\"\n    {\n" << tri << "    }\n"
                        << "    def Mesh \"B\"\n    {\n" << tri << "    }\n"
                        << "    def SphereLight \"Spot\" (\n        prepend apiSchemas = [\"CollectionAPI:lightLink\"]\n    )\n    {\n"
                        << "        uniform bool collection:lightLink:includeRoot = 0\n"
                        << "        rel collection:lightLink:includes = </World/A>\n    }\n"
                        << "    def SphereLight \"Fill\"\n    {\n    }\n}\n";
    json imported = run("importUsd", {{"args", {file.generic_string()}}, {"report", true}});
    CHECK(imported["lightingChannels"] == json::array({"Spot"}));
    CHECK(lc::lights(node("Spot"), node("A")));
    CHECK_FALSE(lc::lights(node("Spot"), node("B")));
    CHECK(lc::lights(node("Fill"), node("A")));
    CHECK(lc::lights(node("Fill"), node("B")));

    fresh();
    run("importUsd", {{"args", {file.generic_string()}}, {"lightingChannels", false}});
    CHECK(lc::lights(node("Spot"), node("B")));
}
