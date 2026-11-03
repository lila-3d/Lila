#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "scene/Constraint.h"
#include "scene/Modifiers.h"

#include <functional>

using namespace rigtest;
using namespace lila::scene;
using D3 = glm::dvec3;

namespace
{
void withCurves()
{
    curve::registerTypes();
    if (!ops::find("curve"))
        curve::registerCommands();
}

std::string plane(const char* name, double size, int subdivisions)
{
    return run("polyPlane", {{"name", name}, {"width", size}, {"depth", size}, {"subdivisionsX", subdivisions}, {"subdivisionsZ", subdivisions}})[0];
}

geo::Index vertexAt(const std::string& mesh, const D3& p)
{
    MeshPtr m = outMesh(mesh);
    for (geo::Index i = 0; i < m->pointCapacity(); ++i)
        if (m->pointValid(i) && glm::length(m->position(i) - p) < 1e-6)
            return i;
    FAIL("no vertex at the position");
    return 0;
}

/** Moves the base mesh's points by offset(point id, position). */
void editPoints(const std::string& mesh, const std::function<D3(geo::Index, const D3&)>& offset)
{
    Graph& g = graph();
    PlugRef plug = modifiers::editPlug(g, g.find(mesh)->id());
    auto m = std::make_shared<geo::Mesh>(*std::get<MeshPtr>(g.value(plug)));
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        m->setPosition(p, m->position(p) + offset(p, m->position(p)));
    g.setValue(plug, MeshPtr(m));
}

/** The pin node driving a pin locator. */
std::string pinNode(const std::string& locator)
{
    auto src = graph().source({node(locator).id(), Transform::kTranslate, -1});
    REQUIRE(src);
    return graph().node(src->node)->name();
}

D3 axis(const std::string& name, int k) { return D3(world(name)[k]); }

std::string locator(const char* name, const D3& at)
{
    std::string l = run("spaceLocator", {{"name", name}}).get<std::string>();
    run("move", {{"args", {l}}, {"by", v3(at.x, at.y, at.z)}});
    return l;
}
} // namespace

TEST_CASE("pin rides a curve by fraction, length and parameter, framed by the rail, an up object and roll")
{
    withCurves();
    run("newScene");
    std::string wire = run("curve", {{"type", "poly"}, {"points", {v3(0, 0, 0), v3(10, 0, 0)}}, {"name", "Wire"}});
    json half = run("pin", {{"args", {wire}}, {"along", 0.5}});
    REQUIRE(half.size() == 1);
    checkNear(pos(half[0]), D3(5, 0, 0));
    checkNear(axis(half[0], 0), D3(1, 0, 0));
    CHECK(node(pinNode(half[0])).type().name == "pin");

    checkNear(pos(run("pin", {{"args", {wire}}, {"curveMode", "length"}, {"along", 3.0}})[0]), D3(3, 0, 0));
    checkNear(pos(run("pin", {{"args", {wire}}, {"curveMode", "parameter"}, {"along", 0.25}})[0]), D3(2.5, 0, 0));

    std::string up = locator("Up", D3(5, 0, 10));
    std::string aimed = run("pin", {{"args", {wire}}, {"along", 0.5}, {"upMode", "upObject"}, {"upObject", up}})[0];
    checkNear(axis(aimed, 1), D3(0, 0, 1));
    run("setAttr", {{"args", {pinNode(aimed) + ".roll", 90.0}}});
    checkNear(axis(aimed, 1), D3(0, -1, 0));
    checkNear(axis(aimed, 0), D3(1, 0, 0));

    // An object pinned with no along lands nearest itself and keeps its offset as the curve moves.
    std::string bead = locator("Bead", D3(7, 2, 0));
    run("pin", {{"args", {bead, wire}}});
    checkNear(pos(bead), D3(7, 2, 0));
    run("move", {{"args", {wire}}, {"by", v3(0, 5, 0)}});
    checkNear(pos(bead), D3(7, 7, 0));
    checkNear(pos(half[0]), D3(5, 5, 0));
}

