#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "ui/ReferenceOverlay.h"
#include "ui/ReferenceSet.h"
#include "ui/ReferenceWindow.h"

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QTabBar>

#include <filesystem>

using namespace lila;
using namespace lila::ui;
namespace refs = lila::ui::references;

namespace
{
std::filesystem::path testDir()
{
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "lila_reference_tests";
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

/** A w x h PNG in the test folder. */
std::string png(const char* name, int w, int h, QColor color = Qt::red)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(color);
    const auto file = testDir() / name;
    REQUIRE(img.save(QString::fromStdWString(file.wstring()), "PNG"));
    return file.string();
}

void freshSet()
{
    refs::install();
    referenceOverlays::install({});
    refs::apply(refs::Set{});
    UndoStack::get().clear();
}

bool near2d(glm::dvec2 a, glm::dvec2 b)
{
    return glm::length(a - b) < 1e-6;
}
} // namespace

TEST_CASE("reference set: json round trip keeps images, overlays, windows and bound views")
{
    refs::Set s;
    s.images.push_back({.id = "ref1", .name = "front", .path = "refs/front.png", .flipH = true, .quarterTurns = 3, .contrast = 1.5, .window = "main"});
    s.images.push_back({.id = "ref2", .name = "side", .data = "aGVsbG8=", .ext = "jpg", .grayscale = true, .window = "window1"});
    refs::Overlay o;
    o.id = "overlay1";
    o.image = "ref2";
    o.view = "front";
    o.anchor = refs::Anchor::TopRight;
    o.offset = {-0.2, 0.3};
    o.mode = refs::Mode::Outline;
    o.depth = refs::Depth::Behind;
    o.bound.set = true;
    o.bound.yaw = 30.0;
    o.bound.forward = {1.0, 0.0, 0.0};
    s.overlays.push_back(o);
    s.windows.push_back({.id = "window1", .current = "ref2", .opacity = 0.5, .onTop = true});

    const refs::Set back = refs::Set::fromJson(s.toJson());
    REQUIRE(back.images.size() == 2);
    CHECK(back.images[0].flipH);
    CHECK(back.images[0].quarterTurns == 3);
    CHECK(back.images[0].contrast == doctest::Approx(1.5));
    CHECK(back.images[1].data == "aGVsbG8=");
    CHECK(back.images[1].ext == "jpg");
    REQUIRE(back.overlays.size() == 1);
    CHECK(back.overlays[0].anchor == refs::Anchor::TopRight);
    CHECK(back.overlays[0].mode == refs::Mode::Outline);
    CHECK(back.overlays[0].depth == refs::Depth::Behind);
    CHECK(back.overlays[0].bound.set);
    CHECK(back.overlays[0].bound.yaw == doctest::Approx(30.0));
    CHECK(near2d(back.overlays[0].offset, {-0.2, 0.3}));
    REQUIRE(back.window("window1"));
    CHECK(back.window("window1")->onTop);
    CHECK(back.window("window1")->opacity == doctest::Approx(0.5));
    // The first window is always there, showing its tab.
    REQUIRE(back.window(refs::kMainWindow));
    CHECK(back.window(refs::kMainWindow)->current == "ref1");
}

TEST_CASE("reference set: loading repairs missing windows, stray currents and overlays of missing images")
{
    nlohmann::json j = {{"images", {{{"id", "a"}, {"path", "x.png"}, {"window", "w9"}}, {{"id", "b"}}}},
                        {"overlays", {{{"id", "o1"}, {"image", "gone"}}}},
                        {"windows", {{{"id", "main"}, {"current", "a"}}}}};
    const refs::Set s = refs::Set::fromJson(j);
    CHECK(s.images.size() == 1); // b has no file
    CHECK(s.overlays.empty());
    REQUIRE(s.window("w9"));
    CHECK(s.window("w9")->current == "a");
    CHECK(s.window("main")->current.empty());
}

TEST_CASE("reference window drops: Shift replaces the shown tab, otherwise new tabs")
{
    CHECK(refs::dropAction(false, true) == refs::DropAction::AddTabs);
    CHECK(refs::dropAction(true, true) == refs::DropAction::Replace);
    CHECK(refs::dropAction(true, false) == refs::DropAction::AddTabs);
    CHECK(refs::ReferenceWindow::dropActionFor(Qt::ShiftModifier, true) == refs::DropAction::Replace);
}

