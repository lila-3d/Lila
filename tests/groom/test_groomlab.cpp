#include <doctest/doctest.h>

#include "GroomTestUtil.h"
#include "curve/CurveNodes.h"
#include "curve/NodeHelpers.h"
#include "curve/TestUtil.h"
#include "groom/GroomLab.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <glm/gtc/constants.hpp>
#include <tbb/global_control.h>

#include <filesystem>

using namespace lila;
using groomtest::D3;

namespace
{
/** Scatter roots on the scalp and interpolate the guides over them. */
std::string scatterGraph(double density)
{
    return R"({"version": 1, "nodes": [
      {"name": "scalp", "type": "Scalp"},
      {"name": "guides", "type": "Guides"},
      {"name": "scatter", "type": "Scatter", "params": {"density": )" +
           std::to_string(density) + R"(}},
      {"name": "interp", "type": "Interpolate"}],
     "connections": [
      {"from": "scalp", "to": "scatter", "input": "scalp"},
      {"from": "scatter", "to": "interp", "input": "strands"},
      {"from": "guides", "to": "interp", "input": "guides"},
      {"from": "scalp", "to": "interp", "input": "scalp"}],
     "output": "interp"})";
}

/** Guides standing out of the sphere around its crown, 8 cm long. */
curve::Splines crownGuides(double radius)
{
    curve::Splines s;
    for (int i = 0; i < 8; ++i)
    {
        const double polar = 0.4, azimuth = glm::two_pi<double>() * i / 8.0;
        const D3 n(std::sin(polar) * std::cos(azimuth), std::cos(polar), std::sin(polar) * std::sin(azimuth));
        s.push_back(groomtest::guide(n * radius, n, 8.0, 10));
    }
    s.push_back(groomtest::guide(D3(0.0, radius, 0.0), D3(0.0, 1.0, 0.0), 8.0, 10));
    return s;
}

void checkStrands(const curve::Splines& strands, double radius)
{
    REQUIRE(strands.size() > 100);
    for (const curve::Spline& s : strands)
    {
        REQUIRE(s.size() >= 2);
        // Rooted on the scalp, growing outward about as long as the guides.
        CHECK(glm::length(s.points.front()) == doctest::Approx(radius).epsilon(0.01));
        CHECK(glm::length(s.points.back()) > radius + 4.0);
        CHECK(s.pointAttrs.at("width").size() == s.size());
    }
}
} // namespace

TEST_CASE("growStrands: a groom graph scatters strands over a Lila scalp and interpolates Lila guide curves")
{
    const double radius = 10.0;
    const geo::Mesh scalp = groomtest::sphereMesh(radius);
    const groom::GrownStrands grown = groom::growStrands(scalp, crownGuides(radius), scatterGraph(2.0));
    CHECK(grown.warnings.empty());
    checkStrands(grown.strands, radius);

    // Denser scatter, more strands; the same graph twice, the same strands.
    const size_t sparse = grown.strands.size();
    CHECK(groom::growStrands(scalp, crownGuides(radius), scatterGraph(6.0)).strands.size() > 2 * sparse);
    const groom::GrownStrands again = groom::growStrands(scalp, crownGuides(radius), scatterGraph(2.0));
    REQUIRE(again.strands.size() == sparse);
    CHECK(again.strands[sparse / 2].points.back() == grown.strands[sparse / 2].points.back());

    CHECK_THROWS(groom::growStrands(scalp, {}, "{not json"));
}

TEST_CASE("groomStrands node: grows on the connected scalp, follows its transform and the guides curve")
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    const double radius = 10.0;
    const std::string head =
        curvetest::run("polySphere", {{"name", "Head"}, {"radius", radius}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    scene::Node* scalp = g.find(head);
    REQUIRE(scalp);
    scene::Node* guides = curve::helpers::createCurve(g, crownGuides(radius), "Guides");
    scene::Node* strands = g.createNode(*scene::types::find("groomStrands"), "Strands");
    REQUIRE(strands);
    const auto at = [&](const char* a) { return strands->type().attrIndex(a); };
    g.connect({scalp->id(), scene::MeshNode::kOutMesh, -1}, {strands->id(), at("inMesh"), -1});
    g.connect({scalp->id(), scene::Transform::kWorldMatrix, -1}, {strands->id(), at("meshMatrix"), -1});
    g.connect({guides->id(), curve::CurveShape::kWorldSpace, -1}, {strands->id(), at("inCurve"), -1});
    g.setValue({strands->id(), at("graph"), -1}, scatterGraph(2.0));

    g.evaluate();
    CHECK(strands->as<std::string>(at("warning")).empty());
    const int64_t count = strands->as<int64_t>(at("strandCount"));
    checkStrands(curve::splinesOf(strands->value(at("outCurve"))), radius);

    // Moving the head moves the strands' roots with it.
    curvetest::run("setAttr", {{"args", {head + ".translateX", 50.0}}});
    g.evaluate();
    CHECK(strands->as<int64_t>(at("strandCount")) == count);
    for (const curve::Spline& s : curve::splinesOf(strands->value(at("outCurve"))))
        CHECK(glm::length(s.points.front() - D3(50.0, 0.0, 0.0)) == doctest::Approx(radius).epsilon(0.01));

    // The default graph (empty) grows hair too.
    g.setValue({strands->id(), at("graph"), -1}, std::string());
    g.evaluate();
    CHECK(strands->as<int64_t>(at("strandCount")) > 0);

    // A graph that can't load says so and grows nothing.
    g.setValue({strands->id(), at("graph"), -1}, std::string("{\"nodes\": [{\"name\": \"x\", \"type\": \"NoSuchNode\"}]}"));
    g.evaluate();
    CHECK(strands->as<int64_t>(at("strandCount")) == 0);
    CHECK(!strands->as<std::string>(at("warning")).empty());
}

namespace
{
/** Every bit of the strands: points, per-point attrs and per-curve attrs, in order. */
uint64_t bitsOf(const curve::Splines& strands)
{
    uint64_t h = 1469598103934665603ull;
    auto add = [&](const void* data, size_t bytes) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i)
            h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const curve::Spline& s : strands)
    {
        add(s.points.data(), s.points.size() * sizeof(D3));
        for (const auto& [name, values] : s.pointAttrs)
        {
            add(name.data(), name.size());
            add(values.data(), values.size() * sizeof(double));
        }
        for (const auto& [name, value] : s.attrs)
        {
            add(name.data(), name.size());
            add(&value, sizeof value);
        }
    }
    return h;
}

