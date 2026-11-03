#include <doctest/doctest.h>

#include "TestUtil.h"

#include "geo/Mesh.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"
#include "geo/Topology.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Isolation.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "ui/Isolate.h"

#include <algorithm>
#include <cmath>

using namespace lila;
using geo::Index;
using scene::ComponentMode;
using namespace testutil;
namespace comp = scene::components;
namespace iso = scene::isolation;

namespace
{
struct Plane
{
    std::string name;
    scene::NodeId id = scene::kNoNode;
    scene::MeshPtr mesh;
};

/** A 4 x 4 quad plane on the integer grid from -2 to 2, face centres on the half units. */
Plane plane()
{
    Plane p;
    p.name = run("polyPlane", {{"width", 4}, {"depth", 4}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    p.id = g.find(p.name)->id();
    p.mesh = scene::modifiers::cage(g, p.id);
    return p;
}

/** Makes the set the active view's for the component commands while alive. */
struct ActiveSet
{
    explicit ActiveSet(const iso::Set& s) { iso::setActiveProvider([&s] { return &s; }); }
    ~ActiveSet() { iso::setActiveProvider({}); }
};

size_t shownCount(const std::vector<uint8_t>& mask) { return size_t(std::count(mask.begin(), mask.end(), uint8_t(1))); }
} // namespace

TEST_CASE("isolating objects shows them and their children, nothing else")
{
    run("newScene");
    run("selectMode", {{"args", {"object"}}});
    Plane a = plane(), b = plane();
    run("select", {{"args", {a.name}}});
    scene::Graph& g = scene::Graph::current();
    iso::Set set = iso::fromSelection(g);
    CHECK(set.active());
    CHECK(set.shows(a.id));
    CHECK_FALSE(set.shows(b.id));
    CHECK_FALSE(set.isPartial(a.id));
    CHECK(set.shownFaces(a.id, *a.mesh).empty());

    // The view-side pick filter: a hidden node is never hit, a shown one is.
    const glm::dmat4 world(1.0);
    CHECK_FALSE(ui::isolate::raycast(&set, b.id, b.mesh, world, {0.5, 10.0, 0.5}, {0.0, -1.0, 0.0}));
    CHECK(ui::isolate::raycast(&set, a.id, a.mesh, world, {0.5, 10.0, 0.5}, {0.0, -1.0, 0.0}));
    CHECK(ui::isolate::raycast(nullptr, b.id, b.mesh, world, {0.5, 10.0, 0.5}, {0.0, -1.0, 0.0}));
    CHECK_FALSE(ui::isolate::shows(&set, b.id));
    CHECK(ui::isolate::shows(nullptr, b.id));

    iso::addSelection(set, g); // again: no change
    run("select", {{"args", {b.name}}});
    iso::addSelection(set, g);
    CHECK(set.shows(b.id));
    run("select", {{"args", {a.name}}});
    iso::removeSelection(set, g);
    CHECK_FALSE(set.shows(a.id));
    CHECK(set.shows(b.id));
    run("select", {{"args", {b.name}}});
    iso::removeSelection(set, g);
    // Removing the last one leaves an isolation showing nothing rather than everything.
    CHECK(set.active());
    CHECK_FALSE(set.shows(b.id));
}

TEST_CASE("isolating a child leaves its unselected parent hidden")
{
    run("newScene");
    run("selectMode", {{"args", {"object"}}});
    Plane parent = plane(), child = plane();
    run("parent", {{"args", {child.name, parent.name}}});
    scene::Graph& g = scene::Graph::current();
    g.select({child.id});
    iso::Set set = iso::fromSelection(g);
    CHECK(set.shows(child.id));
    CHECK_FALSE(set.shows(parent.id));

    g.select({parent.id});
    set = iso::fromSelection(g);
    CHECK(set.shows(parent.id));
    CHECK(set.shows(child.id));
}

TEST_CASE("isolating components shows only the faces they touch")
{
    run("newScene");
    Plane p = plane();
    const geo::Mesh& m = *p.mesh;
    scene::Graph& g = scene::Graph::current();
    Index f0 = faceAt(m, -1.5, -1.5), f1 = faceAt(m, -0.5, -1.5);
    run("selectComponents", {{"args", {p.name}}, {"faces", {f0, f1}}});
    REQUIRE(comp::mode() == ComponentMode::Face);
    iso::Set set = iso::fromSelection(g);
    REQUIRE(set.isPartial(p.id));
    std::vector<uint8_t> mask = set.shownFaces(p.id, m);
    CHECK(shownCount(mask) == 2);
    CHECK(mask[f0] == 1);
    CHECK(mask[f1] == 1);

    iso::Visibility vis = iso::visibility(m, mask);
    CHECK(vis.face(f0));
    CHECK_FALSE(vis.face(faceAt(m, 1.5, 1.5)));
    CHECK(vis.point(pointAt(m, -2, -2)));
    CHECK(vis.point(pointAt(m, 0, -1)));
    CHECK_FALSE(vis.point(pointAt(m, 2, 2)));
    CHECK(vis.edge(m.findEdge(pointAt(m, 0, -2), pointAt(m, 0, -1)))); // the border of the region shows
    CHECK_FALSE(vis.edge(m.findEdge(pointAt(m, 1, -2), pointAt(m, 1, -1))));

    // Raycasts go through the hidden faces.
    const glm::dmat4 world(1.0);
    CHECK(ui::isolate::raycast(&set, p.id, p.mesh, world, {-1.5, 10.0, -1.5}, {0.0, -1.0, 0.0}));
    CHECK_FALSE(ui::isolate::raycast(&set, p.id, p.mesh, world, {1.5, 10.0, 1.5}, {0.0, -1.0, 0.0}));
    auto masked = ui::isolate::masked(&set, p.id, p.mesh);
    REQUIRE(masked);
    CHECK(masked->mesh->indices.size() == 2 * 2 * 3);
    CHECK(masked->mesh->shareVertices);
    CHECK(ui::isolate::masked(&set, p.id, p.mesh) == masked); // cached: the mask applies once per change

    // A vertex isolates the faces around it.
    run("selectMode", {{"args", {"vertex"}}});
    run("selectComponents", {{"args", {p.name}}, {"vertices", {pointAt(m, 0, 0)}}});
    iso::Set around = iso::fromSelection(g);
    CHECK(shownCount(around.shownFaces(p.id, m)) == 4);

    // Adding more faces grows it; removing takes faces away.
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {p.name}}, {"faces", {faceAt(m, 1.5, 1.5)}}});
    iso::addSelection(set, g);
    CHECK(shownCount(set.shownFaces(p.id, m)) == 3);
    run("selectComponents", {{"args", {p.name}}, {"faces", {f0}}});
    iso::removeSelection(set, g);
    mask = set.shownFaces(p.id, m);
    CHECK(shownCount(mask) == 2);
    CHECK(mask[f0] == 0);

    // A whole object loses just the removed faces.
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {p.name}}});
    iso::Set whole = iso::fromSelection(g);
    CHECK_FALSE(whole.isPartial(p.id));
    run("selectMode", {{"args", {"face"}}});
    run("selectComponents", {{"args", {p.name}}, {"faces", {f0}}});
    iso::removeSelection(whole, g);
    CHECK(shownCount(whole.shownFaces(p.id, m)) == 15);

    // Saved with the scene by name.
    iso::Set back = iso::fromJson(iso::toJson(set, g), g);
    CHECK(back.shownFaces(p.id, m) == set.shownFaces(p.id, m));
    CHECK(back.version != set.version);
    CHECK_FALSE(iso::fromJson(nullptr, g).active());
}