TEST_CASE("reference overlay crop keeps the image where it is on screen")
{
    const glm::dvec2 view(800, 600);
    refs::Overlay o;
    o.scale = 0.5; // a 2:1 image, 600 x 300 px centred
    const glm::dvec2 before = refs::overlayCorners(o, nullptr, 2.0, view)[1];
    // Keep the right third, as for the back view of a model sheet.
    refs::setCrop(o, nullptr, 2.0, view, {2.0 / 3.0, 0.0, 1.0, 1.0});
    auto c = refs::overlayCorners(o, nullptr, 2.0, view);
    CHECK(near2d(c[1], before));
    CHECK(near2d(c[0], {500, 150}));
    CHECK(o.scale == doctest::Approx(0.5));
    CHECK(refs::overlayImageUv(o, nullptr, 2.0, view, {600, 300}).x == doctest::Approx(5.0 / 6.0));

    // A flipped image crops the same part of the picture, now on the left side of the quad.
    refs::Image flipped{.id = "f", .flipH = true};
    CHECK(refs::overlayUv(o, &flipped) == glm::dvec4(1, 0, 2.0 / 3.0, 1));

    // Cropping the height shrinks the shown height with it.
    refs::setCrop(o, nullptr, 2.0, view, {2.0 / 3.0, 0.0, 1.0, 0.5});
    CHECK(o.scale == doctest::Approx(0.25));
    c = refs::overlayCorners(o, nullptr, 2.0, view);
    CHECK(near2d(c[1], before));

    refs::Set s;
    s.images.push_back({.id = "ref1", .path = "sheet.png"});
    o.id = "overlay1";
    o.image = "ref1";
    s.overlays.push_back(o);
    const refs::Set back = refs::Set::fromJson(s.toJson());
    REQUIRE(back.overlays.size() == 1);
    CHECK(back.overlays[0].crop == o.crop);
}

TEST_CASE("reference overlay layout: anchors, aspect, quarter turns, flips and hit tests")
{
    const glm::dvec2 view(800, 600);
    refs::Overlay o;
    o.scale = 0.5; // 300 px tall, 600 wide
    auto c = refs::overlayCorners(o, nullptr, 2.0, view);
    CHECK(near2d(c[0], {100, 150}));
    CHECK(near2d(c[2], {700, 450}));
    CHECK(refs::overlayContains(o, nullptr, 2.0, view, {400, 300}));
    CHECK_FALSE(refs::overlayContains(o, nullptr, 2.0, view, {50, 300}));
    CHECK_FALSE(refs::overlayContains(o, nullptr, 2.0, view, {400, 470}));

    // A quarter turn: shown height is the image's width.
    refs::Image turned{.id = "t", .quarterTurns = 1};
    c = refs::overlayCorners(o, &turned, 2.0, view);
    const glm::dvec2 lo = glm::min(glm::min(c[0], c[1]), glm::min(c[2], c[3])), hi = glm::max(glm::max(c[0], c[1]), glm::max(c[2], c[3]));
    CHECK(hi.y - lo.y == doctest::Approx(300.0));
    CHECK(hi.x - lo.x == doctest::Approx(150.0));

    // Top right anchor with a negative x offset stays inside the right edge.
    o.anchor = refs::Anchor::TopRight;
    o.offset = {-0.5, 0.25};
    c = refs::overlayCorners(o, nullptr, 1.0, view);
    CHECK(near2d((c[0] + c[2]) * 0.5, {500, 150}));

    refs::Image flipped{.id = "f", .flipH = true};
    CHECK(refs::overlayUv(refs::Overlay{}, &flipped) == glm::dvec4(1, 0, 0, 1));

    // Re-anchoring keeps the overlay where it is on screen.
    refs::Overlay r = o;
    refs::reanchor(r, refs::Anchor::BottomLeft, view);
    auto rc = refs::overlayCorners(r, nullptr, 1.0, view);
    CHECK(near2d((rc[0] + rc[2]) * 0.5, {500, 150}));
}

