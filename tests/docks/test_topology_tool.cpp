#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "geo/Primitives.h"
#include "retopo/RetopoCommands.h"
#include "retopo/TopologyCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/TopologyTool.h"
#include "ui/TopologyToolkit.h"
#include "ui/ViewportTool.h"

#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QWheelEvent>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace lila;
using json = nlohmann::json;

namespace
{
void setup()
{
    static bool done = false;
    if (!done)
    {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        retopo::registerCommands();
        done = true;
    }
    ui::viewportTools::set(nullptr);
    scene::Graph::current().clear();
    UndoStack::get().clear();
    PrefStore::get().set("general", "/topology/edgeLength", 10.0);
    PrefStore::get().set("general", "/topology/brush", 5.0);
}

scene::Node* meshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

/** Looking down -Y at the XZ plane, 1 px per cm, world origin at pixel (100, 100), +X right and +Z down the screen. */
ui::ViewInfo topView()
{
    ui::ViewInfo v;
    v.eye = {0.0, 200.0, 0.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0.0, 0.0, -1.0));
    v.proj = glm::ortho(-100.0, 100.0, -100.0, 100.0, 1.0, 400.0);
    v.width = v.height = 200.0;
    v.ortho = true;
    v.camera = "top";
    return v;
}

/** Looking down -Z, 1 px per cm, world origin at pixel (100, 100), +Y up the screen. */
ui::ViewInfo frontView()
{
    ui::ViewInfo v;
    v.eye = {0.0, 0.0, 200.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0));
    v.proj = glm::ortho(-100.0, 100.0, -100.0, 100.0, 1.0, 400.0);
    v.width = v.height = 200.0;
    v.ortho = true;
    v.camera = "front";
    return v;
}

QPointF px(double x, double z) { return QPointF(100.0 + x, 100.0 + z); }

ui::TopologyTool* tool() { return ui::activeTopologyTool(); }

void begin(ui::TopoMode mode)
{
    scene::Node* ref = scene::Graph::current().find("ref");
    REQUIRE(ref);
    ui::viewportTools::set(std::make_unique<ui::TopologyTool>(ref->id(), "topo", mode));
    REQUIRE(tool());
}

/** A left drag through the points, optionally with modifiers, as the viewport delivers it. */
void drag(const std::vector<QPointF>& pts, const ui::ViewInfo& view, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    REQUIRE(pts.size() >= 1);
    QMouseEvent press(QEvent::MouseButtonPress, pts.front(), pts.front(), Qt::LeftButton, Qt::LeftButton, mods);
    tool()->mousePress(&press, view);
    for (size_t i = 1; i < pts.size(); ++i)
    {
        QMouseEvent move(QEvent::MouseMove, pts[i], pts[i], Qt::NoButton, Qt::LeftButton, mods);
        tool()->mouseMove(&move, view);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, pts.back(), pts.back(), Qt::LeftButton, Qt::NoButton, mods);
    tool()->mouseRelease(&release, view);
}

/** Points every 2 px along straight segments through the corners. */
std::vector<QPointF> path(const std::vector<QPointF>& corners)
{
    std::vector<QPointF> out{corners.front()};
    for (size_t i = 1; i < corners.size(); ++i)
    {
        QPointF a = corners[i - 1], b = corners[i];
        int steps = std::max(1, int(std::hypot(b.x() - a.x(), b.y() - a.y()) / 2.0));
        for (int k = 1; k <= steps; ++k)
            out.push_back(a + (b - a) * (double(k) / steps));
    }
    return out;
}

void toolWheel(QPointF at, int notches, const ui::ViewInfo& view, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWheelEvent e(at, at, QPoint(), QPoint(0, 120 * notches), Qt::NoButton, mods, Qt::NoScrollPhase, false);
    tool()->wheel(&e, view);
}

void toolKey(int k)
{
    QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
    tool()->keyPress(&e, topView());
}

const geo::Mesh* topoMesh()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find("topo");
    return n ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh).get() : nullptr;
}

std::vector<retopo::ConstructRecord> records()
{
    scene::Node* n = scene::Graph::current().find("topo");
    return n ? retopo::retopoConstructs(scene::Graph::current(), *n) : std::vector<retopo::ConstructRecord>{};
}

geo::Mesh plane()
{
    geo::PlaneOptions o;
    o.width = 160.0;
    o.depth = 160.0;
    o.subdivX = 40;
    o.subdivZ = 40;
    return geo::makePlane(o);
}

bool allQuads(const geo::Mesh& m)
{
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceSize(f) != 4)
            return false;
    return true;
}
} // namespace

