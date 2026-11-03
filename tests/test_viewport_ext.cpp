#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/ExtensionContext.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Camera.h"
#include "ui/CursorBadges.h"
#include "ui/DrawOverrides.h"
#include "ui/Gizmo.h"
#include "ui/Manipulator.h"
#include "ui/Overlay.h"
#include "ui/RenderPasses.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportRenderers.h"
#include "ui/ViewportTool.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

using namespace lila;
using namespace lila::ui;
using json = nlohmann::json;
using namespace testutil;

namespace
{
ViewInfo frontView()
{
    Camera cam;
    cam.yaw = 0.0;
    cam.pitch = 0.0;
    cam.distance = 300.0;
    ViewInfo v;
    v.view = cam.view();
    v.proj = cam.proj(1.0);
    v.eye = cam.eye();
    v.width = 400;
    v.height = 400;
    return v;
}

/** Records what the viewport asked of it. */
class TestGizmo final : public Gizmo
{
public:
    explicit TestGizmo(int* alive) : mAlive(alive) { ++*mAlive; }
    ~TestGizmo() override { --*mAlive; }
    void setView(const ViewInfo&) override {}
    bool supports(std::string_view mode) const override { return mode == "rotate" || mode == "testTool"; }
    bool visible() override { return true; }
    void draw(OverlayPainter&) override {}
    bool hover(QPointF) override { return false; }
    bool press(QPointF, Qt::MouseButton, Qt::KeyboardModifiers) override { return mDragging = true; }
    void drag(QPointF, Qt::KeyboardModifiers) override {}
    void release() override { mDragging = false; }
    void cancel() override { mDragging = false; }
    bool dragging() const override { return mDragging; }
    bool hovering() const override { return false; }

private:
    int* mAlive;
    bool mDragging = false;
};

class TestTool final : public ViewportTool
{
public:
    explicit TestTool(int* alive) : mAlive(alive) { ++*mAlive; }
    ~TestTool() override { --*mAlive; }
    const char* name() const override { return "testTool"; }

private:
    int* mAlive;
};
} // namespace

TEST_CASE("gizmo providers: the default is lila.trs, setActive switches, removal falls back")
{
    REQUIRE(gizmos::find(gizmos::kDefault));
    CHECK(gizmos::activeId() == gizmos::kDefault);
    std::unique_ptr<Gizmo> trs = gizmos::create(gizmos::kDefault);
    REQUIRE(trs);
    CHECK(dynamic_cast<Manipulator*>(trs.get()));
    for (const char* mode : {"select", "move", "rotate", "scale"})
        CHECK(trs->supports(mode));
    CHECK_FALSE(trs->supports("weightPaint"));
    CHECK_FALSE(trs->dragging());
    CHECK(trs->readout().isEmpty());
    CHECK(trs->typed().isEmpty());

    int alive = 0;
    int notified = 0;
    int listener = gizmos::addListener([&] { ++notified; });
    {
        ext::Context ctx("spinnerExt");
        const gizmos::Provider* p = gizmos::add(ctx, {.id = "spinner", .label = "Spinner", .create = [&] { return std::make_unique<TestGizmo>(&alive); }});
        REQUIRE(p);
        CHECK(*gizmos::registry().ownerOf("spinner") == "spinnerExt");
        // Taken ids are refused, not replaced.
        CHECK_FALSE(gizmos::add(ctx, {.id = gizmos::kDefault, .create = [] { return std::unique_ptr<Gizmo>(); }}));
        CHECK(*gizmos::registry().ownerOf(gizmos::kDefault) == "");

        CHECK_FALSE(gizmos::setActive("nope"));
        CHECK(gizmos::setActive("spinner"));
        CHECK(gizmos::activeId() == "spinner");
        CHECK(gizmos::activeOverride() == "spinner");
        std::unique_ptr<Gizmo> g = gizmos::create("spinner");
        CHECK(alive == 1);
        CHECK(g->supports("testTool"));
        CHECK_FALSE(g->supports("move"));
        g.reset();
        CHECK(alive == 0);

        int before = notified;
        ctx.unloadAll();
        CHECK(notified > before);
        CHECK_FALSE(gizmos::find("spinner"));
        // The override names a provider that is gone, so the pref (default) decides again.
        CHECK(gizmos::activeId() == gizmos::kDefault);
    }
    CHECK(gizmos::setActive(""));
    gizmos::removeListener(listener);
}

