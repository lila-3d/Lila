#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
std::vector<glm::dmat4> sampleWorld(const char* name, int frames, double fps)
{
    Graph& g = Graph::current();
    std::vector<glm::dmat4> out;
    for (int f = 0; f <= frames; ++f)
    {
        g.setTime(f / fps);
        g.evaluate();
        out.push_back(g.find(name)->as<glm::dmat4>(Transform::kWorldMatrix));
    }
    return out;
}
} // namespace

TEST_CASE("joints animated in other spaces export as their world motion")
{
    for (bool bake : {false, true})
    {
        CAPTURE(bake);
        run("newScene");
        run("autoKeyframe", {{"state", false}});
        run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
        run("joint", {{"name", "root"}});
        run("joint", {{"name", "chest"}, {"position", v3(0, 100, 0)}, {"orientation", v3(0, 0, 15)}});
        run("joint", {{"name", "hand"}, {"position", v3(30, 110, 0)}});
        run("spaceLocator", {{"name", "prop"}});
        run("setKeyframe", {{"args", {"root.tx", "root.ry", "prop.tz", "prop.rx", "hand.rz"}}, {"time", 0.0}});
        run("setKeyframe", {{"args", {"root.tx"}}, {"time", 1.0}, {"value", 50.0}});
        run("setKeyframe", {{"args", {"root.ry"}}, {"time", 1.0}, {"value", 90.0}});
        run("setKeyframe", {{"args", {"prop.tz"}}, {"time", 1.0}, {"value", -40.0}});
        run("setKeyframe", {{"args", {"prop.rx"}}, {"time", 1.0}, {"value", 45.0}});
        run("setKeyframe", {{"args", {"hand.rz"}}, {"time", 1.0}, {"value", 60.0}});
        run("addSpace", {{"args", {"hand"}}, {"space", {"world", "prop"}}});
        run("switchSpace", {{"args", {"hand"}}, {"space", "world"}, {"time", 0.25}});
        run("switchSpace", {{"args", {"hand"}}, {"space", "prop"}, {"time", 0.5}});
        run("addSpace", {{"args", {"chest"}}, {"space", "world"}});
        run("switchSpace", {{"args", {"chest"}}, {"space", "world"}, {"key", false}});
        const std::vector<glm::dmat4> hand = sampleWorld("hand", 24, 24.0);
        const std::vector<glm::dmat4> chest = sampleWorld("chest", 24, 24.0);

        fs::path dir = fs::temp_directory_path() / "lila_fbx_tests";
        fs::create_directories(dir);
        fs::path file = dir / (bake ? "spaces_baked.fbx" : "spaces.fbx");
        run("exportFbx", {{"args", {file.string(), "root"}}, {"kind", "animation"}, {"bakeAnimation", bake}});

        run("newScene");
        run("importFbx", {{"args", {file.string()}}});
        REQUIRE(Graph::current().find("hand"));
        const std::vector<glm::dmat4> h = sampleWorld("hand", 24, 24.0), c = sampleWorld("chest", 24, 24.0);
        double worst = 0.0;
        for (size_t f = 0; f < hand.size(); ++f)
            for (int k = 0; k < 4; ++k)
                worst = std::max({worst, glm::length(hand[f][k] - h[f][k]), glm::length(chest[f][k] - c[f][k])});
        MESSAGE("worst matrix column error " << worst);
        CHECK(worst < 1e-3);
    }
}