TEST_CASE("pin rides a vertex, an edge and a face, framed by the normal and an edge")
{
    run("newScene");
    std::string p = plane("Plate", 10, 2);
    const geo::Index centre = vertexAt(p, D3(0, 0, 0));
    const geo::Index right = vertexAt(p, D3(5, 0, 0));
    json v = run("pin", {{"args", {p}}, {"source", "vertex"}, {"index", int64_t(centre)}, {"neighbor", int64_t(right)}});
    checkNear(pos(v[0]), D3(0, 0, 0));
    checkNear(axis(v[0], 2), D3(0, 1, 0));
    checkNear(axis(v[0], 0), D3(1, 0, 0));

    MeshPtr m = outMesh(p);
    const geo::Index e = m->findEdge(centre, right);
    REQUIRE(e != geo::kInvalid);
    const D3 a = m->position(m->edgePoint(e, 0)), b = m->position(m->edgePoint(e, 1));
    json ed = run("pin", {{"args", {p}}, {"source", "edge"}, {"index", int64_t(e)}, {"fraction", 0.25}});
    checkNear(pos(ed[0]), glm::mix(a, b, 0.25));
    checkNear(axis(ed[0], 2), D3(0, 1, 0));
    checkNear(axis(ed[0], 0), glm::normalize(b - a));

    json fc = run("pin", {{"args", {p}}, {"source", "face"}, {"index", int64_t(0)}});
    D3 centroid(0.0);
    for (geo::Index q : m->facePoints(0))
        centroid += m->position(q) / double(m->faceSize(0));
    checkNear(pos(fc[0]), centroid);
    checkNear(axis(fc[0], 2), D3(0, 1, 0));

    // Barycentric: the point on the face nearest the object, which keeps its offset as the surface moves.
    std::string button = locator("Button", D3(1, 2, 1));
    run("pin", {{"args", {button, p}}, {"source", "face"}, {"faceMode", "barycentric"}});
    checkNear(pos(button), D3(1, 2, 1));
    editPoints(p, [](geo::Index, const D3&) { return D3(0, 3, 0); });
    checkNear(pos(button), D3(1, 5, 1));
    checkNear(pos(v[0]), D3(0, 3, 0));
    checkNear(pos(ed[0]), glm::mix(a, b, 0.25) + D3(0, 3, 0));

    // A surface pin is the same node in its surface mode.
    json s = run("surfacePin", {{"args", {p}}, {"mode", "nearest"}, {"position", v3(2, 0, 2)}});
    CHECK(node(pinNode(s[0])).type().name == "pin");
    checkNear(pos(s[0]), D3(2, 3, 2));
}

TEST_CASE("pin keeps the offset or snaps onto the pin, and follows translate only when asked")
{
    run("newScene");
    std::string p = plane("Plate", 10, 2);
    const geo::Index centre = vertexAt(p, D3(0, 0, 0));

    std::string kept = locator("Kept", D3(1, 2, 3));
    run("rotate", {{"args", {kept}}, {"by", v3(0, 30, 0)}});
    const glm::dmat4 before = world(kept);
    run("pin", {{"args", {kept, p}}, {"source", "vertex"}, {"index", int64_t(centre)}});
    const glm::dmat4 after = world(kept);
    for (int c = 0; c < 4; ++c)
        checkNear(D3(after[c]), D3(before[c]));

    std::string snapped = locator("Snapped", D3(1, 2, 3));
    json pins = run("pin", {{"args", {snapped, p}}, {"source", "vertex"}, {"index", int64_t(centre)}, {"maintainOffset", false}});
    checkNear(pos(snapped), D3(0, 0, 0));
    checkNear(axis(snapped, 2), axis(pins[0], 2));

    // Tilt the surface about the vertex: translate-only keeps its orientation, translate+rotate turns with it.
    std::string still = locator("Still", D3(0, 1, 0));
    run("pin", {{"args", {still, p}}, {"source", "vertex"}, {"index", int64_t(centre)}, {"follow", "translate"}});
    run("rotate", {{"args", {p}}, {"by", v3(0, 0, 90)}});
    checkNear(axis(still, 0), D3(1, 0, 0));
    checkNear(pos(still), D3(0, 1, 0));
    const glm::dmat4 turned = glm::dmat4(glm::dmat3(D3(0, 1, 0), D3(-1, 0, 0), D3(0, 0, 1))) * before;
    for (int c = 0; c < 4; ++c)
        checkNear(D3(world(kept)[c]), D3(turned[c]));
    checkNear(axis(snapped, 2), D3(-1, 0, 0));
}