TEST_CASE("selection handlers redirect hits and drop the active id on removal")
{
    using namespace selectionHandlers;
    CHECK(activeId().empty());
    CHECK(redirect({Hit{.name = "a"}, Hit{.name = "b"}}) == std::vector<std::string>{"a", "b"});
    CHECK_FALSE(setActive("missing"));

    ext::Context ctx("puppet");
    const Handler* h = add(ctx, {.id = "puppet.dominant",
                                 .label = "Dominant joint",
                                 .redirect = [](const Hit& hit) -> std::optional<std::vector<std::string>> {
                                     if (hit.name == "skin")
                                         return std::vector<std::string>{hit.face == 3 ? "elbow" : "shoulder"};
                                     if (hit.name == "nothing")
                                         return std::vector<std::string>{};
                                     return std::nullopt;
                                 },
                                 .hover = [](const Hit& hit) -> std::optional<Highlight> {
                                     Highlight hl;
                                     hl.mesh = hit.node;
                                     hl.weights = {0.0f, 1.0f};
                                     return hl;
                                 }});
    REQUIRE(h);
    CHECK(setActive("puppet.dominant"));
    CHECK(active() == h);
    // Redirected, kept, emptied and de-duplicated in first-seen order.
    CHECK(redirect({Hit{.name = "skin", .face = 3}, Hit{.name = "loc"}, Hit{.name = "nothing"}, Hit{.name = "skin", .face = 3},
                    Hit{.name = "skin", .click = false}}) == std::vector<std::string>{"elbow", "loc", "shoulder"});
    CHECK(h->hover(Hit{.node = 7})->mesh == 7);

    int notified = 0;
    int listener = addListener([&] { ++notified; });
    ctx.unloadAll();
    CHECK(notified == 1);
    CHECK(activeId().empty());
    CHECK_FALSE(find("puppet.dominant"));
    removeListener(listener);
}

TEST_CASE("candidate marking menu: compass order, overflow, the object menu as a cascade, selectAs on redirects")
{
    using namespace selectionHandlers;
    CHECK(candidateMenu({}).is_null());
    std::vector<Candidate> cands;
    for (const char* n : {"upperarm_l", "upperarm_twist_01_l", "clavicle_l", "lowerarm_l", "spine_05", "a", "b", "c", "d", "e"})
        cands.push_back({.name = n, .label = QString(n).toUpper()});
    json fallback = {{"N", {{"label", "Complete Tool"}, {"action", "x"}}}, {"overflow", json::array({{{"label", "Delete"}, {"action", "y"}}})}};
    json m = candidateMenu(cands, fallback);
    CHECK(m["N"]["label"] == "UPPERARM_L");
    CHECK(m["N"]["action"] == json({{"command", "select"}, {"args", {{"args", {"upperarm_l"}}}}}));
    CHECK(m["S"]["label"] == "UPPERARM_TWIST_01_L");
    CHECK(m["W"]["label"] == "CLAVICLE_L");
    CHECK(m["E"]["label"] == "LOWERARM_L");
    CHECK(m["NE"]["label"] == "SPINE_05");
    CHECK(m["SW"]["label"] == "C");
    REQUIRE(m["overflow"].size() == 4);
    CHECK(m["overflow"][0]["label"] == "D");
    CHECK(m["overflow"][1]["label"] == "E");
    CHECK(m["overflow"][2].value("separator", false));
    CHECK(m["overflow"][3]["label"] == "Object Menu");
    CHECK(m["overflow"][3]["children"]["overflow"].size() == 2);
    CHECK(m["overflow"][3]["children"]["overflow"][0]["label"] == "Complete Tool");
    CHECK_FALSE(candidateMenu({cands[0]}).contains("overflow"));

    // Redirected names go through the joints' selectAs, unless bypassed.
    run("newScene");
    run("joint", {{"name", "upperarm_l"}});
    run("joint", {{"name", "upperarm_twist_01_l"}});
    run("selectAs", {{"args", {"upperarm_twist_01_l"}}, {"target", "upperarm_l"}});
    ext::Context ctx("twisty");
    REQUIRE(add(ctx, {.id = "twisty.pick",
                      .label = "Twist pick",
                      .redirect = [](const Hit&) -> std::optional<std::vector<std::string>> { return std::vector<std::string>{"upperarm_twist_01_l"}; },
                      .candidates = [](const Hit& h) { return std::vector<Candidate>{{.name = h.name}}; }}));
    REQUIRE(setActive("twisty.pick"));
    CHECK(redirect({Hit{.name = "Skin"}}) == std::vector<std::string>{"upperarm_l"});
    CHECK(candidates(Hit{.name = "x"}).size() == 1);
    setSelectAsHeld(true);
    CHECK(selectAsBypassed());
    CHECK(redirect({Hit{.name = "Skin"}}) == std::vector<std::string>{"upperarm_twist_01_l"});
    setSelectAsHeld(false);
    CHECK(applySelectAs("upperarm_twist_01_l") == "upperarm_l");
    CHECK(applySelectAs("nobody") == "nobody");
    ctx.unloadAll();
    CHECK(candidates(Hit{.name = "x"}).empty());
}

