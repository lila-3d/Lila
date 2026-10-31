#include "ops/Command.h"
#include "geo/Symmetry.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "uv/SymmetryUv.h"
#include "uv/UvEdit.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace lila;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

scene::MeshPtr meshOf(const std::string& name)
{
    scene::Graph::current().evaluate();
    return scene::Graph::current().find(name)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

std::vector<glm::vec2> uvsOf(const std::string& name)
{
    auto s = meshOf(name)->cornerAttrs().get<glm::vec2>("uv0");
    return {s.begin(), s.end()};
}

/** Corner pairs across x = 0, found from the positions. */
std::vector<std::pair<geo::Index, geo::Index>> cornerPairs(const geo::Mesh& m)
{
    geo::SymmetryMap map = geo::SymmetryMap::build(m, geo::axisPlane(0));
    std::vector<std::pair<geo::Index, geo::Index>> out;
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
    {
        if (!m.cornerValid(c))
            continue;
        geo::Index f = map.mirrorFace(m, m.cornerFace(c)), p = map.mirrorPoint(m.cornerPoint(c));
        if (f != geo::kInvalid && p != geo::kInvalid)
            out.emplace_back(c, m.findCorner(f, p));
    }
    return out;
}

/** Largest miss of the uvs from mirror symmetry about the line u = u0. */
double asymmetry(const std::string& name, double u0)
{
    scene::MeshPtr m = meshOf(name);
    std::vector<glm::vec2> uv = uvsOf(name);
    double worst = 0.0;
    for (auto [c, pc] : cornerPairs(*m))
        worst = std::max({worst, std::abs(double(uv[pc].x) - (2.0 * u0 - double(uv[c].x))), std::abs(double(uv[pc].y) - double(uv[c].y))});
    return worst;
}

std::vector<uint32_t> facesOnSide(const geo::Mesh& m, double sign)
{
    std::vector<uint32_t> out;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceCentroid(f).x * sign > 1e-6)
            out.push_back(f);
    return out;
}

glm::dvec2 uvCentroid(const std::string& name, std::span<const uint32_t> faces)
{
    scene::MeshPtr m = meshOf(name);
    std::vector<glm::vec2> uv = uvsOf(name);
    glm::dvec2 sum(0.0);
    double n = 0.0;
    for (uint32_t f : faces)
        m->forEachFaceCorner(f, [&](geo::Index c) {
            sum += glm::dvec2(uv[c]);
            n += 1.0;
        });
    return sum / n;
}

/** Pulls the two halves of the plane's uvs apart into islands mirrored about u = 0.5. */
void splitHalves(const std::string& name)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.find(name);
    scene::PlugRef base = uv::uvEditPlug(*n);
    geo::Mesh m = *std::get<scene::MeshPtr>(g.storedValue(base));
    std::span<glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const float du = m.faceCentroid(f).x > 0.0 ? 0.1f : -0.1f;
            m.forEachFaceCorner(f, [&](geo::Index c) { uv[c].x += du; });
        }
    g.setValue(base, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
}

std::vector<uint32_t> allFaces(const geo::Mesh& m)
{
    std::vector<uint32_t> out;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            out.push_back(f);
    return out;
}

std::string symmetricPlane()
{
    run("newScene");
    std::string plane = run("polyPlane", {{"subdivisionsX", 4}, {"subdivisionsZ", 2}})[0];
    run("setSymmetry", {{"args", {plane}}, {"axis", "x"}});
    run("select", {{"args", {plane}}});
    return plane;
}
} // namespace

TEST_CASE("uv symmetry: a one-sided move keeps a spanning island symmetric")
{
    std::string plane = symmetricPlane();
    REQUIRE(asymmetry(plane, 0.5) < 1e-5);
    scene::MeshPtr m = meshOf(plane);
    std::vector<uint32_t> right = facesOnSide(*m, 1.0);
    REQUIRE(right.size() == 4);
    std::vector<glm::vec2> before = uvsOf(plane);

    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {plane}}, {"faces", {right.front()}}});
    run("uvTransform", {{"translate", {0.05, 0.02}}});
    CHECK(asymmetry(plane, 0.5) < 1e-5);
    std::vector<glm::vec2> after = uvsOf(plane);
    size_t movedOut = 0, movedBack = 0;
    for (auto [c, pc] : cornerPairs(*m))
        if (c != pc && std::abs(after[c].x - before[c].x - 0.05f) < 1e-5f)
        {
            ++movedOut;
            movedBack += std::abs(after[pc].x - before[pc].x + 0.05f) < 1e-5f && std::abs(after[pc].y - before[pc].y - 0.02f) < 1e-5f;
        }
    CHECK(movedOut > 0);
    CHECK(movedBack == movedOut);

    // The whole island moves as it is.
    run("selectComponents", {{"args", {plane}}, {"faces", allFaces(*m)}});
    run("uvTransform", {{"translate", {0.3, 0.0}}});
    CHECK(asymmetry(plane, 0.8) < 1e-5);

    // Symmetry off: only the selection moves.
    run("setSymmetry", {{"args", {plane}}, {"axis", "off"}});
    run("selectComponents", {{"args", {plane}}, {"faces", {right.front()}}});
    run("uvTransform", {{"translate", {0.05, 0.0}}});
    CHECK(asymmetry(plane, 0.8) > 0.01);
}