TEST_CASE("reference overlay bound view: full at its own view, gone past the fade angle")
{
    refs::Overlay o;
    CHECK(refs::boundFade(o, {0, 0, -1}) == 1.0);
    o.bound.set = true;
    o.bound.forward = {0, 0, -1};
    o.fadeAngle = 15.0;
    CHECK(refs::boundFade(o, {0, 0, -1}) == doctest::Approx(1.0));
    const double a4 = glm::radians(4.0), a10 = glm::radians(10.0), a20 = glm::radians(20.0);
    CHECK(refs::boundFade(o, {std::sin(a4), 0, -std::cos(a4)}) == doctest::Approx(1.0));
    const double mid = refs::boundFade(o, {std::sin(a10), 0, -std::cos(a10)});
    CHECK(mid > 0.0);
    CHECK(mid < 1.0);
    CHECK(refs::boundFade(o, {std::sin(a20), 0, -std::cos(a20)}) == doctest::Approx(0.0));
    CHECK(refs::boundFade(o, {1, 0, 0}) == doctest::Approx(0.0));
}

TEST_CASE("reference overlay park: opaque, down the right edge, inside the view")
{
    refs::Set s;
    for (int i = 1; i <= 3; ++i)
    {
        s.images.push_back({.id = "ref" + std::to_string(i), .path = "x.png"});
        refs::Overlay o;
        o.id = "overlay" + std::to_string(i);
        o.image = "ref" + std::to_string(i);
        o.opacity = 0.2;
        s.overlays.push_back(o);
    }
    const glm::dvec2 view(1600, 900);
    refs::park(s, "persp", [](const refs::Image&) { return 1.5; }, view);
    double lastBottom = 0.0;
    for (const refs::Overlay& o : s.overlays)
    {
        CHECK(o.opacity == 1.0);
        CHECK(o.anchor == refs::Anchor::TopRight);
        auto c = refs::overlayCorners(o, nullptr, 1.5, view);
        CHECK(c[1].x <= view.x + 1e-6);
        CHECK(c[1].x > view.x * 0.9);
        CHECK(c[0].y >= lastBottom - 1e-6);
        CHECK(c[2].y <= view.y + 1e-6);
        lastBottom = c[2].y;
    }
}

TEST_CASE("referenceImage: add, edit, replace keeps settings, remove takes overlays, all undoable")
{
    freshSet();
    const std::string front = png("front.png", 64, 32), side = png("side.png", 32, 64, Qt::blue), front2 = png("front2.png", 64, 32, Qt::green);
    nlohmann::json ids = ops::execute("referenceImage", {{"args", {front, side}}});
    REQUIRE(ids.is_array());
    REQUIRE(refs::current().images.size() == 2);
    CHECK(refs::current().images[0].name == "front");
    CHECK(refs::current().tabs(refs::kMainWindow).size() == 2);
    const std::string id = ids[0].get<std::string>();

    ops::execute("referenceImage", {{"edit", id}, {"flipH", true}, {"contrast", 2.0}});
    CHECK(refs::current().image(id)->flipH);
    ops::execute("referenceImage", {{"args", {front2}}, {"replace", id}});
    CHECK(refs::current().image(id)->name == "front2");
    CHECK(refs::current().image(id)->flipH);
    CHECK(refs::current().image(id)->contrast == doctest::Approx(2.0));
    UndoStack::get().undo();
    CHECK(refs::current().image(id)->name == "front");

    ops::execute("referenceOverlay", {{"args", {id}}, {"view", "persp"}});
    CHECK(refs::current().overlays.size() == 1);
    ops::execute("referenceImage", {{"remove", {id}}});
    CHECK(refs::current().images.size() == 1);
    CHECK(refs::current().overlays.empty());
    UndoStack::get().undo();
    CHECK(refs::current().images.size() == 2);
    CHECK(refs::current().overlays.size() == 1);

    CHECK_THROWS_AS(ops::execute("referenceImage", {{"args", {(testDir() / "notes.txt").string()}}}), CommandError);
}

TEST_CASE("referenceImage embed: the bytes travel in the set and come back as a file")
{
    freshSet();
    const std::string file = png("embed.png", 8, 8, Qt::yellow);
    const std::string id = ops::execute("referenceImage", {{"args", {file}}, {"embed", true}}).get<std::string>();
    const refs::Image* img = refs::current().image(id);
    REQUIRE(img);
    CHECK_FALSE(img->data.empty());
    CHECK(img->ext == "png");
    std::filesystem::remove(file);
    const std::string cached = refs::fileOf(*img);
    CHECK(std::filesystem::exists(cached));
    CHECK(QImage(QString::fromStdString(cached)).pixelColor(0, 0) == QColor(Qt::yellow));
}

