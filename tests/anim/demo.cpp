// Throwaway harness: builds the time slider, graph editor and channel box over a keyed scene and grabs PNGs.
// Usage: lila_anim_demo <outdir>

#include "TestUtil.h"
#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/ChannelBox.h"
#include "ui/GraphEditor.h"
#include "ui/Theme.h"
#include "ui/TimeSlider.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QMouseEvent>

#include <cstdlib>
#include <filesystem>
#include <cstdio>

using namespace lila;
using animtest::run;

namespace
{
QString gOut;

void pump()
{
    for (int i = 0; i < 5; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
    }
}

void save(QWidget* w, const char* name)
{
    pump();
    w->grab().save(gOut + "/" + name);
    std::printf("saved %s\n", name);
}

void mouse(QWidget* w, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {})
{
    QMouseEvent e(type, at, w->mapToGlobal(at), button, buttons, mods);
    QApplication::sendEvent(w, &e);
}
} // namespace

int main(int argc, char** argv)
{
    gOut = argc > 1 ? argv[1] : ".";
    // Keep the demo out of the real user prefs.
    auto cfg = std::filesystem::temp_directory_path() / "lila_anim_demo";
    std::filesystem::create_directories(cfg);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", cfg.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", cfg.string().c_str(), 1);
#endif
    mainq::init();
    PrefStore::get().start();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    scene::anim::registerCommands();

    QApplication app(argc, argv);
    ui::applyTheme(app);
    ui::actions::registerBuiltins();
    ui::registerAnimationUi();

    for (const char* k : {"Esc", "Alt+.", "Alt+,", ".", ",", "Alt+V", "Shift+W", "S"})
    {
        QKeySequence seq = QKeySequence::fromString(k, QKeySequence::PortableText);
        std::printf("key '%s' -> count %d key 0x%x mods 0x%x\n", k, seq.count(), seq.isEmpty() ? 0 : int(seq[0].key()),
                    seq.isEmpty() ? 0 : int(seq[0].keyboardModifiers()));
    }

    run("spaceLocator", {{"name", "hero"}});
    for (auto [t, x, y, r] : {std::tuple{0.0, 0.0, 0.0, 0.0}, {0.5, 40.0, 25.0, 45.0}, {1.2, 15.0, 25.0, 90.0}, {2.0, 60.0, 5.0, 30.0},
                              {3.0, 30.0, 18.0, 120.0}})
    {
        run("setKeyframe", {{"args", {"hero.tx"}}, {"time", t}, {"value", x}});
        run("setKeyframe", {{"args", {"hero.ty"}}, {"time", t}, {"value", y}});
        run("setKeyframe", {{"args", {"hero.ry"}}, {"time", t}, {"value", r}});
    }
    run("keyTangent", {{"args", {"hero.ty"}}, {"outTangentType", "step"}});
    run("keyTangent", {{"args", {"hero.ry"}}, {"inTangentType", "linear"}, {"outTangentType", "linear"}});
    run("setInfinity", {{"args", {"hero.tx"}}, {"postInfinite", "cycle"}});
    run("select", {{"args", {"hero"}}});
    run("currentTime", {{"args", {1.2}}});

    ui::TimeSlider slider;
    slider.resize(1200, 58);
    slider.show();
    save(&slider, "anim_timeslider.png");

    ui::ChannelBox channels;
    channels.resize(260, 360);
    channels.show();
    save(&channels, "anim_channelbox.png");

    ui::GraphEditor editor;
    editor.resize(1000, 440);
    editor.show();
    pump();
    editor.frame(false);
    save(&editor, "anim_grapheditor.png");

    // Box-select everything in the curve view so the tangent handles show.
    if (auto* view = editor.findChild<QWidget*>("curveView"))
    {
        QPointF a(5, 25), b(view->width() - 5, view->height() - 5);
        mouse(view, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
        mouse(view, QEvent::MouseMove, (a + b) / 2, Qt::NoButton, Qt::LeftButton);
        mouse(view, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
        save(&editor, "anim_grapheditor_box.png");
        mouse(view, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
        save(&editor, "anim_grapheditor_selected.png");

        // Drag one key up through commands, then undo it.
        double before = run("getAttr", {{"args", {"hero.tx"}}}).get<double>();
        mouse(view, QEvent::MouseButtonPress, QPointF(5, 25), Qt::LeftButton, Qt::LeftButton);
        mouse(view, QEvent::MouseButtonRelease, QPointF(5, 25), Qt::LeftButton, Qt::NoButton);
        std::printf("tx at 1.2s before: %.3f\n", before);
    }

    // Real-time playback for half a second.
    run("currentTime", {{"args", {0.0}}});
    run("play");
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 500)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    save(&slider, "anim_timeslider_playing.png");
    run("stop");
    std::printf("time after ~0.5 s of playback: %.3f s (frame %.0f)\n", scene::Graph::current().time(),
                scene::anim::frameOf(scene::Graph::current().time()));

    run("playbackOptions", {{"everyFrame", true}});
    run("currentTime", {{"args", {0.0}}});
    run("play");
    clock.restart();
    while (clock.elapsed() < 500)
        QApplication::processEvents(QEventLoop::AllEvents, 5);
    run("stop");
    std::printf("every-frame playback after ~0.5 s: frame %.0f\n", scene::anim::frameOf(scene::Graph::current().time()));

    PrefStore::get().stop();
    return 0;
}
