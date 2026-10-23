#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;

namespace
{
json run(const std::string& name, const json& call = json::object()) { return ops::execute(name, call); }
std::string made(const json& r) { return (r.is_array() ? r[0] : r).get<std::string>(); }
json members(const std::string& set) { return run("setMembers", {{"args", {set}}}); }

std::string tempPath(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / "lila_tests" / "sets";
    std::filesystem::create_directories(dir);
    return (dir / name).generic_string();
}
} // namespace

TEST_CASE("object sets: membership, renames, undo")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube")), c = made(run("polyCube"));
    run("select", {{"args", {a, b}}});
    std::string set = run("createSet", {{"name", "Props"}}).get<std::string>();
    CHECK(set == "Props");
    CHECK(members(set) == json({a, b}));
    CHECK(run("listSets") == json({"Props"}));
    CHECK(run("listSets", {{"quickSelect", true}}).empty());

    run("addToSet", {{"args", {set, c, a}}});
    CHECK(members(set) == json({a, b, c}));
    run("undo");
    CHECK(members(set) == json({a, b}));

    run("removeFromSet", {{"args", {set, a}}});
    CHECK(members(set) == json({b}));
    run("undo");

    // An object can be in several sets, unlike a display layer.
    std::string quick = run("createSet", {{"args", {b}}, {"name", "Pick"}, {"quickSelect", true}}).get<std::string>();
    CHECK(run("listSets", {{"quickSelect", true}}) == json({"Pick"}));
    CHECK(run("listSets", {{"object", b}}) == json({"Props", "Pick"}));

    run("rename", {{"args", {a, "Crate"}}});
    run("rename", {{"args", {set, "Stuff"}}});
    CHECK(members("Stuff") == json({"Crate", b}));

    run("select", {{"clear", true}});
    run("selectSetMembers", {{"args", {"Stuff"}}});
    CHECK(Graph::current().selection().size() == 2);

    CHECK_THROWS_AS(run("addToSet", {{"args", {"Stuff", "Pick"}}}), CommandError);
    CHECK_THROWS_AS(run("addToSet", {{"args", {"Stuff", "nothing"}}}), CommandError);

    run("deleteSet", {{"args", {"Pick"}}});
    CHECK(run("listSets") == json({"Stuff"}));
    run("undo");
    CHECK(members("Pick") == json({b}));
}

TEST_CASE("object sets: component members and quick select")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    std::string set = run("createSet", {{"args", {cube + ".vtx[0:2]", cube + ".vtx[5]", cube + ".f[1]"}}, {"name", "Bits"}, {"quickSelect", true}})
                          .get<std::string>();
    CHECK(members(set) == json({cube + ".vtx[0:2]", cube + ".vtx[5]", cube + ".f[1]"}));
    CHECK(run("setMembers", {{"args", {set}}, {"nodes", true}}) == json({cube}));

    run("removeFromSet", {{"args", {set, cube + ".vtx[1]"}}});
    CHECK(members(set) == json({cube + ".vtx[0]", cube + ".vtx[2]", cube + ".vtx[5]", cube + ".f[1]"}));
    run("addToSet", {{"args", {set, cube + ".vtx[1]"}}});

    // Selecting a component set selects the mesh and its components, in a component mode it has.
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"clear", true}});
    run("selectSetMembers", {{"args", {set}}});
    Graph& g = Graph::current();
    const NodeId id = g.find(cube)->id();
    CHECK(g.selection() == std::vector<NodeId>{id});
    CHECK(components::mode() == ComponentMode::Vertex);
    CHECK(components::selected(id, ComponentMode::Vertex) == components::Ids{0, 1, 2, 5});
    CHECK(components::selected(id, ComponentMode::Face) == components::Ids{1});

    // A set made in component mode takes the components, not the objects.
    components::select(id, ComponentMode::Vertex, {3, 4});
    std::string fromSel = run("createSet").get<std::string>();
    CHECK(members(fromSel) == json({cube + ".vtx[3:4]"}));

    // A topology edit carries component members to their new ids.
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("createSet", {{"args", {cube + ".f[4:5]"}}, {"name", "Top"}});
    run("deleteComponents");
    CHECK(members("Top") == json({cube + ".f[3:4]"}));
    run("undo");
    CHECK(members("Top") == json({cube + ".f[4:5]"}));

    CHECK_THROWS_AS(run("createSet", {{"args", {cube + ".uv[0]"}}}), CommandError);
    CHECK_THROWS_AS(run("createSet", {{"args", {cube + ".vtx[3:1]"}}}), CommandError);
}