TEST_CASE("uv symmetry: two islands move and turn mirrored")
{
    std::string plane = symmetricPlane();
    splitHalves(plane);
    run("select", {{"args", {plane}}});
    CHECK(run("uvLayout")["islands"] == 2);
    REQUIRE(asymmetry(plane, 0.5) < 1e-5);
    scene::MeshPtr m;

    m = meshOf(plane);
    std::vector<uint32_t> right = facesOnSide(*m, 1.0), left = facesOnSide(*m, -1.0);
    glm::dvec2 r0 = uvCentroid(plane, right), l0 = uvCentroid(plane, left);
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {plane}}, {"faces", right}});
    run("uvTransform", {{"translate", {0.2, 0.1}}});
    glm::dvec2 r1 = uvCentroid(plane, right), l1 = uvCentroid(plane, left);
    CHECK(r1.x - r0.x == doctest::Approx(0.2).epsilon(1e-4));
    CHECK(r1.y - r0.y == doctest::Approx(0.1).epsilon(1e-4));
    CHECK(l1.x - l0.x == doctest::Approx(-0.2).epsilon(1e-4));
    CHECK(l1.y - l0.y == doctest::Approx(0.1).epsilon(1e-4));
    CHECK(asymmetry(plane, 0.5) < 1e-4);

    // Selecting both (as the uv editor does with symmetry on), each turns about its own centre, mirrored.
    run("selectComponents", {{"args", {plane}}, {"faces", allFaces(*m)}});
    run("uvTransform", {{"rotate", 30.0}, {"perIsland", true}});
    CHECK(asymmetry(plane, 0.5) < 1e-4);
    CHECK(glm::length(uvCentroid(plane, right) - r1) < 1e-4);
}

TEST_CASE("uv symmetry: unwrap keeps the halves symmetric")
{
    std::string plane = symmetricPlane();
    scene::MeshPtr m = meshOf(plane);
    std::vector<uint32_t> centre;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e) && std::abs(m->position(m->edgePoint(e, 0)).x) < 1e-6 && std::abs(m->position(m->edgePoint(e, 1)).x) < 1e-6 &&
            !m->isBoundaryEdge(e))
            centre.push_back(e);
    REQUIRE(centre.size() == 2);
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {plane}}, {"edges", centre}});
    run("uvCutEdges");
    run("selectMode", {{"args", {"object"}}});
    run("uvUnwrap");
    CHECK(asymmetry(plane, 0.5) < 1e-4);
    run("uvStraighten");
    CHECK(asymmetry(plane, 0.5) < 1e-4);
}

TEST_CASE("uv symmetry: picks and pins take the mirror partner")
{
    std::string plane = symmetricPlane();
    scene::Node* n = scene::Graph::current().find(plane);
    scene::MeshPtr m = meshOf(plane);
    std::optional<geo::SymmetryMap> map = uv::uvSymmetryMap(*n, *m);
    REQUIRE(map);
    std::vector<uint32_t> right = facesOnSide(*m, 1.0);
    const geo::Index pick[] = {right.front()};
    std::vector<geo::Index> faces = uv::mirrorComponents(*map, *m, scene::ComponentMode::Face, pick);
    REQUIRE(faces.size() == 2);
    geo::Index other = faces[0] == pick[0] ? faces[1] : faces[0];
    CHECK(m->faceCentroid(other).x == doctest::Approx(-m->faceCentroid(pick[0]).x));

    uv::UvMirror mirror = uv::buildUvMirror(*m, *map, "uv0");
    REQUIRE(mirror.islandPartner.size() == 1);
    CHECK(mirror.islandPartner[0] == 0);
    CHECK(mirror.fitted[0] == 1);

    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {plane}}, {"faces", {pick[0]}}});
    run("uvPin");
    auto pin = meshOf(plane)->cornerAttrs().get<bool>("uvPin");
    for (auto [c, pc] : cornerPairs(*meshOf(plane)))
        CHECK(pin[c] == pin[pc]);
    CHECK(std::count(pin.begin(), pin.end(), true) > 4);
}

