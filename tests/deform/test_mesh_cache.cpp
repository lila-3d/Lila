#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Anim.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshCache.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using namespace deformtest;
namespace fs = std::filesystem;

namespace
{
std::vector<glm::dvec3> pointsAt(const std::string& mesh, double t)
{
    Graph& g = graph();
    g.setTime(t);
    g.evaluate();
    MeshPtr m = g.find(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
    REQUIRE(m);
    return {m->positions().begin(), m->positions().end()};
}

bool nearLoose(const glm::dvec3& a, const glm::dvec3& b, double eps = 1e-4) { return glm::length(a - b) <= eps; }

/** Rest points moved along x by 10 per second, sampled at 0, 1 and 2 seconds. */
std::shared_ptr<MeshCacheSamples> sliding(const std::vector<glm::dvec3>& rest)
{
    auto s = std::make_shared<MeshCacheSamples>();
    for (int k = 0; k <= 2; ++k)
    {
        std::vector<glm::dvec3> pts = rest;
        for (glm::dvec3& p : pts)
            p.x += 10.0 * k;
        s->add(double(k), pts);
    }
    return s;
}

int cachesOn(const std::string& mesh)
{
    int n = 0;
    for (NodeId id : modifiers::stack(graph(), graph().find(mesh)->id()))
        n += graph().node(id)->type().name == meshCache::kType;
    return n;
}
} // namespace

TEST_CASE("a mesh cache plays its samples back at the scene time, interpolated, held or looped")
{
    run("newScene");
    const std::string box = run("polyCube", {{"name", "Box"}, {"width", 2}, {"height", 2}, {"depth", 2}})[0];
    const std::vector<glm::dvec3> rest = pointsAt(box, 0.0);
    Graph& g = graph();
    const NodeId mesh = g.find(box)->id();
    const NodeId cache = meshCache::create(g, mesh, sliding(rest));
    CHECK(meshCache::find(g, mesh) == cache);
    REQUIRE(meshCache::samples(g, cache));
    CHECK(meshCache::samples(g, cache)->sampleCount() == 3);

    CHECK(nearLoose(pointsAt(box, 0.0)[0], rest[0]));
    CHECK(nearLoose(pointsAt(box, 0.5)[0], rest[0] + glm::dvec3(5, 0, 0)));
    CHECK(nearLoose(pointsAt(box, 2.0)[3], rest[3] + glm::dvec3(20, 0, 0)));
    // Past the end it holds the last sample, before the start the first.
    CHECK(nearLoose(pointsAt(box, 7.0)[0], rest[0] + glm::dvec3(20, 0, 0)));
    CHECK(nearLoose(pointsAt(box, -1.0)[0], rest[0]));

    const NodeType& t = g.node(cache)->type();
    g.setValue({cache, t.attrIndex("interpolate"), -1}, false);
    CHECK(nearLoose(pointsAt(box, 1.75)[0], rest[0] + glm::dvec3(10, 0, 0)));
    g.setValue({cache, t.attrIndex("interpolate"), -1}, true);
    g.setValue({cache, t.attrIndex("outOfRange"), -1}, int64_t(1));
    CHECK(nearLoose(pointsAt(box, 2.5)[0], rest[0] + glm::dvec3(5, 0, 0)));
    g.setValue({cache, t.attrIndex("outOfRange"), -1}, int64_t(0));
    g.setValue({cache, t.attrIndex("speed"), -1}, 2.0);
    g.setValue({cache, t.attrIndex("timeOffset"), -1}, 0.5);
    CHECK(nearLoose(pointsAt(box, 1.0)[0], rest[0] + glm::dvec3(10, 0, 0)));
    g.setValue({cache, t.attrIndex("speed"), -1}, 1.0);
    g.setValue({cache, t.attrIndex("timeOffset"), -1}, 0.0);

    // Half the envelope is half way between the input and the cache.
    g.setValue({cache, DeformerNode::kEnvelope, -1}, 0.5);
    CHECK(nearLoose(pointsAt(box, 2.0)[0], rest[0] + glm::dvec3(10, 0, 0)));
    g.setValue({cache, DeformerNode::kEnvelope, -1}, 1.0);

    json info = run("meshCacheInfo", {{"args", {box}}});
    CHECK(info["samples"] == 3);
    CHECK(info["points"] == rest.size());
    CHECK(info["end"].get<double>() == doctest::Approx(2.0));

    // A cache for other points passes its input through and says why.
    auto wrong = std::make_shared<MeshCacheSamples>();
    wrong->add(0.0, std::vector<glm::dvec3>(3, glm::dvec3(1.0)));
    meshCache::setSamples(g, cache, wrong);
    CHECK(nearLoose(pointsAt(box, 1.0)[0], rest[0]));
    CHECK(!static_cast<const ModifierNode*>(g.node(cache))->error().empty());
    g.setTime(0.0);
}

TEST_CASE("mesh cache samples save and load in both scene formats")
{
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        run("newScene");
        const std::string box = run("polyCube", {{"name", "Box"}, {"subdivisionsX", 3}})[0];
        const std::vector<glm::dvec3> rest = pointsAt(box, 0.0);
        meshCache::create(graph(), graph().find(box)->id(), sliding(rest), "Cache");
        const std::string path = tempFile(std::string("meshcache") + ext).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        run("openScene", {{"args", {path}}});
        REQUIRE(graph().find("Cache"));
        const auto s = meshCache::samples(graph(), graph().find("Cache")->id());
        REQUIRE(s);
        CHECK(s->sampleCount() == 3);
        CHECK(s->pointCount == rest.size());
        CHECK(s->times[2] == doctest::Approx(2.0));
        const std::vector<glm::dvec3> mid = pointsAt(box, 1.5);
        for (size_t p = 0; p < rest.size(); ++p)
            CHECK(nearLoose(mid[p], rest[p] + glm::dvec3(15, 0, 0)));
        graph().setTime(0.0);
    }
}

TEST_CASE("bakeMeshCache records a mesh's motion into a cache on top of its stack, undoably")
{
    run("newScene");
    const std::string box = run("polyCube", {{"name", "Box"}})[0];
    const std::vector<glm::dvec3> rest = pointsAt(box, 0.0);
    meshCache::create(graph(), graph().find(box)->id(), sliding(rest));
    const double fps = anim::playback().fps;

    json made = run("bakeMeshCache", {{"args", {box}}, {"start", 0}, {"end", int(2 * fps)}, {"step", int(fps)}, {"name", "Baked"}});
    REQUIRE(made.size() == 1);
    CHECK(cachesOn(box) == 2);
    json info = run("meshCacheInfo", {{"args", {made[0]}}});
    CHECK(info["samples"] == 3);
    CHECK(info["end"].get<double>() == doctest::Approx(2.0));
    // The baked cache replays what the stack under it made.
    CHECK(nearLoose(pointsAt(box, 1.0)[0], rest[0] + glm::dvec3(10, 0, 0)));

    run("undo");
    CHECK(cachesOn(box) == 1);
    CHECK(!graph().find(made[0].get<std::string>()));
    run("redo");
    CHECK(cachesOn(box) == 2);
    graph().setTime(0.0);
}