TEST_CASE("draw overrides: locators by default, the most derived type wins, picking through the override")
{
    const scene::NodeType* locator = scene::types::find("locator");
    const scene::NodeType* camera = scene::types::find("camera");
    const scene::NodeType* transform = scene::types::find("transform");
    REQUIRE(locator);
    REQUIRE(camera);
    REQUIRE(transform);
    REQUIRE(drawOverrides::forType(*locator));
    CHECK(drawOverrides::forType(*locator)->id == "lila.locator");
    CHECK(drawOverrides::forType(*camera)->id == "lila.locator");
    CHECK_FALSE(drawOverrides::forType(*transform));

    {
        ext::Context ctx("cams");
        int drawn = 0;
        REQUIRE(drawOverrides::add(ctx, {.id = "cams.camera", .nodeType = "camera", .draw = [&](drawOverrides::DrawContext&) { ++drawn; }}));
        CHECK(drawOverrides::forType(*camera)->id == "cams.camera");
        CHECK(drawOverrides::forType(*locator)->id == "lila.locator");
        ctx.unloadAll();
    }
    CHECK(drawOverrides::forType(*camera)->id == "lila.locator");

    // A locator at the origin, 20 units across, seen from the front: its X line runs through the view's centre.
    run("newScene");
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*locator, "loc");
    g.setValue({n->id(), scene::Locator::kLocalScale, -1}, glm::dvec3(10.0));
    g.evaluate();
    const ViewInfo view = frontView();
    auto centre = view.project({0, 0, 0});
    REQUIRE(centre);
    const drawOverrides::DrawOverride* o = drawOverrides::forType(n->type());
    glm::dmat4 world = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    std::optional<double> onLine = o->pick({.view = view, .node = *n, .world = world, .a = *centre + QPointF(5, 1), .b = *centre + QPointF(5, 1)});
    REQUIRE(onLine);
    CHECK(*onLine == doctest::Approx(1.0).epsilon(0.05));
    CHECK_FALSE(o->pick({.view = view, .node = *n, .world = world, .a = QPointF(-50, -50), .b = QPointF(-50, -50), .click = false}));
    CHECK(o->pick({.view = view, .node = *n, .world = world, .a = *centre - QPointF(30, 30), .b = *centre + QPointF(30, 30), .click = false}));

    render::FrameState s;
    OverlayPainter painter(s.overlayTris, 1.0);
    drawOverrides::DrawContext dc{.frame = s, .painter = painter, .view = view, .node = *n};
    dc.world = world;
    o->draw(dc);
    CHECK(s.lines.size() == 6);
}