struct StrandsScene
{
    std::string head;
    scene::Node* strands = nullptr;

    int at(const char* a) const { return strands->type().attrIndex(a); }
    curve::Splines grown() const
    {
        scene::Graph::current().evaluate();
        return curve::splinesOf(strands->value(at("outCurve")));
    }
};

/** A head, guides on its crown and a groomStrands node growing the default graph's hair on it. */
StrandsScene strandsScene(double radius)
{
    scene::Graph& g = scene::Graph::current();
    g.clear();
    UndoStack::get().clear();
    StrandsScene out;
    out.head = curvetest::run("polySphere", {{"name", "Head"}, {"radius", radius}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    scene::Node* scalp = g.find(out.head);
    scene::Node* guides = curve::helpers::createCurve(g, crownGuides(radius), "Guides");
    out.strands = g.createNode(*scene::types::find("groomStrands"), "Strands");
    g.connect({scalp->id(), scene::MeshNode::kOutMesh, -1}, {out.strands->id(), out.at("inMesh"), -1});
    g.connect({scalp->id(), scene::Transform::kWorldMatrix, -1}, {out.strands->id(), out.at("meshMatrix"), -1});
    g.connect({guides->id(), curve::CurveShape::kWorldSpace, -1}, {out.strands->id(), out.at("inCurve"), -1});
    return out;
}
} // namespace

TEST_CASE("groomStrands: the same strands bit for bit on any thread count, evaluated again, and after save and load")
{
    const double radius = 10.0;
    const curve::Splines first = strandsScene(radius).grown();
    REQUIRE(first.size() > 1000);
    const uint64_t bits = bitsOf(first);

    // Every parallel loop cut differently: one thread takes each range whole, two split and steal.
    for (const int threads : {1, 2})
    {
        const tbb::global_control cap(tbb::global_control::max_allowed_parallelism, size_t(threads));
        CHECK_MESSAGE(bitsOf(strandsScene(radius).grown()) == bits, threads, " threads");
    }

    // Evaluated again from the node's cache, and after an edit that is undone: the same bits.
    StrandsScene s = strandsScene(radius);
    CHECK(bitsOf(s.grown()) == bits);
    CHECK(bitsOf(s.grown()) == bits);
    curvetest::run("setAttr", {{"args", {s.head + ".translateY", 3.0}}});
    CHECK(bitsOf(s.grown()) != bits);
    curvetest::run("setAttr", {{"args", {s.head + ".translateY", 0.0}}});
    CHECK(bitsOf(s.grown()) == bits);
    scene::Graph::current().setValue({s.strands->id(), s.at("graph"), -1}, scatterGraph(2.0));
    CHECK(bitsOf(s.grown()) != bits);
    scene::Graph::current().setValue({s.strands->id(), s.at("graph"), -1}, std::string());
    CHECK(bitsOf(s.grown()) == bits);

    // Saved and opened again: a fresh node grows the same strands.
    const auto path = std::filesystem::temp_directory_path() / "lila_groomstrands_roundtrip.lila";
    curvetest::run("saveScene", {{"args", {path.string()}}});
    scene::Graph::current().clear();
    UndoStack::get().clear();
    curvetest::run("openScene", {{"args", {path.string()}}});
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* opened = g.find("Strands");
    REQUIRE(opened);
    CHECK(bitsOf(curve::splinesOf(opened->value(opened->type().attrIndex("outCurve")))) == bits);
    std::filesystem::remove(path);
}
