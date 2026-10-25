#include <doctest/doctest.h>

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/DeformEval.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

MeshPtr outMesh(const std::string& mesh)
{
    Graph& g = Graph::current();
    g.evaluate();
    return g.find(mesh)->as<MeshPtr>(MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("deformer evaluation: identical inputs reuse the last output")
{
    run("newScene");
    std::string mesh = run("polySphere", {{"name", "Ball"}, {"radius", 10}})[0];
    json made = run("nonlinear", {{"args", {mesh}}, {"type", "bend"}, {"params", {{"angle", 45.0}}}});
    const std::string bend = made[1];
    run("setKeyframe", {{"args", {bend + ".angle"}}, {"time", 0.0}, {"value", 45.0}});
    run("setKeyframe", {{"args", {bend + ".angle"}}, {"time", 1.0}, {"value", 45.0}});
    run("setKeyframe", {{"args", {bend + ".angle"}}, {"time", 2.0}, {"value", 90.0}});
    run("currentTime", {{"args", {0.0}}});
    MeshPtr first = outMesh(mesh);
    REQUIRE(first);

    // The key between two equal keys dirties the bend but changes none of its inputs.
    deformEval::resetStats();
    run("currentTime", {{"args", {0.5}}});
    MeshPtr same = outMesh(mesh);
    CHECK(same == first);
    CHECK(deformEval::stats().cacheHits == 1);
    CHECK(deformEval::stats().computes == 0);

    deformEval::resetStats();
    run("currentTime", {{"args", {1.5}}});
    MeshPtr bent = outMesh(mesh);
    CHECK(bent != first);
    CHECK(deformEval::stats().computes == 1);
    CHECK(deformEval::stats().cacheHits == 0);
    CHECK(bent->position(0) != first->position(0));

    // A deformer above the bend sees the bend's same output, so it is skipped too.
    run("currentTime", {{"args", {0.0}}});
    run("deltaMush", {{"args", {mesh}}});
    outMesh(mesh);
    deformEval::resetStats();
    run("currentTime", {{"args", {0.5}}});
    outMesh(mesh);
    CHECK(deformEval::stats().cacheHits == 2);
    CHECK(deformEval::stats().computes == 0);
}

TEST_CASE("deformer evaluation: delta mush binds once per topology, rest shape and settings")
{
    run("newScene");
    std::string a = run("polySphere", {{"name", "A"}, {"radius", 10}})[0];
    std::string b = run("polySphere", {{"name", "B"}, {"radius", 10}})[0];
    deformEval::clearBindCache();
    deformEval::resetStats();
    std::string mushA = run("deltaMush", {{"args", {a}}})[0];
    std::string mushB = run("deltaMush", {{"args", {b}}})[0];
    outMesh(a);
    outMesh(b);
    CHECK(deformEval::stats().bindMisses == 1);
    CHECK(deformEval::stats().bindHits == 1);

    // Rebinding to the same shape finds the same binding; other settings bind again.
    deformEval::resetStats();
    run("rebindDeformer", {{"args", {mushA}}});
    outMesh(a);
    CHECK(deformEval::stats().bindMisses == 0);
    run("setAttr", {{"args", {mushB + ".iterations", 4}}});
    outMesh(b);
    CHECK(deformEval::stats().bindMisses == 1);
}

TEST_CASE("deformer evaluation: hashes are stable and topology-sensitive")
{
    std::vector<double> v(1001);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = double(i) * 0.5;
    const uint64_t h = deformEval::hashBytes(v.data(), v.size() * sizeof(double));
    CHECK(h == deformEval::hashBytes(v.data(), v.size() * sizeof(double)));
    v[700] += 1e-9;
    CHECK(h != deformEval::hashBytes(v.data(), v.size() * sizeof(double)));

    run("newScene");
    std::string a = run("polySphere", {{"name", "A"}, {"radius", 10}})[0];
    std::string b = run("polySphere", {{"name", "B"}, {"radius", 20}})[0];
    std::string c = run("polySphere", {{"name", "C"}, {"radius", 10}, {"subdivisionsAxis", 21}})[0];
    CHECK(deformEval::topologyHash(*outMesh(a)) == deformEval::topologyHash(*outMesh(b)));
    CHECK(deformEval::topologyHash(*outMesh(a)) != deformEval::topologyHash(*outMesh(c)));

    auto x = std::make_shared<int>(1), y = std::make_shared<int>(1);
    CHECK(deformEval::identityKey(x) == deformEval::identityKey(x));
    CHECK(deformEval::identityKey(x) != deformEval::identityKey(y));
}