TEST_CASE("object sets: deleted members drop out and come back on undo")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube"));
    std::string set = run("createSet", {{"args", {a, b + ".f[0]"}}}).get<std::string>();
    run("delete", {{"args", {a}}});
    CHECK(members(set) == json({b + ".f[0]"}));
    run("undo");
    CHECK(members(set) == json({a, b + ".f[0]"}));
    run("delete", {{"args", {b}}});
    CHECK(members(set) == json({a}));

    // A new node with a deleted member's name doesn't join.
    std::string again = made(run("polyCube", {{"name", b}}));
    CHECK(again == b);
    CHECK(members(set) == json({a}));
}

TEST_CASE("object sets save and open in both formats")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube"));
    run("createSet", {{"args", {a, b + ".e[2:4]"}}, {"name", "Export"}});
    run("createSet", {{"args", {b + ".vtx[7]"}}, {"name", "Quick"}, {"quickSelect", true}});
    run("createSet", {{"empty", true}, {"name", "Empty"}});
    const json before = run("listSets", {{"info", true}});

    for (const char* ext : {".lila", ".lilaa"})
    {
        auto path = tempPath(std::string("sets") + ext);
        run("saveScene", {{"args", {path}}});
        run("newScene");
        CHECK(run("listSets").empty());
        run("openScene", {{"args", {path}}});
        CHECK(run("listSets", {{"info", true}}) == before);
        // Bound by id after loading: renames keep the membership.
        run("rename", {{"args", {a, "Renamed"}}});
        CHECK(members("Export")[0] == "Renamed");
        run("rename", {{"args", {"Renamed", a}}});
    }
}

TEST_CASE("object sets survive referencing")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube"));
    run("createSet", {{"args", {a, b + ".f[2]"}}, {"name", "Parts"}});
    std::string asset = tempPath("set_asset.lilaa");
    run("saveScene", {{"args", {asset}}});

    run("newScene");
    run("createReference", {{"args", {asset}}, {"namespace", "hero"}});
    CHECK(members("hero:Parts") == json({"hero:" + a, "hero:" + b + ".f[2]"}));

    // A scene set holding referenced objects keeps them through unload and reload, and in the saved scene.
    run("createSet", {{"args", {"hero:" + a}}, {"name", "Shot"}});
    run("unloadReference", {{"args", {"hero"}}});
    CHECK(members("Shot").empty());
    std::string shot = tempPath("set_shot.lila");
    run("saveScene", {{"args", {shot}}});
    run("loadReference", {{"args", {"hero"}}});
    CHECK(members("Shot") == json({"hero:" + a}));
    run("newScene");
    run("openScene", {{"args", {shot}}});
    CHECK(members("Shot").empty());
    run("loadReference", {{"args", {"hero"}}});
    CHECK(members("Shot") == json({"hero:" + a}));

    // Edits to a referenced set are reference edits.
    run("addToSet", {{"args", {"hero:Parts", "hero:" + b}}});
    run("saveScene", {{"args", {shot}}});
    run("newScene");
    run("openScene", {{"args", {shot}}});
    CHECK(members("hero:Parts") == json({"hero:" + a, "hero:" + b + ".f[2]", "hero:" + b}));
}
