#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

TEST_CASE("transfer attributes copies uvs and positions")
{
    run("newScene");
    std::string src = run("polySphere", {{"radius", 50.0}, {"name", "Src"}, {"generator", false}})[0];
    std::string dst = run("polySphere", {{"radius", 50.0}, {"name", "Dst"}, {"generator", false}, {"subdivisionsAxis", 40}, {"subdivisionsHeight", 30}})[0];
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
    std::string box = run("polyCube", {{"name", "Box"}, {"generator", false}, {"width", 300}, {"height", 300}, {"depth", 300},
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
    std::string box = run("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}, {"name", "Block"}, {"generator", false}})[0];
    std::string cutter = run("polyCylinder", {{"name", "Drill"}, {"generator", false}, {"radius", 20}, {"height", 300}})[0];
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

TEST_CASE("guided transfer fits a similar piece and rebuilds its uv islands")
{
    run("newScene");
    std::string src = run("polyCube", {{"name", "Plank"}, {"generator", false}, {"width", 100}, {"height", 10}, {"depth", 20}})[0];
    std::string dst = run("polyCube", {{"name", "Plank2"}, {"generator", false}, {"width", 120}, {"height", 9}, {"depth", 22},
                                       {"subdivisionsX", 6}, {"subdivisionsY", 2}, {"subdivisionsZ", 3}})[0];
    run("rotate", {{"args", {dst}}, {"by", {0, 90, 0}}});
    run("move", {{"args", {dst}}, {"by", {400, 50, 0}}});
    json r = run("transferAttributes", {{"args", {src, dst}}, {"uvs", true}, {"sampleSpace", "guided"}});
    REQUIRE(r.is_array());
    CHECK(r[0]["fitError"].get<double>() < 0.02);
    CHECK(r[0]["islands"].get<int>() > 0);

    const geo::Mesh& s = meshOf(src);
    const geo::Mesh& d = meshOf(dst);
    auto su = s.cornerAttrs().get<glm::vec2>("uv0");
    auto du = d.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!du.empty());
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (geo::Index c = 0; c < s.cornerCapacity(); ++c)
        if (s.cornerValid(c))
        {
            lo = glm::min(lo, su[c]);
            hi = glm::max(hi, su[c]);
        }
    const glm::vec2 pad = (hi - lo) * 0.1f;
    size_t outside = 0;
    for (geo::Index c = 0; c < d.cornerCapacity(); ++c)
        if (d.cornerValid(c))
            outside += glm::any(glm::lessThan(du[c], lo - pad)) || glm::any(glm::greaterThan(du[c], hi + pad));
    CHECK(outside == 0);

    run("undo");
    json kept = run("transferAttributes", {{"args", {src, dst}}, {"uvs", true}, {"sampleSpace", "guided"}, {"keepShape", true}});
    REQUIRE(kept.is_array());
    CHECK(kept[0]["relaxed"].get<int>() == 0);
    CHECK(kept[0]["islands"].get<int>() > 0);
}

TEST_CASE("guided transfer follows a mirrored and stretched copy")
{
    run("newScene");
    std::string src = run("polyCube", {{"name", "Bar"}, {"generator", false}, {"width", 100}, {"height", 8}, {"depth", 12},
                                       {"subdivisionsX", 6}, {"subdivisionsY", 1}, {"subdivisionsZ", 2}})[0];
    run("move", {{"args", {src}}, {"by", {60, 0, 0}}});
    run("freezeTransforms", {{"args", {src}}});
    std::string dst = run("duplicate", {{"args", {src}}})[0];
    run("scale", {{"args", {dst}}, {"by", {-1.1, 1, 0.95}}});
    run("freezeTransforms", {{"args", {dst}}});
    run("move", {{"args", {dst}}, {"by", {0, 0, 200}}});
    for (bool keep : {false, true})
    {
        json r = run("transferAttributes", {{"args", {src, dst}}, {"uvs", true}, {"sampleSpace", "guided"}, {"keepShape", keep}});
        REQUIRE(r.is_array());
        CHECK(r[0]["fitError"].get<double>() < 0.02);
        CHECK(r[0]["unwrapped"].get<int>() == 0);
        run("undo");
    }
}

TEST_CASE("guided transfer gives faces the source lacks islands of their own")
{
    run("newScene");
    std::string src = run("polyCube", {{"name", "Open"}, {"generator", false}, {"width", 100}, {"height", 10}, {"depth", 10},
                                       {"subdivisionsX", 6}})[0];
    json cap = json::array();
    const geo::Mesh& m = meshOf(src);
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceNormal(f).x > 0.9)
            cap.push_back(f);
    REQUIRE(!cap.empty());
    run("selectComponents", {{"args", {src}}, {"faces", cap}});
    run("deleteComponents");
    std::string dst = run("polyCube", {{"name", "Closed"}, {"generator", false}, {"width", 100}, {"height", 10}, {"depth", 10},
                                       {"subdivisionsX", 6}})[0];
    run("move", {{"args", {dst}}, {"by", {0, 0, 100}}});
    for (bool separate : {true, false})
    {
        json r = run("transferAttributes", {{"args", {src, dst}}, {"uvs", true}, {"sampleSpace", "guided"}, {"separateUnmatched", separate}});
        REQUIRE(r.is_array());
        CHECK(r[0]["unmatched"].get<int>() == (separate ? 1 : 0));
        run("undo");
    }
}