namespace
{
/** Every face's uvs equal its images' under the mesh's radial symmetry, corner by matched corner. */
void checkStacked(const std::string& mesh, std::vector<geo::Index> only = {})
{
    geo::Mesh m = *meshOf(mesh);
    const auto spec = scene::symmetrySpec(*scene::Graph::current().find(mesh));
    REQUIRE(spec);
    REQUIRE(spec->radial);
    geo::SymmetryMap map = geo::SymmetryMap::persistent(m, *spec->radial, 1e-4);
    std::span<const glm::vec2> uv = m.cornerAttrs().get<glm::vec2>("uv0");
    REQUIRE(!uv.empty());
    size_t compared = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || (!only.empty() && std::ranges::find(only, f) == only.end()))
            continue;
        m.forEachFaceCorner(f, [&](geo::Index c) {
            for (int g = 1; g < map.order(); ++g)
                if (const geo::Index fi = map.imageFace(m, g, f); fi != geo::kInvalid && fi != f)
                {
                    const geo::Index ci = m.findCorner(fi, map.image(g, m.cornerPoint(c)));
                    REQUIRE(ci != geo::kInvalid);
                    CHECK(uv[ci] == uv[c]);
                    ++compared;
                }
        });
    }
    CHECK(compared > 0);
}
} // namespace

TEST_CASE("uvStackRadial stacks one wedge's uvs on every copy")
{
    for (bool mirror : {false, true})
    {
        CAPTURE(mirror);
        run("newScene");
        const std::string mesh = run("polyCylinder", {{"subdivisionsAxis", 24}, {"subdivisionsHeight", 3}, {"caps", false}})[0];
        run("setSymmetry", {{"args", {mesh}}, {"axis", "radial"}, {"count", 6}, {"mirror", mirror}});
        run("select", {{"args", {mesh}}});
        run("selectMode", {{"args", {"object"}}});
        run("uvStackRadial", {{"wedge", 0}});
        checkStacked(mesh);
    }
}

TEST_CASE("uv edits on a radially symmetric mesh stack the copies")
{
    run("newScene");
    const std::string mesh = run("polyCylinder", {{"subdivisionsAxis", 24}, {"subdivisionsHeight", 3}, {"caps", false}})[0];
    run("setSymmetry", {{"args", {mesh}}, {"axis", "radial"}, {"count", 8}});
    run("selectComponents", {{"args", {mesh}}, {"faces", {0, 1, 2}}});
    run("uvUnwrapSymmetric");
    checkStacked(mesh, {0, 1, 2});
}

TEST_CASE("uv symmetry: cutting along the symmetry line opens it")
{
    std::string plane = symmetricPlane();
    run("uvProject", {{"type", "planar"}, {"axis", "y"}});
    scene::MeshPtr m = meshOf(plane);
    std::vector<uint32_t> centre;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e) && std::abs(m->position(m->edgePoint(e, 0)).x) < 1e-6 && std::abs(m->position(m->edgePoint(e, 1)).x) < 1e-6 &&
            !m->isBoundaryEdge(e))
            centre.push_back(e);
    REQUIRE(centre.size() == 2);
    run("selectMode", {{"args", {"edge"}}});
    run("selectComponents", {{"args", {plane}}, {"edges", centre}});
    CHECK(run("uvCut")["cut"] == 2);
    run("selectMode", {{"args", {"object"}}});
    json info = run("uvLayout");
    CHECK(info["islands"] == 2);
    CHECK(info["foldedFaces"] == 0);
}

TEST_CASE("uv symmetry: auto uv cuts both halves alike")
{
    run("newScene");
    std::string s = run("polySphere", {{"radius", 10}, {"subdivisionsAxis", 16}, {"subdivisionsHeight", 12}})[0];
    run("setSymmetry", {{"args", {s}}, {"axis", "x"}});
    run("select", {{"args", {s}}});
    json r = run("uvAuto");
    CHECK(r["ok"] == true);
    scene::Node* n = scene::Graph::current().find(s);
    scene::MeshPtr m = meshOf(s);
    std::optional<geo::SymmetryMap> map = uv::uvSymmetryMap(*n, *m);
    REQUIRE(map);
    std::span<const bool> seam = std::as_const(*m).edgeAttrs().get<bool>(geo::attr::kSeam);
    size_t mismatched = 0, seams = 0;
    for (geo::Index e = 0; e < m->edgeCapacity(); ++e)
        if (m->edgeValid(e) && seam[e])
        {
            ++seams;
            const geo::Index o = map->mirrorEdge(*m, e);
            mismatched += o == geo::kInvalid || !seam[o];
        }
    CHECK(seams > 0);
    CHECK(mismatched == 0);
    CHECK(run("uvLayout")["foldedFaces"] == 0);
}