TEST_CASE("referenceSet: save and load a .lilaref board")
{
    freshSet();
    ops::execute("referenceImage", {{"args", {png("board.png", 16, 16)}}, {"grayscale", true}});
    const auto file = testDir() / "boards" / "hero.lilaref";
    ops::execute("referenceSet", {{"save", file.string()}});
    CHECK(std::filesystem::exists(file));
    ops::execute("referenceSet", {{"clear", true}});
    CHECK(refs::current().images.empty());
    ops::execute("referenceSet", {{"load", file.string()}});
    REQUIRE(refs::current().images.size() == 1);
    CHECK(refs::current().images[0].grayscale);
    CHECK(std::filesystem::exists(refs::fileOf(refs::current().images[0])));
    CHECK(ops::execute("referenceSet", {{"query", true}})["storage"] == "scene");
}

TEST_CASE("referenceOverlay: a file becomes an overlay-only image that leaves with its overlay")
{
    freshSet();
    const std::string file = png("overlay.png", 40, 20);
    const std::string id = ops::execute("referenceOverlay", {{"args", {file}}, {"view", "front"}, {"opacity", 0.4}, {"mode", "outline"}}).get<std::string>();
    const refs::Overlay* o = refs::current().overlay(id);
    REQUIRE(o);
    CHECK(o->view == "front");
    CHECK(o->opacity == doctest::Approx(0.4));
    CHECK(o->mode == refs::Mode::Outline);
    CHECK(refs::current().image(o->image)->window.empty());
    CHECK(referenceOverlays::activeOverlay("front") == id);
    ops::execute("referenceOverlay", {{"edit", id}, {"anchor", "topLeft"}, {"depth", "behind"}});
    CHECK(refs::current().overlay(id)->depth == refs::Depth::Behind);
    CHECK(refs::current().overlay(id)->anchor == refs::Anchor::TopLeft);
    CHECK(ops::execute("referenceOverlay", {{"query", true}, {"view", "front"}}).size() == 1);
    ops::execute("referenceOverlay", {{"remove", {id}}});
    CHECK(refs::current().overlays.empty());
    CHECK(refs::current().images.empty());
    UndoStack::get().undo();
    CHECK(refs::current().overlays.size() == 1);
}

TEST_CASE("referenceOverlay: clicking a hidden overlay's outliner row keeps it hidden")
{
    freshSet();
    const std::string id = ops::execute("referenceOverlay", {{"args", {png("hidden.png", 20, 20)}}, {"view", "front"}}).get<std::string>();
    ops::execute("referenceOverlay", {{"edit", id}, {"visible", false}});
    referenceOverlays::activateRow(id);
    CHECK_FALSE(refs::current().overlay(id)->visible);
    CHECK(referenceOverlays::activeOverlay("front") == id);
    ops::execute("referenceOverlay", {{"edit", id}, {"visible", true}});
    referenceOverlays::activateRow(id);
    CHECK(refs::current().overlay(id)->visible);
    CHECK(referenceOverlays::adjusting());
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(qApp, &esc);
    CHECK_FALSE(referenceOverlays::adjusting());
}

TEST_CASE("reference window: the tab bar shows from the second image")
{
    freshSet();
    refs::ReferenceWindow w(refs::kMainWindow);
    w.resize(400, 300);
    w.show();
    CHECK_FALSE(w.tabBar()->isVisible());
    ops::execute("referenceImage", {{"args", {png("one.png", 10, 10)}}});
    CHECK_FALSE(w.tabBar()->isVisible());
    CHECK_FALSE(w.currentImage().empty());
    ops::execute("referenceImage", {{"args", {png("two.png", 10, 10)}}});
    CHECK(w.tabBar()->isVisible());
    CHECK(w.tabBar()->count() == 2);
    CHECK(w.currentImage() == refs::current().images[1].id);
    ops::execute("referenceImage", {{"remove", {refs::current().images[1].id}}});
    CHECK_FALSE(w.tabBar()->isVisible());
}