TEST_CASE("topology tool: patch, wheel and pull on a closed shape")
{
    setup();
    meshNode("ref", plane());
    begin(ui::TopoMode::Patch);
    ui::ViewInfo view = topView();
    drag(path({px(-40, -30), px(40, -30), px(40, 30), px(-40, 30), px(-40, -28)}), view);
    const geo::Mesh* m = topoMesh();
    REQUIRE(m);
    CHECK(m->numFaces() > 4);
    CHECK(allQuads(*m));
    std::vector<retopo::ConstructRecord> recs = records();
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].kind == retopo::ConstructKind::Patch);
    CHECK(tool()->activeConstruct() == recs[0].id);
    size_t faces = m->numFaces();

    toolWheel(px(0, -29), 1, view);
    CHECK(topoMesh()->numFaces() != faces);
    CHECK(records().size() == 1);
    CHECK(records()[0].live);

    drag(path({px(0, 0), px(20, 10)}), view);
    CHECK(records()[0].spec.contains("pull"));
    CHECK(allQuads(*topoMesh()));

    toolKey(Qt::Key_Return);
    CHECK(records().empty());
    CHECK(topoMesh()->numFaces() > 4);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: strip, strokes, bridge and sweep")
{
    setup();
    meshNode("ref", plane());
    ui::ViewInfo view = topView();

    begin(ui::TopoMode::Strip);
    drag(path({px(-60, -60), px(0, -60), px(0, 0)}), view);
    std::vector<retopo::ConstructRecord> recs = records();
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].kind == retopo::ConstructKind::Strip);
    REQUIRE(topoMesh());
    CHECK(topoMesh()->numFaces() > 3);
    CHECK(allQuads(*topoMesh()));

    tool()->setMode(ui::TopoMode::Stroke);
    drag(path({px(20, 20), px(70, 20)}), view);
    recs = records();
    REQUIRE(recs.size() == 2);
    CHECK(recs[1].kind == retopo::ConstructKind::Stroke);
    size_t before = topoMesh()->numFaces();
    drag(path({px(20, 50), px(70, 50)}), view);
    recs = records();
    REQUIRE(recs.size() == 2);
    CHECK(recs[1].kind == retopo::ConstructKind::Bridge);
    CHECK(topoMesh()->numFaces() > before);

    // From the bridge's far corner, outward: a sweep of the border beside it.
    before = topoMesh()->numFaces();
    drag(path({px(70, 50), px(70, 75)}), view);
    recs = records();
    REQUIRE(recs.size() == 3);
    CHECK(recs[2].kind == retopo::ConstructKind::Sweep);
    CHECK(topoMesh()->numFaces() > before);
    for (geo::Index e = 0; e < topoMesh()->edgeCapacity(); ++e)
        if (topoMesh()->edgeValid(e))
            CHECK(topoMesh()->edgeFaceCount(e) <= 2);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: cylinder rings, pencil guides, auto region and handles")
{
    setup();
    SUBCASE("cylinder")
    {
        geo::CylinderOptions o;
        o.radius = 20.0;
        o.height = 120.0;
        o.subdivAxis = 48;
        o.subdivHeight = 12;
        o.caps = false;
        meshNode("ref", geo::makeCylinder(o));
        begin(ui::TopoMode::Cylinder);
        ui::ViewInfo view = frontView();
        // Lines across the upright tube at two heights (screen y down is world -Y).
        drag({QPointF(60, 140), QPointF(140, 140)}, view);
        std::vector<retopo::ConstructRecord> recs = records();
        REQUIRE(recs.size() == 1);
        CHECK(recs[0].kind == retopo::ConstructKind::Cylinder);
        drag({QPointF(60, 60), QPointF(140, 60)}, view);
        REQUIRE(topoMesh());
        CHECK(topoMesh()->numFaces() >= 16);
        CHECK(allQuads(*topoMesh()));
        size_t faces = topoMesh()->numFaces();
        // Across the middle of the cylinder: a ring inserted, not a new cylinder.
        drag({QPointF(60, 100), QPointF(140, 100)}, view);
        CHECK(records().size() == 1);
        CHECK(records()[0].spec["rings"].size() == 3);
        toolWheel(QPointF(100, 80), 1, view);
        CHECK(records()[0].spec["points"].get<int>() == 9);
        CHECK(topoMesh()->numFaces() != faces);
    }
    SUBCASE("pencil")
    {
        meshNode("ref", plane());
        begin(ui::TopoMode::Pencil);
        ui::ViewInfo view = topView();
        drag(path({px(-50, 0), px(50, 0)}), view);
        drag(path({px(0, -50), px(0, 50)}), view);
        scene::Node* topo = scene::Graph::current().find("topo");
        REQUIRE(topo);
        CHECK(retopo::retopoGuides(*topo).size() == 2);
        drag(path({px(-10, -40), px(10, -40)}), view, Qt::ControlModifier);
        CHECK(retopo::retopoGuides(*topo).size() == 1);
    }
    SUBCASE("auto region and handles")
    {
        meshNode("ref", plane());
        begin(ui::TopoMode::Region);
        ui::ViewInfo view = topView();
        drag(path({px(-30, 0), px(30, 0)}), view);
        CHECK(tool()->regionFaceCount() > 20);
        toolKey(Qt::Key_Return);
        std::vector<retopo::ConstructRecord> recs = records();
        REQUIRE(recs.size() == 1);
        CHECK(recs[0].kind == retopo::ConstructKind::Region);
        REQUIRE(topoMesh());
        CHECK(topoMesh()->numFaces() > 4);
        CHECK(tool()->regionFaceCount() == 0);

        tool()->setMode(ui::TopoMode::Handles);
        drag(path({px(-20, 0), px(20, 0)}), view);
        geo::Mesh before = *topoMesh();
        drag(path({px(-20, 0), px(-20, 15)}), view);
        const geo::Mesh* after = topoMesh();
        double moved = 0.0;
        for (geo::Index p = 0; p < before.pointCapacity() && p < after->pointCapacity(); ++p)
            if (before.pointValid(p) && after->pointValid(p))
                moved = std::max(moved, glm::length(after->position(p) - before.position(p)));
        CHECK(moved > 3.0);
    }
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology toolkit lists the constructs and follows the active one")
{
    setup();
    meshNode("ref", plane());
    begin(ui::TopoMode::Strip);
    drag(path({px(-60, -60), px(0, -60)}), topView());
    ui::TopologyToolkit kit;
    kit.show();
    auto* list = kit.findChild<QListWidget*>();
    REQUIRE(list);
    CHECK(list->count() == 1);
    ui::viewportTools::set(nullptr);
}