TEST_CASE("component tools leave hidden faces alone")
{
    run("newScene");
    Plane p = plane();
    const geo::Mesh& m = *p.mesh;
    scene::Graph& g = scene::Graph::current();
    // The left two columns show.
    std::vector<uint32_t> left;
    for (double x : {-1.5, -0.5})
        for (double z : {-1.5, -0.5, 0.5, 1.5})
            left.push_back(faceAt(m, x, z));
    run("selectComponents", {{"args", {p.name}}, {"faces", left}});
    iso::Set set = iso::fromSelection(g);
    ActiveSet activeSet(set);

    // Grow from a face on the region's edge stays inside it.
    run("selectComponents", {{"args", {p.name}}, {"faces", {faceAt(m, -0.5, 0.5)}}});
    run("growSelection");
    for (uint32_t f : comp::selected(p.id, ComponentMode::Face))
        CHECK(m.faceCentroid(f).x < 0.0);
    CHECK(comp::selected(p.id, ComponentMode::Face).size() == 6);

    // Converting from the object takes only what shows.
    run("selectMode", {{"args", {"object"}}});
    run("select", {{"args", {p.name}}});
    run("convertSelection", {{"args", {"face"}}});
    CHECK(comp::selected(p.id, ComponentMode::Face).size() == 8);
    run("convertSelection", {{"args", {"vertex"}}});
    CHECK(comp::selected(p.id, ComponentMode::Vertex).size() == 15);

    // Select All Components and grow on vertices.
    run("selectAllComponents");
    CHECK(comp::selected(p.id, ComponentMode::Vertex).size() == 15);

    // An edge loop across the plane stops where the faces end.
    run("selectMode", {{"args", {"edge"}}});
    Index e = m.findEdge(pointAt(m, -2, 0), pointAt(m, -1, 0));
    json loop = run("selectLoopThrough", {{"args", {p.name}}, {"edge", e}});
    CHECK(loop.size() == 2);
    run("selectComponents", {{"args", {p.name}}, {"edges", {e}}});
    run("selectLoop");
    CHECK(comp::selected(p.id, ComponentMode::Edge).size() == 2);

    // Face loop through a hidden-region crossing edge keeps the shown run.
    json faces = run("selectLoopThrough", {{"args", {p.name}}, {"edge", m.findEdge(pointAt(m, -1, 0), pointAt(m, -1, 1))}, {"faces", true}});
    CHECK(faces.size() == 2);
}

