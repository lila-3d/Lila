#include <doctest/doctest.h>

#include "cmds/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return cmds::execute(name, call); }

const geo::Mesh& meshOf(const std::string& name)
{
    auto& g = scene::Graph::current();
    g.evaluate();
    return *g.find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("transfer attributes copies uvs and positions")
{
    run("newScene");
    std::string src = run("polySphere", {{"name", "Src"}, {"history", false}})[0];
    std::string dst = run("polySphere", {{"name", "Dst"}, {"history", false}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
    run("transferAttributes", {{"args", {src, dst}}, {"uvs", true}});
    const geo::Mesh& d = meshOf(dst);
    auto uv = d.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    // A point on the equator of the denser sphere should land on the source's uv band there, v near 0.5.
    size_t near = 0;
    for (geo::Index c = 0; c < d.cornerCapacity(); ++c)
        if (d.cornerValid(c) && std::abs(d.position(d.cornerPoint(c)).y) < 1e-6)
            near += std::abs(uv[c].y - 0.5f) < 0.05f;
    CHECK(near > 0);

    // Shrink-wrap: a big cube pulled onto the sphere ends inside the sphere's bounds.
    std::string box = run("polyCube", {{"name", "Box"}, {"history", false}, {"width", 300}, {"height", 300}, {"depth", 300},
                                       {"subdivisionsX", 4}, {"subdivisionsY", 4}, {"subdivisionsZ", 4}})[0];
    run("transferAttributes", {{"args", {src, box}}, {"uvs", false}, {"positions", true}});
    json info = run("meshInfo", {{"args", {box}}});
    CHECK(info["boundsMax"][0].get<double>() <= 50.0 + 1e-6);
    run("undo");
    CHECK(run("meshInfo", {{"args", {box}}})["boundsMax"][0].get<double>() == doctest::Approx(150));
}

TEST_CASE("boolean difference cuts a closed hole")
{
    run("newScene");
    std::string box = run("polyCube", {{"name", "Block"}, {"history", false}})[0];
    std::string cutter = run("polyCylinder", {{"name", "Drill"}, {"history", false}, {"radius", 20}, {"height", 300}})[0];
    run("polyBoolean", {{"args", {box, cutter}}, {"operation", "difference"}});
    CHECK(!run("objExists", {{"args", {cutter}}}).get<bool>());
    const geo::Mesh& m = meshOf(box);
    CHECK(m.validate(geo::kValidateManifold).empty());
    CHECK(m.numFaces() > 6);
    // The cube's flat top survives as one polygon with a hole-bordering ring of faces, not hundreds of triangles.
    CHECK(m.numFaces() < 200);
    run("undo");
    CHECK(run("objExists", {{"args", {cutter}}}).get<bool>());
}