TEST_CASE("viewport hooks keep their order, replace in place and unload with their owner")
{
    const size_t drawersBefore = viewportHooks::drawers().size();
    std::vector<std::string> order;
    auto drawer = [&](std::string tag) { return [&order, tag](render::FrameState&, OverlayPainter&, const ViewInfo&) { order.push_back(tag); }; };
    ext::Context ctx("hooks");
    CHECK(viewportHooks::addDrawer(ctx, "hooks.b", drawer("b")) != 0);
    CHECK(viewportHooks::addDrawer(ctx, "hooks.a", drawer("a")) != 0);
    CHECK(viewportHooks::addDrawer(ctx, "hooks.a", drawer("dup")) == 0);
    CHECK(viewportHooks::addPicker(ctx, "hooks.p", [](QPointF, QPointF, bool, const ViewInfo&) {
              return std::vector<viewportHooks::PickHit>{{"thing", 1.0}};
          }) != 0);
    CHECK(viewportHooks::drawers().size() == drawersBefore + 2);
    std::vector<std::string> ids = viewportHooks::drawerIds();
    auto b = std::find(ids.begin(), ids.end(), "hooks.b"), a = std::find(ids.begin(), ids.end(), "hooks.a");
    REQUIRE(b != ids.end());
    REQUIRE(a != ids.end());
    CHECK(b < a);

    // Lila's own named hook replaces in place.
    viewportHooks::addDrawer("lila.test", drawer("first"));
    viewportHooks::addDrawer("hooks.c", drawer("c"));
    viewportHooks::addDrawer("lila.test", drawer("second"));
    render::FrameState s;
    OverlayPainter painter(s.overlayTris, 1.0);
    for (const auto& d : viewportHooks::drawers())
        d(s, painter, frontView());
    auto pos = [&](const char* tag) { return std::find(order.begin(), order.end(), tag) - order.begin(); };
    CHECK(pos("b") < pos("a"));
    CHECK(pos("a") < pos("second"));
    CHECK(pos("second") < pos("c"));
    CHECK(std::find(order.begin(), order.end(), "first") == order.end());
    CHECK(viewportHooks::removeDrawer("lila.test"));
    CHECK(viewportHooks::removeDrawer("hooks.c"));

    ctx.unloadAll();
    CHECK(viewportHooks::drawers().size() == drawersBefore);
    const std::vector<std::string> pickers = viewportHooks::pickerIds();
    CHECK(std::find(pickers.begin(), pickers.end(), "hooks.p") == pickers.end());
}

TEST_CASE("render passes run per stage by order, then id")
{
    std::vector<std::string> ran;
    auto pass = [&](std::string id, renderPasses::Stage stage, int order) {
        return renderPasses::Pass{.id = id, .stage = stage, .order = order, .run = [&ran, id](renderPasses::Context&) { ran.push_back(id); }};
    };
    ext::Context ctx("passes");
    REQUIRE(renderPasses::add(ctx, pass("p.late", renderPasses::Stage::Overlay, 5)));
    REQUIRE(renderPasses::add(ctx, pass("p.b", renderPasses::Stage::Overlay, 0)));
    REQUIRE(renderPasses::add(ctx, pass("p.a", renderPasses::Stage::Overlay, 0)));
    REQUIRE(renderPasses::add(ctx, pass("p.scene", renderPasses::Stage::Scene, 0)));
    REQUIRE(renderPasses::add(ctx, {.id = "p.throws", .stage = renderPasses::Stage::Overlay, .order = 1, .run = [](renderPasses::Context&) {
                                        throw std::runtime_error("boom");
                                    }}));
    render::FrameState s;
    OverlayPainter painter(s.overlayTris, 1.0);
    ViewInfo view = frontView();
    renderPasses::Context c{.frame = s, .painter = painter, .view = view};
    renderPasses::run(renderPasses::Stage::Overlay, c);
    CHECK(ran == std::vector<std::string>{"p.a", "p.b", "p.late"});
    ctx.unloadAll();
    CHECK(renderPasses::registry().empty());
}

TEST_CASE("cursor badges: owner, removal, order and the icons they need")
{
    const std::vector<std::string> before = cursorBadges::ids();
    ext::Context ctx("badges");
    CHECK(cursorBadges::add(ctx, {.id = "badges.drag", .icon = "hand-move", .active = [](const ViewportState& v) { return v.gizmo != nullptr; }, .message = "Dragging"}) != 0);
    CHECK(overlayIcons::contains("hand-move"));
    CHECK(cursorBadges::add(ctx, {.id = "badges.drag", .icon = "x"}) == 0);
    CHECK(cursorBadges::ids().back() == "badges.drag");

    ViewportState st;
    std::vector<const cursorBadges::Badge*> on = cursorBadges::active(st);
    CHECK(std::none_of(on.begin(), on.end(), [](const cursorBadges::Badge* b) { return b->id == "badges.drag"; }));
    Manipulator m;
    st.gizmo = &m;
    on = cursorBadges::active(st);
    CHECK(std::any_of(on.begin(), on.end(), [](const cursorBadges::Badge* b) { return b->id == "badges.drag"; }));

    ctx.unloadAll();
    CHECK(cursorBadges::ids() == before);
}