TEST_CASE("pin rides the deformed shape or the rest shape of a skinned mesh")
{
    run("newScene");
    std::string body = run("polyPlane", {{"name", "Body"}, {"width", 20}, {"depth", 20}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(-10, 0, 0)}, {"name", "hip"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "knee"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "ankle"}});
    run("skinCluster", {{"args", {"hip", body}}});
    const geo::Index v = vertexAt(body, D3(5, 0, 5));
    std::string onFinal = locator("OnFinal", D3(5, 0, 5));
    std::string onRest = locator("OnRest", D3(5, 0, 5));
    run("pin", {{"args", {onFinal, body}}, {"source", "vertex"}, {"index", int64_t(v)}});
    run("pin", {{"args", {onRest, body}}, {"source", "vertex"}, {"index", int64_t(v)}, {"shape", "rest"}});
    run("setAttr", {{"args", {"knee.rotateZ", 40.0}}});
    const D3 moved = outMesh(body)->position(v);
    CHECK(glm::length(moved - D3(5, 0, 5)) > 1.0);
    checkNear(pos(onFinal), moved);
    checkNear(pos(onRest), D3(5, 0, 5));
}

TEST_CASE("several pins on one object blend by weight like a parent constraint")
{
    run("newScene");
    std::string p = plane("Plate", 10, 2);
    const geo::Index left = vertexAt(p, D3(-5, 0, 0)), right = vertexAt(p, D3(5, 0, 0));
    std::string prop = locator("Prop", D3(0, 1, 0));
    json a = run("pin", {{"args", {prop, p}}, {"source", "vertex"}, {"index", int64_t(left)}, {"follow", "translate"}});
    json b = run("pin", {{"args", {prop, p}}, {"source", "vertex"}, {"index", int64_t(right)}, {"follow", "translate"}});
    checkNear(pos(prop), D3(0, 1, 0));
    editPoints(p, [&](geo::Index i, const D3&) { return i == left ? D3(0, 4, 0) : D3(0.0); });
    checkNear(pos(prop), D3(0, 3, 0));
    run("setAttr", {{"args", {pinNode(a[0]) + ".weight", 0.0}}});
    checkNear(pos(prop), D3(5, 1, 0));
    run("setAttr", {{"args", {pinNode(a[0]) + ".weight", 3.0}}});
    checkNear(pos(prop), D3(-2.5, 4, 0));

    // Parent-style pins keep the object where it is and carry it when the whole source moves.
    std::string crate = locator("Crate", D3(0, 2, 1));
    run("pin", {{"args", {crate, p}}, {"source", "vertex"}, {"index", int64_t(left)}});
    run("pin", {{"args", {crate, p}}, {"source", "vertex"}, {"index", int64_t(right)}});
    const D3 at = pos(crate);
    checkNear(at, D3(0, 2, 1));
    run("move", {{"args", {p}}, {"by", v3(0, 0, 7)}});
    checkNear(pos(crate), at + D3(0, 0, 7));
}

TEST_CASE("pin follow full scales the object with the source")
{
    run("newScene");
    std::string p = plane("Plate", 10, 2);
    std::string prop = locator("Prop", D3(2, 1, 2));
    run("pin", {{"args", {prop, p}}, {"source", "face"}, {"follow", "full"}});
    checkNear(node(prop).as<D3>(Transform::kScale), D3(1.0));
    editPoints(p, [](geo::Index, const D3& q) { return q; });
    graph().evaluate();
    checkNear(node(prop).as<D3>(Transform::kScale), D3(2.0));
}

TEST_CASE("pin rebinds through uv when the mesh's topology changes")
{
    run("newScene");
    std::string p = plane("Plate", 10, 2);
    const geo::Index corner = vertexAt(p, D3(5, 0, 5));
    json v = run("pin", {{"args", {p}}, {"source", "vertex"}, {"index", int64_t(corner)}});
    json e = run("pin", {{"args", {p}}, {"source", "edge"}, {"position", v3(2.5, 0, 0)}});
    checkNear(pos(v[0]), D3(5, 0, 5));
    const D3 edgeAt = pos(e[0]);

    Graph& g = graph();
    const NodeId gen = modifiers::stackSource(g, g.find(p)->id());
    REQUIRE(gen != kNoNode);
    for (const char* attr : {"subdivisionsX", "subdivisionsZ"})
        g.setValue({gen, g.node(gen)->type().attrIndex(attr), -1}, int64_t(5));
    CHECK(outMesh(p)->numPoints() == 36);
    checkNear(pos(v[0]), D3(5, 0, 5));
    checkNear(pos(e[0]), edgeAt);
}

