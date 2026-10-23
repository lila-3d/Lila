#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/DisplayLayers.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
namespace dl = lila::scene::displayLayers;

namespace
{
json run(const std::string& name, const json& call = json::object()) { return ops::execute(name, call); }
std::string made(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }
NodeId idOf(const std::string& name) { return Graph::current().find(name)->id(); }
std::string layerName(const std::string& node)
{
    const Node* l = dl::layerOf(Graph::current(), idOf(node));
    return l ? l->name() : std::string();
}
} // namespace

TEST_CASE("display layers: membership, one layer per object, undo")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube")), c = made(run("polyCube"));
    std::string l1 = run("createDisplayLayer", {{"args", {a, b}}, {"name", "Props"}}).get<std::string>();
    CHECK(l1 == "Props");
    CHECK(layerName(a) == "Props");
    CHECK(layerName(c).empty());
    CHECK(run("displayLayer", {{"args", {l1}}})["members"] == json({a, b}));

    // Joining another layer leaves the first.
    std::string l2 = run("createDisplayLayer", {{"args", {b}}}).get<std::string>();
    CHECK(layerName(b) == l2);
    CHECK(run("displayLayer", {{"args", {l1}}})["members"] == json({a}));
    run("undo");
    CHECK(Graph::current().find(l2) == nullptr);
    CHECK(layerName(b) == "Props");

    run("editDisplayLayerMembers", {{"args", {l1, c}}});
    CHECK(layerName(c) == "Props");
    run("editDisplayLayerMembers", {{"args", {"defaultLayer", c}}});
    CHECK(layerName(c).empty());
    run("removeFromDisplayLayer", {{"args", {a}}});
    CHECK(layerName(a).empty());
    run("undo");
    CHECK(layerName(a) == "Props");

    // Renames on either side keep the membership.
    run("rename", {{"args", {a, "Crate"}}});
    run("displayLayer", {{"args", {l1}}, {"name", "Set"}});
    CHECK(layerName("Crate") == "Set");

    run("selectDisplayLayerMembers", {{"args", {"Set"}}});
    CHECK(Graph::current().selection().size() == 2);

    run("deleteDisplayLayer", {{"args", {"Set"}}});
    CHECK(layerName("Crate").empty());
    CHECK(Graph::current().find("Crate"));
    run("undo");
    CHECK(layerName("Crate") == "Set");

    std::string copy = made(run("duplicate", {{"args", {"Crate"}}}));
    CHECK(layerName(copy) == "Set");
}

TEST_CASE("display layers: visibility and hide on playback")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    std::string grp = made(run("group", {{"args", {cube}}}));
    Graph& g = Graph::current();
    std::string layer = run("createDisplayLayer", {{"args", {grp}}}).get<std::string>();
    CHECK(effectivelyVisible(g, idOf(cube)));

    run("displayLayer", {{"args", {layer}}, {"visibility", false}});
    CHECK_FALSE(effectivelyVisible(g, idOf(grp)));
    CHECK_FALSE(effectivelyVisible(g, idOf(cube)));
    CHECK(g.find(cube)->as<bool>(Transform::kVisibility));
    run("undo");
    CHECK(effectivelyVisible(g, idOf(cube)));

    run("displayLayer", {{"args", {layer}}, {"hideOnPlayback", true}});
    CHECK(effectivelyVisible(g, idOf(cube)));
    anim::play(1);
    CHECK_FALSE(effectivelyVisible(g, idOf(cube)));
    anim::stop();
    CHECK(effectivelyVisible(g, idOf(cube)));
}

TEST_CASE("display layers: template and reference are drawn but not pickable; colour")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    std::string grp = made(run("group", {{"args", {cube}}}));
    std::string other = made(run("polyCube"));
    Graph& g = Graph::current();
    std::string layer = run("createDisplayLayer", {{"args", {grp}}}).get<std::string>();
    CHECK(dl::pickable(g, idOf(cube)));
    CHECK_FALSE(dl::display(g, idOf(cube)).color);

    run("displayLayer", {{"args", {layer}}, {"displayType", "template"}});
    CHECK(dl::display(g, idOf(cube)).type == dl::DisplayType::Template);
    CHECK_FALSE(dl::pickable(g, idOf(cube)));
    CHECK(effectivelyVisible(g, idOf(cube)));
    CHECK(dl::pickable(g, idOf(other)));

    run("displayLayer", {{"args", {layer}}, {"displayType", "reference"}, {"color", {1.0, 0.0, 0.0}}});
    CHECK_FALSE(dl::pickable(g, idOf(cube)));
    REQUIRE(dl::display(g, idOf(cube)).color);
    CHECK(dl::display(g, idOf(cube)).color->x == 1.0);

    // A child in a layer of its own follows that one, not its parent's.
    run("createDisplayLayer", {{"args", {cube}}});
    CHECK(dl::pickable(g, idOf(cube)));
    CHECK_FALSE(dl::display(g, idOf(cube)).color);

    run("displayLayer", {{"args", {layer}}, {"useColor", false}});
    CHECK(run("displayLayer", {{"args", {layer}}})["color"].is_null());
    CHECK_THROWS_AS(run("displayLayer", {{"args", {layer}}, {"displayType", "ghost"}}), CommandError);
    CHECK_THROWS_AS(run("displayLayer", {{"args", {cube}}}), CommandError);
}

TEST_CASE("display layers: order")
{
    run("newScene");
    for (const char* n : {"A", "B", "C"})
        run("createDisplayLayer", {{"empty", true}, {"name", n}});
    auto names = [] {
        std::vector<std::string> out;
        for (const json& l : run("listDisplayLayers"))
            out.push_back(l["name"].get<std::string>());
        return out;
    };
    CHECK(names() == std::vector<std::string>{"A", "B", "C"});
    run("reorderDisplayLayer", {{"args", {"C"}}, {"index", 0}});
    CHECK(names() == std::vector<std::string>{"C", "A", "B"});
    run("undo");
    CHECK(names() == std::vector<std::string>{"A", "B", "C"});
    run("reorderDisplayLayer", {{"args", {"A"}}, {"index", 9}});
    CHECK(names() == std::vector<std::string>{"B", "C", "A"});
}

TEST_CASE("display layers save and open in both formats")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube"));
    run("createDisplayLayer", {{"args", {a}}, {"name", "Hidden"}});
    run("displayLayer", {{"args", {"Hidden"}}, {"visibility", false}, {"hideOnPlayback", true}});
    run("createDisplayLayer", {{"args", {b}}, {"name", "Ref"}});
    run("displayLayer", {{"args", {"Ref"}}, {"displayType", "reference"}, {"color", {0.0, 0.5, 1.0}}});
    run("reorderDisplayLayer", {{"args", {"Ref"}}, {"index", 0}});
    const json before = run("listDisplayLayers");

    auto dir = std::filesystem::temp_directory_path() / "lila_tests";
    std::filesystem::create_directories(dir);
    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = (dir / (std::string("layers") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        CHECK(run("listDisplayLayers").empty());
        run("openScene", {{"args", {path}}});
        CHECK(run("listDisplayLayers") == before);
        CHECK_FALSE(effectivelyVisible(Graph::current(), idOf(a)));
        CHECK_FALSE(dl::pickable(Graph::current(), idOf(b)));
    }
}