TEST_CASE("overlay icons come and go by id")
{
    const uint64_t gen = overlayIcons::generation();
    CHECK(overlayIcons::contains("grid-pattern"));
    ext::Context ctx("icons");
    QImage img(8, 8, QImage::Format_ARGB32);
    img.fill(Qt::white);
    CHECK(overlayIcons::add(ctx, "icons.dot", {.image = img}) != 0);
    CHECK(overlayIcons::contains("icons.dot"));
    CHECK(overlayIcons::generation() > gen);
    CHECK(overlayIcons::add(ctx, "grid-pattern", {.tabler = "x"}) == 0);
    ctx.unloadAll();
    CHECK_FALSE(overlayIcons::contains("icons.dot"));
    CHECK(overlayIcons::contains("grid-pattern"));
}

TEST_CASE("viewport tools: activate by id, removal ends the active tool")
{
    int alive = 0;
    ext::Context ctx("tools");
    REQUIRE(viewportTools::add(ctx, {.id = "tools.test", .label = "Test", .create = [&] { return std::make_unique<TestTool>(&alive); }}));
    CHECK_FALSE(viewportTools::activate("tools.missing"));
    CHECK(viewportTools::activate("tools.test"));
    CHECK(alive == 1);
    CHECK(viewportTools::activeId() == "tools.test");
    REQUIRE(viewportTools::active());
    CHECK(std::string(viewportTools::active()->name()) == "testTool");

    ctx.unloadAll();
    CHECK(alive == 0);
    CHECK_FALSE(viewportTools::active());
    CHECK(viewportTools::activeId().empty());

    // A tool set directly carries no id, and removing an unrelated definition leaves it alone.
    viewportTools::add({.id = "tools.other", .create = [&] { return std::make_unique<TestTool>(&alive); }});
    viewportTools::set(std::make_unique<TestTool>(&alive));
    CHECK(viewportTools::activeId().empty());
    CHECK(viewportTools::remove("tools.other"));
    CHECK(alive == 1);
    viewportTools::set(nullptr);
    CHECK(alive == 0);
}

namespace
{
struct RendererProbe
{
    int opened = 0, closed = 0, renders = 0, changes = 0;
    viewportRenderers::Request last;
};

class ProbeSession final : public viewportRenderers::Session
{
public:
    explicit ProbeSession(RendererProbe& p) : mProbe(p) { ++mProbe.opened; }
    ~ProbeSession() override { ++mProbe.closed; }
    void render(const viewportRenderers::Request& r) override
    {
        ++mProbe.renders;
        mProbe.last = r;
    }
    void sceneChanged(scene::Change, scene::NodeId) override { ++mProbe.changes; }

private:
    RendererProbe& mProbe;
};

viewportRenderers::Request requestOf(const ViewInfo& v)
{
    viewportRenderers::Request r;
    r.view = v;
    r.width = int(v.width);
    r.height = int(v.height);
    return r;
}
} // namespace

