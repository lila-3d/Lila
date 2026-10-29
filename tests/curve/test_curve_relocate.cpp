#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <map>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json rcall(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

json rpoints(std::initializer_list<D3> list)
{
    json a = json::array();
    for (const D3& p : list)
        a.push_back({p.x, p.y, p.z});
    return a;
}

/** World-space points of every mesh and world matrix of every transform, at a few times. */
struct RelocState
{
    std::map<std::string, glm::dmat4> worlds;
    std::map<std::string, std::vector<D3>> points;
};

std::vector<RelocState> relocSample()
{
    scene::Graph& g = scene::Graph::current();
    const double now = g.time();
    std::vector<RelocState> out;
    for (double t : {0.0, 0.3, 0.5, 1.0})
    {
        g.setTime(t, false);
        g.evaluate();
        RelocState s;
        for (scene::NodeId id : g.nodes())
        {
            const scene::Node* n = g.node(id);
            if (!n->type().dag)
                continue;
            const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            s.worlds[n->fullPath()] = w;
            if (n->type().isA("mesh"))
                if (const scene::MeshPtr& m = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
                    for (const D3& p : m->positions())
                        s.points[n->fullPath()].push_back(D3(w * glm::dvec4(p, 1.0)));
        }
        out.push_back(std::move(s));
    }
    g.setTime(now, false);
    g.evaluate();
    return out;
}

glm::dmat4 relocMatrix(const json& j)
{
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = j[size_t(c * 4 + r)].get<double>();
    return m;
}

/** Largest distance of a point from m times where it was; for a non-history transform, of its axes and origin too. */
double relocError(const std::vector<RelocState>& before, const std::vector<RelocState>& after, const glm::dmat4& m, const std::vector<std::string>& shaped)
{
    double worst = 0.0;
    for (size_t i = 0; i < before.size(); ++i)
    {
        for (const auto& [name, rpoints] : before[i].points)
        {
            const auto& got = after[i].points.at(name);
            REQUIRE(got.size() == rpoints.size());
            for (size_t p = 0; p < rpoints.size(); ++p)
                worst = std::max(worst, glm::length(got[p] - D3(m * glm::dvec4(rpoints[p], 1.0))));
        }
        for (const auto& [name, w] : before[i].worlds)
        {
            const bool kept = std::find(shaped.begin(), shaped.end(), name) != shaped.end();
            const glm::dmat4 want = kept ? m * w * glm::inverse(m) : m * w;
            const glm::dmat4& got = after[i].worlds.at(name);
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, glm::length(D3(got[c]) - D3(want[c])));
        }
    }
    return worst;
}
} // namespace

TEST_CASE("relocateScene moves sweeps and lofts with their curves, leaving the history meshes' transforms in place")
{
    rcall("newScene");
    std::string path = rcall("curve", {{"type", "poly"}, {"points", rpoints({D3(4000, 0, -2500), D3(4000, 0, -2450), D3(4030, 0, -2420)})}, {"name", "Path"}});
    json sweep = rcall("extrude", {{"args", {path}}, {"sides", 6}, {"radius", 4.0}, {"startCap", "grid"}, {"endCap", "grid"}});
    const std::string tube = sweep[0];
    std::string a = rcall("circle", {{"radius", 10.0}, {"center", {4100, 0, -2500}}});
    std::string b = rcall("circle", {{"radius", 14.0}, {"center", {4100, 20, -2500}}});
    std::string c = rcall("circle", {{"radius", 8.0}, {"center", {4100, 40, -2500}}});
    json loft = rcall("loft", {{"args", {a, b, c}}, {"samples", 16}, {"spansBetween", 2}});
    const std::string vase = loft[0];
    // A child of a history mesh follows its parent's transform, which stays put, so it needs a move of its own.
    rcall("spaceLocator", {{"name", "Tag"}});
    rcall("setAttr", {{"args", {"Tag.translate", {4100, 50, -2500}}}});
    rcall("parent", {{"args", {"Tag", vase}}});
    // Keys on a curve: the sweep follows them.
    rcall("setKeyframe", {{"args", {"Path.tz"}}, {"time", 0.0}, {"value", 0.0}});
    rcall("setKeyframe", {{"args", {"Path.tz"}}, {"time", 1.0}, {"value", 30.0}});
    rcall("setAttr", {{"args", {"Path.rotate", {0, 30, 0}}}});
    rcall("currentTime", {{"args", {0.0}}});

    const std::vector<RelocState> before = relocSample();
    json r = rcall("relocateScene", {{"args", {path}}, {"from", "bottom"}, {"rotation", "yaw"}});
    INFO(r.dump());
    const glm::dmat4 m = relocMatrix(r["matrix"]);
    CHECK(std::abs(r["rotation"][1].get<double>()) > 10.0);
    CHECK(glm::length(glm::dvec3(m[3])) > 1000.0);
    CHECK(r["notes"].dump().find("shaped by world-space history") != std::string::npos);
    scene::Graph& g = scene::Graph::current();
    const std::vector<std::string> shaped = {g.find(tube)->fullPath(), g.find(vase)->fullPath()};
    CHECK(relocError(before, relocSample(), m, shaped) < 1e-6);

    rcall("undo");
    const std::vector<RelocState> back = relocSample();
    for (size_t i = 0; i < before.size(); ++i)
    {
        CHECK(back[i].worlds == before[i].worlds);
        CHECK(back[i].points == before[i].points);
    }
}

TEST_CASE("relocateScene moves a pipe whose path curve was deleted, keeping the curve it holds")
{
    rcall("newScene");
    std::string path = rcall("curve", {{"type", "poly"}, {"points", rpoints({D3(4000, 0, -2500), D3(4000, 0, -2400), D3(4100, 0, -2400)})}, {"name", "Run"}});
    json r = rcall("pipe", {{"args", {path}}, {"preset", "pipe"}, {"sides", 8}});
    const std::string node = r[1];
    std::string anchor = rcall("spaceLocator", {{"name", "Anchor"}});
    rcall("setAttr", {{"args", {"Anchor.translate", {4000, 0, -2500}}}});
    rcall("delete", {{"args", {path}}});
    scene::Graph& g = scene::Graph::current();
    REQUIRE(g.find(node));
    const scene::Node* pn = g.find(node);
    REQUIRE(!g.source({pn->id(), pn->type().attrIndex("path"), -1}));

    const std::vector<RelocState> before = relocSample();
    json moved = rcall("relocateScene", {{"args", {anchor}}, {"from", "bottom"}, {"rotation", "yaw"}});
    INFO(moved.dump());
    const glm::dmat4 m = relocMatrix(moved["matrix"]);
    CHECK(glm::length(glm::dvec3(m[3])) > 1000.0);
    std::vector<std::string> shaped;
    for (scene::NodeId id : g.nodes())
        if (const scene::Node* n = g.node(id); n->type().isA("mesh"))
            shaped.push_back(n->fullPath());
    CHECK(relocError(before, relocSample(), m, shaped) < 1e-6);
}