TEST_CASE("chains clip at hidden elements, wrapping when closed")
{
    std::vector<uint32_t> chain{0, 1, 2, 3, 4, 5};
    auto hide3 = [](uint32_t x) { return x != 3; };
    CHECK(iso::clipChain(chain, 1, false, hide3) == std::vector<uint32_t>{0, 1, 2});
    CHECK(iso::clipChain(chain, 1, true, hide3) == std::vector<uint32_t>{4, 5, 0, 1, 2});
    CHECK(iso::clipChain(chain, 3, true, hide3).empty());
    CHECK(iso::clipChain(chain, 2, true, [](uint32_t) { return true; }) == chain);
}

TEST_CASE("smooth preview faces map back to the isolated ones")
{
    geo::Mesh m = geo::makePlane({.width = 2.0, .depth = 2.0, .subdivX = 2, .subdivZ = 1});
    std::vector<uint8_t> shown(m.faceCapacity(), 0);
    shown[0] = 1;
    for (int levels : {1, 2})
    {
        geo::SubdivideOptions o;
        o.levels = levels;
        geo::Mesh smooth = geo::subdivide(m, o);
        std::vector<uint8_t> mask = iso::subdividedFaces(m, levels, shown, smooth.faceCapacity());
        REQUIRE(mask.size() == smooth.faceCapacity());
        CHECK(shownCount(mask) == smooth.faceCapacity() / 2);
        // The shown half is the side face 0 lies on.
        const double side = m.faceCentroid(0).x;
        for (Index f = 0; f < smooth.faceCapacity(); ++f)
            CHECK((smooth.faceCentroid(f).x * side > 0.0) == bool(mask[f]));
    }
}