TEST_CASE("viewport renderers: registry, per-view selection, commands and unloading")
{
    viewportRenderers::registerCommands();
    RendererProbe probe;
    ext::Context ctx("vrExt");
    auto open = [&probe](int) { return std::make_unique<ProbeSession>(probe); };
    REQUIRE(viewportRenderers::add(ctx, {.id = "vr.probe", .name = "Probe", .doc = "A test renderer.", .open = open}));
    CHECK_FALSE(viewportRenderers::add(ctx, {.id = "vr.probe", .open = open}));
    CHECK_FALSE(viewportRenderers::add(ctx, {.id = viewportRenderers::kLila, .open = open}));
    REQUIRE(viewportRenderers::add(ctx, {.id = "vr.refuses", .name = "Refuses", .open = [](int) { return std::unique_ptr<viewportRenderers::Session>(); }}));

    const auto choices = viewportRenderers::choices();
    REQUIRE(choices.size() >= 3);
    CHECK(choices[0].first == viewportRenderers::kLila);
    CHECK(std::find(choices.begin(), choices.end(), std::pair<std::string, std::string>{"vr.probe", "Probe"}) != choices.end());

    constexpr int kPanelA = 40, kPanelB = 41;
    CHECK(viewportRenderers::selected(kPanelA) == viewportRenderers::kLila);
    CHECK_FALSE(viewportRenderers::select(kPanelA, "vr.missing"));
    CHECK_FALSE(viewportRenderers::select(kPanelA, "vr.refuses"));
    CHECK(viewportRenderers::selected(kPanelA) == viewportRenderers::kLila);

    // The command picks per viewport; the other one keeps Lila's.
    CHECK(run("viewportRenderer", {{"args", {"vr.probe"}}, {"panel", kPanelA}}) == "vr.probe");
    CHECK(probe.opened == 1);
    CHECK(run("viewportRenderer", {{"panel", kPanelA}}) == "vr.probe");
    CHECK(run("viewportRenderer", {{"panel", kPanelB}}) == viewportRenderers::kLila);
    CHECK_THROWS_AS(run("viewportRenderer", {{"args", {"vr.missing"}}, {"panel", kPanelA}}), CommandError);
    const json listed = run("viewportRenderers");
    REQUIRE(listed.is_array());
    CHECK(listed[0]["id"] == viewportRenderers::kLila);
    CHECK(std::any_of(listed.begin(), listed.end(), [](const json& r) { return r["id"] == "vr.probe" && r["doc"] == "A test renderer."; }));

    // Only a view drawing with a renderer is handed over.
    render::FrameState s;
    CHECK(viewportRenderers::apply(kPanelA, requestOf(frontView()), s));
    CHECK(s.externalScene);
    render::FrameState own;
    CHECK_FALSE(viewportRenderers::apply(kPanelB, requestOf(frontView()), own));
    CHECK_FALSE(own.externalScene);

    // Back to Lila closes the session; unloading the extension sends its views back too.
    CHECK(run("viewportRenderer", {{"args", {"lila"}}, {"panel", kPanelA}}) == viewportRenderers::kLila);
    CHECK(probe.closed == 1);
    REQUIRE(viewportRenderers::select(kPanelB, "vr.probe"));
    ctx.unloadAll();
    CHECK(probe.closed == 2);
    CHECK(viewportRenderers::selected(kPanelB) == viewportRenderers::kLila);
    CHECK_FALSE(viewportRenderers::registry().contains("vr.probe"));
    viewportRenderers::release(kPanelA);
    viewportRenderers::release(kPanelB);
}

TEST_CASE("viewport renderers: asked again when the view, time or scene changes, with the scene's changes fed in")
{
    run("newScene");
    RendererProbe probe;
    viewportRenderers::add({.id = "vr.feed", .open = [&probe](int) { return std::make_unique<ProbeSession>(probe); }});
    constexpr int kPanel = 42;
    REQUIRE(viewportRenderers::select(kPanel, "vr.feed"));
    const ViewInfo view = frontView();

    render::FrameState s;
    viewportRenderers::apply(kPanel, requestOf(view), s);
    CHECK(probe.renders == 1);
    CHECK(probe.last.serial == 1);
    CHECK(probe.last.graph == &scene::Graph::current());
    CHECK(probe.last.width == 400);
    viewportRenderers::apply(kPanel, requestOf(view), s);
    CHECK(probe.renders == 1);

    ViewInfo moved = view;
    moved.view = glm::translate(moved.view, glm::dvec3(1.0, 0.0, 0.0));
    viewportRenderers::apply(kPanel, requestOf(moved), s);
    CHECK(probe.renders == 2);
    viewportRenderers::Request later = requestOf(moved);
    later.time = 1.0;
    viewportRenderers::apply(kPanel, later, s);
    CHECK(probe.renders == 3);
    CHECK(probe.last.time == 1.0);

    // A scene edit reaches the session and asks for a frame; selecting does only the first.
    const uint64_t revision = probe.last.sceneRevision;
    json made = run("createNode", {{"args", {"transform"}}});
    CHECK(probe.changes > 0);
    viewportRenderers::apply(kPanel, later, s);
    CHECK(probe.renders == 4);
    CHECK(probe.last.sceneRevision > revision);
    run("select", {{"args", {made}}});
    viewportRenderers::apply(kPanel, later, s);
    CHECK(probe.renders == 4);

    // A ray through the centre pixel runs down the view axis, and depthOf measures along it.
    glm::dvec3 o, d;
    probe.last.ray(200.0, 200.0, o, d);
    const glm::dmat4& v = probe.last.view.view;
    CHECK(glm::dot(d, -glm::dvec3(v[0][2], v[1][2], v[2][2])) == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(probe.last.depthOf(o + d * 100.0) == doctest::Approx(probe.last.depthOf(o) + 100.0).epsilon(1e-6));

    viewportRenderers::remove("vr.feed");
    CHECK(probe.closed == 1);
    viewportRenderers::release(kPanel);
}