TEST_CASE("bakePin keys a pinned joint and lets the pins go")
{
    withCurves();
    run("newScene");
    std::string wire = run("curve", {{"type", "poly"}, {"points", {v3(0, 0, 0), v3(10, 0, 0)}}, {"name", "Wire"}});
    key(wire + ".ty", 0.0, 0.0);
    key(wire + ".ty", 1.0, 6.0);
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(4, 1, 0)}, {"name", "tip"}});
    run("pin", {{"args", {"tip", wire}}});

    Graph& g = graph();
    std::vector<D3> before;
    for (double t : {0.0, 0.5, 1.0})
    {
        g.setTime(t);
        before.push_back(pos("tip"));
    }
    json joints = run("bakePin", {{"args", {"tip"}}, {"start", 0.0}, {"end", 1.0}, {"fps", 10.0}});
    REQUIRE(joints.size() == 1);
    CHECK(node(joints[0]).name() == "tip");
    for (NodeId id : g.nodes())
        if (g.node(id)->type().isA("constraint"))
            FAIL("a constraint is left: " << g.node(id)->name());
    int i = 0;
    for (double t : {0.0, 0.5, 1.0})
    {
        g.setTime(t);
        checkNear(pos("tip"), before[size_t(i++)], 1e-5);
    }
    checkNear(before[1], D3(4, 4, 0), 1e-5);
}

TEST_CASE("bakePin makes a socket for a prop riding one joint rigidly, else a keyed bone")
{
    run("newScene");
    std::string p = plane("Board", 10, 2);
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("skinCluster", {{"args", {"root", p}}});
    key("root.ry", 0.0, 0.0);
    key("root.ry", 1.0, 90.0);
    Graph& g = graph();
    g.setTime(0.0);

    const geo::Index v = vertexAt(p, D3(5, 0, 5));
    std::string held = locator("Held", D3(5, 1, 5));
    run("pin", {{"args", {held, p}}, {"source", "vertex"}, {"index", int64_t(v)}});
    json socket = run("bakePin", {{"args", {held}}, {"start", 0.0}, {"end", 1.0}, {"fps", 10.0}});
    REQUIRE(socket.size() == 1);
    Node& s = node(socket[0]);
    CHECK(s.name() == "Held_socket");
    REQUIRE(g.parent(s.id()) != kNoNode);
    CHECK(g.node(g.parent(s.id()))->name() == "root");
    CHECK_FALSE(g.source({s.id(), Transform::kTranslate, -1}).has_value());
    for (double t : {0.0, 0.5})
    {
        g.setTime(t);
        checkNear(pos(socket[0]), pos(held), 1e-5);
    }

    // A pin sliding along an edge moves relative to the joint: a bone keyed every frame.
    g.setTime(0.0);
    const geo::Index far = vertexAt(p, D3(-5, 0, 5));
    MeshPtr m = outMesh(p);
    const geo::Index mid = vertexAt(p, D3(0, 0, 5));
    const geo::Index e = m->findEdge(far, mid);
    REQUIRE(e != geo::kInvalid);
    std::string slider = locator("Slider", D3(-2.5, 0, 5));
    json pins = run("pin", {{"args", {slider, p}}, {"source", "edge"}, {"index", int64_t(e)}, {"fraction", 0.5}});
    key(pinNode(pins[0]) + ".fraction", 0.0, 0.0);
    key(pinNode(pins[0]) + ".fraction", 1.0, 1.0);
    json bone = run("bakePin", {{"args", {slider}}, {"start", 0.0}, {"end", 1.0}, {"fps", 10.0}});
    REQUIRE(bone.size() == 1);
    Node& b = node(bone[0]);
    CHECK(b.name() == "Slider_bone");
    CHECK(g.node(g.parent(b.id()))->name() == "root");
    CHECK(g.source({b.id(), Transform::kTranslate, 0}).has_value());
    for (double t : {0.0, 0.3, 0.5, 1.0})
    {
        g.setTime(t);
        checkNear(pos(bone[0]), pos(slider), 1e-4);
    }

    // A pin driving nothing bakes at its own frame.
    g.setTime(0.0);
    json bare = run("pin", {{"args", {p}}, {"source", "vertex"}, {"index", int64_t(v)}});
    json bareJoint = run("bakePin", {{"args", bare}, {"start", 0.0}, {"end", 1.0}, {"fps", 10.0}});
    REQUIRE(bareJoint.size() == 1);
    CHECK(node(bareJoint[0]).name() == bare[0].get<std::string>() + "_socket");
    checkNear(pos(bareJoint[0]), pos(bare[0]), 1e-6);

    std::string loose = plane("Loose", 4, 1);
    json loosePin = run("pin", {{"args", {loose}}, {"source", "surface"}});
    CHECK_THROWS(run("bakePin", {{"args", loosePin}}));
}
