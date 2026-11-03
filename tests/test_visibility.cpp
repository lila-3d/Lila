#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "scene/HudStats.h"
#include "scene/Transform.h"

using namespace lila;
using namespace lila::scene;
using namespace testutil;

namespace
{
bool own(const std::string& name) { return Graph::current().find(name)->as<bool>(Transform::kVisibility); }
} // namespace

TEST_CASE("hiding a group hides what is under it unless inheritance is off")
{
    run("newScene");
    std::string cube = made(run("polyCube"));
    std::string inner = made(run("group", {{"args", {cube}}}));
    std::string outer = made(run("group", {{"args", {inner}}}));
    Graph& g = Graph::current();
    CHECK(effectivelyVisible(g, idOf(cube)));

    run("hide", {{"args", {outer}}});
    CHECK(own(cube));
    CHECK_FALSE(effectivelyVisible(g, idOf(cube)));
    CHECK_FALSE(effectivelyVisible(g, idOf(inner)));
    CHECK_FALSE(hud::visibleInHierarchy(g, idOf(cube)));
    CHECK(hud::polyStats(g, "uv0").scene.faces == 0);

    PrefStore::get().set("general", kInheritVisibilityPref, false);
    CHECK_FALSE(inheritVisibility());
    CHECK(effectivelyVisible(g, idOf(cube)));
    CHECK_FALSE(effectivelyVisible(g, idOf(outer)));
    CHECK(hud::polyStats(g, "uv0").scene.faces == 6);
    PrefStore::get().remove("general", kInheritVisibilityPref);
    CHECK(inheritVisibility());

    run("show", {{"args", {outer}}});
    run("hide", {{"args", {cube}}});
    CHECK_FALSE(effectivelyVisible(g, idOf(cube)));
    CHECK(effectivelyVisible(g, idOf(inner)));
}

TEST_CASE("toggleVisibility sets every object the same way by majority")
{
    run("newScene");
    std::string a = made(run("polyCube")), b = made(run("polyCube")), c = made(run("polyCube"));
    run("hide", {{"args", {c}}});

    // Two of three visible: all hide.
    CHECK(run("toggleVisibility", {{"args", {a, b, c}}}) == false);
    CHECK_FALSE(own(a));
    CHECK_FALSE(own(b));
    CHECK_FALSE(own(c));
    run("undo");
    CHECK(own(a));
    CHECK(own(b));
    CHECK_FALSE(own(c));

    // One of three visible: all show.
    run("hide", {{"args", {b}}});
    CHECK(run("toggleVisibility", {{"args", {a, b, c}}}) == true);
    CHECK((own(a) && own(b) && own(c)));

    // A tie hides.
    run("hide", {{"args", {a}}});
    CHECK(run("toggleVisibility", {{"args", {a, b}}}) == false);
    CHECK_FALSE(own(a));
    CHECK_FALSE(own(b));

    // A single object still flips.
    run("toggleVisibility", {{"args", {a}}});
    CHECK(own(a));
}

TEST_CASE("grow and shrink work for every component kind")
{
    run("newScene");
    std::string cube = made(run("polyCube"));

    run("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    run("growSelection");
    CHECK(selectedCount(cube, "faces") == 5);
    run("shrinkSelection");
    CHECK(selectedCount(cube, "faces") == 1);

    run("selectComponents", {{"args", {cube}}, {"edges", {0}}});
    CHECK(run("selectMode") == "edge");
    run("growSelection");
    CHECK(selectedCount(cube, "edges") == 5);
    run("shrinkSelection");
    CHECK(selectedCount(cube, "edges") == 1);
    run("undo");
    CHECK(selectedCount(cube, "edges") == 5);

    run("selectComponents", {{"args", {cube}}, {"vertices", {0}}});
    run("growSelection");
    CHECK(selectedCount(cube, "vertices") == 7);

    run("newScene");
    std::string plane = made(run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 4}}));
    run("selectComponents", {{"args", {plane}}, {"faces", {5}}});
    run("growSelection");
    CHECK(selectedCount(plane, "faces") == 9);
    run("growSelection");
    CHECK(selectedCount(plane, "faces") == 16);
    run("selectComponents", {{"args", {plane}}, {"faces", {0}}});
    run("growSelection");
    CHECK(selectedCount(plane, "faces") == 4);
}
