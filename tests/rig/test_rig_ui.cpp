#include "TestUtil.h"

#include "scene/Skin.h"
#include "ui/Camera.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/WeightPaintTool.h"

#include <QMouseEvent>

using namespace rigtest;
using namespace lila::scene;

namespace
{
std::string skinnedCylinder()
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 20}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    return mesh;
}

lila::ui::ViewInfo frontView()
{
    lila::ui::Camera cam;
    cam.yaw = 0.0;
    cam.pitch = 0.0;
    cam.distance = 300.0;
    lila::ui::ViewInfo v;
    v.view = cam.view();
    v.proj = cam.proj(1.0);
    v.eye = cam.eye();
    v.width = 400;
    v.height = 400;
    return v;
}

double total(const std::string& mesh, const std::string& joint)
{
    double sum = 0.0;
    for (const json& p : run("skinPercent", {{"args", {mesh}}}))
        sum += p["weights"].value(joint, 0.0);
    return sum;
}

bool grayscale(const std::vector<lila::render::LineVertex>& v)
{
    for (const auto& x : v)
        if (x.color.r != x.color.g || x.color.g != x.color.b)
            return false;
    return true;
}

void mouse(lila::ui::ViewportTool* tool, QEvent::Type type, QPointF at, const lila::ui::ViewInfo& view)
{
    Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, at, Qt::LeftButton, held, Qt::NoModifier);
    if (type == QEvent::MouseButtonPress)
        tool->mousePress(&e, view);
    else if (type == QEvent::MouseMove)
        tool->mouseMove(&e, view);
    else
        tool->mouseRelease(&e, view);
}
} // namespace

TEST_CASE("joints draw in grayscale and pick by bone")
{
    skinnedCylinder();
    lila::ui::ViewInfo view = frontView();
    lila::render::FrameState s;
    lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
    for (const auto& d : lila::ui::viewportHooks::drawers())
        d(s, painter, view);
    CHECK(s.lines.size() > 100);
    CHECK(grayscale(s.lines));

    // A click on the bone between bone_b and bone_c picks bone_b.
    auto at = view.project({0, 25, 0});
    REQUIRE(at);
    std::string best;
    double px = 1e9;
    for (const auto& p : lila::ui::viewportHooks::pickers())
        for (const auto& h : p(*at, *at, true, view))
            if (h.pixels < px)
            {
                px = h.pixels;
                best = h.name;
            }
    CHECK(best == "bone_b");
}

TEST_CASE("a weight paint stroke paints the current influence as one undo step")
{
    std::string mesh = skinnedCylinder();
    run("select", {{"args", {mesh, "bone_c"}}});
    lila::ui::startWeightPaint();
    lila::ui::ViewportTool* tool = lila::ui::viewportTools::active();
    REQUIRE(tool);
    CHECK(std::string(tool->name()) == "weightPaint");

    lila::ui::ViewInfo view = frontView();
    double before = total(mesh, "bone_c");
    // Default brush: replace with 1. Stroke across the middle of the tube, where bone_c had little weight.
    QPointF mid(200, 200);
    mouse(tool, QEvent::MouseButtonPress, mid, view);
    mouse(tool, QEvent::MouseMove, mid + QPointF(15, 0), view);
    mouse(tool, QEvent::MouseButtonRelease, mid + QPointF(15, 0), view);
    double after = total(mesh, "bone_c");
    CHECK(after > before + 1.0);
    CHECK(run("skinInfo", {{"args", {mesh}}})["unnormalized"] == 0);

    lila::render::FrameState s;
    lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
    tool->draw(s, painter, view);
    CHECK(!s.tris3d.empty());
    CHECK(!grayscale(s.tris3d));

    run("undo");
    CHECK(total(mesh, "bone_c") == doctest::Approx(before));
    run("redo");
    CHECK(total(mesh, "bone_c") == doctest::Approx(after));
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("brush math keeps sums and locks")
{
    std::string mesh = skinnedCylinder();
    NodeId sk = skin::find(graph(), node(mesh).id());
    WeightTable t = skin::table(graph(), sk);
    auto infs = skin::influences(graph(), sk);
    std::vector<char> locked(3, 0);
    locked[size_t(skin::indexOf(infs, "bone_a"))] = 1;
    uint32_t c = uint32_t(skin::indexOf(infs, "bone_c"));
    for (size_t p = 0; p < t.points.size(); ++p)
    {
        float lockedBefore = t.get(p, uint32_t(skin::indexOf(infs, "bone_a")));
        skin::paintPoint(t, p, c, skin::PaintMode::Add, false, 0.5f, 1.0f, locked, 4);
        float sum = 0.0f;
        for (auto& [i, w] : t.points[p])
            sum += w;
        CHECK(sum == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(t.get(p, uint32_t(skin::indexOf(infs, "bone_a"))) == doctest::Approx(lockedBefore));
    }
}
