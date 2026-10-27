#include "TexTest.h"

#include "gn/Routing.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/Ports.h"
#include "tex/TextureSet.h"

using namespace textest;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }
json args(std::initializer_list<json> a) { return {{"args", json(a)}}; }

/** Bricks tinted by Colorize; the tint's input comes straight from the bricks or through a chain of reroutes. */
Graph tinted(int reroutes)
{
    Graph g;
    add(g, "Bricks", "bricks");
    add(g, "Tint", "colorize");
    std::string from = "Bricks.mask";
    for (int i = 0; i < reroutes; ++i)
    {
        std::string r = "R" + std::to_string(i);
        add(g, r, gn::kRerouteType);
        link(g, from, r + ".input");
        from = r + ".output";
    }
    link(g, from, "Tint.input");
    return g;
}
} // namespace

TEST_CASE("tex: reroutes evaluate exactly like the wire they sit on")
{
    image::Image direct = eval(tinted(0), "Tint", "out", 32);
    image::Image routed = eval(tinted(3), "Tint", "out", 32);
    CHECK(maxDiff(direct, routed) == 0.0);

    // A reroute takes the type of what feeds it.
    Graph g = tinted(1);
    NodePorts p = portsOf(g, *g.node("R0"));
    REQUIRE(p.outputs.size() == 1);
    CHECK(p.outputs[0].type == portsOf(g, *g.node("Bricks")).output("mask")->type);

    // Nothing feeding it: the reroute's readers fall back to their own values.
    Graph cut = tinted(1);
    std::erase_if(cut.links, [](const GLink& l) { return l.toNode == "R0"; });
    Plan pl = plan(cut, "Tint", "out", 16);
    CHECK(pl.error.empty());
}

TEST_CASE("tex: a Receive evaluates like the wire into its Send")
{
    Graph g;
    add(g, "Bricks", "bricks");
    add(g, "Tint", "colorize");
    add(g, "S", gn::kSendType, {{"name", "mask"}});
    add(g, "R", gn::kReceiveType, {{"name", "mask"}});
    link(g, "Bricks.mask", "S.value");
    link(g, "R.value", "Tint.input");
    CHECK(maxDiff(eval(tinted(0), "Tint", "out", 32), eval(g, "Tint", "out", 32)) == 0.0);
    CHECK(gn::portalProblems(g.nodes, g.links).empty());
}

TEST_CASE("tex: a muted node passes its input through")
{
    // Bricks -> Invert -> Tint2, with Invert muted, reads as Bricks -> Tint2.
    Graph chained;
    add(chained, "Bricks", "bricks");
    add(chained, "Inv", "invert").muted = true;
    add(chained, "Tint2", "colorize");
    link(chained, "Bricks.mask", "Inv.input");
    link(chained, "Inv.out", "Tint2.input");
    Graph direct;
    add(direct, "Bricks", "bricks");
    add(direct, "Tint2", "colorize");
    link(direct, "Bricks.mask", "Tint2.input");
    CHECK(maxDiff(eval(direct, "Tint2", "out", 32), eval(chained, "Tint2", "out", 32)) == 0.0);
    chained.node("Inv")->muted = false;
    CHECK(maxDiff(eval(direct, "Tint2", "out", 32), eval(chained, "Tint2", "out", 32)) > 0.1);
}

TEST_CASE("tex: a portal name that would loop is refused")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 32}});
    std::string s = run("texAddNode", {{"args", {"Rock", gn::kSendType}}}).get<std::string>();
    std::string r = run("texAddNode", {{"args", {"Rock", gn::kReceiveType}}}).get<std::string>();
    run("texAddNode", {{"args", {"Rock", "colorize"}}, {"name", "Tint"}});
    run("texConnect", args({"Rock", r + ".value", "Tint.input"}));
    run("texConnect", args({"Rock", "Tint.out", s + ".value"}));
    run("texSetValue", args({"Rock", s + ".name", "loop"}));
    CHECK_THROWS(run("texSetValue", args({"Rock", r + ".name", "loop"})));
}

TEST_CASE("tex: reroutes connect, save and undo through the commands")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 32}});
    run("texAddNode", {{"args", {"Rock", "bricks"}}, {"name", "Bricks"}});
    std::string r = run("texAddNode", {{"args", {"Rock", gn::kRerouteType}}}).get<std::string>();
    run("texConnect", args({"Rock", "Bricks.mask", r + ".input"}));
    run("texConnect", args({"Rock", r + ".output", "Output.height"}));
    run("texConnect", args({"Rock", r + ".output", "Output.roughness"}));
    json info = run("texGraphInfo", args({"Rock"}));
    CHECK_FALSE(info.contains("error"));
    tex::Graph g = tex::readGraph(*tex::findSet(scene::Graph::current(), "Rock"));
    CHECK(g.links.size() == 3);
    run("undo");
    CHECK(tex::readGraph(*tex::findSet(scene::Graph::current(), "Rock")).links.size() == 2);
}
