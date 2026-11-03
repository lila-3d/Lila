#include "ui/Animation.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AudioUi.h"
#include "ui/DockSidebar.h"
#include "ui/GraphEditor.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"

#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QPointer>
#include <QTimer>

#include <cmath>

namespace lila::ui
{
namespace
{
namespace anim = scene::anim;
using scene::Graph;

struct TimeHub
{
    QTimer* poll = nullptr;
    double last = std::nan("");
    std::vector<std::pair<QPointer<QObject>, std::function<void()>>> listeners;

    void fire()
    {
        last = Graph::current().time();
        std::erase_if(listeners, [](const auto& l) { return l.first.isNull(); });
        auto copy = listeners;
        for (auto& [owner, fn] : copy)
            if (owner)
                fn();
    }
};

TimeHub& hub()
{
    static TimeHub h;
    if (!h.poll && qApp)
    {
        h.poll = new QTimer(qApp);
        h.poll->setInterval(30);
        QObject::connect(h.poll, &QTimer::timeout, [] {
            if (Graph::current().time() != hub().last)
                hub().fire();
        });
        h.poll->start();
    }
    return h;
}

void setTime(double seconds)
{
    ops::execute("currentTime", {{"args", {seconds}}});
    notifyTimeChanged();
}

/** Steps the scene time through a PlaybackClock at the playback speed. */
struct Player
{
    QTimer* timer = nullptr;
    QElapsedTimer wall;
    anim::PlaybackClock clock;
    anim::PlaybackSpeed speed = anim::PlaybackSpeed::RealTime;
    double customSpeed = 1.0;
    int direction = 0;
    /** When the every-frame speeds last stepped. */
    double lastStep = -1.0;

    double now() const { return double(wall.nsecsElapsed()) * 1e-9; }

    void start(int dir)
    {
        if (!timer)
        {
            timer = new QTimer(qApp);
            timer->setTimerType(Qt::PreciseTimer);
            QObject::connect(timer, &QTimer::timeout, [this] { tick(); });
        }
        const anim::Playback& p = anim::playback();
        direction = dir;
        double t = anim::snapToFrame(Graph::current().time());
        if (t < p.minTime - 1e-9 || t > p.maxTime + 1e-9 || (dir > 0 && t >= p.maxTime && p.loop) || (dir < 0 && t <= p.minTime && p.loop))
            t = dir > 0 ? p.minTime : p.maxTime;
        setTime(t);
        wall.start();
        lastStep = -1.0;
        clock.reset(std::llround(anim::frameOf(t)), now());
        startTimer();
        audio::playbackStarted(t, dir, soundSpeed(p));
    }

    /** Real-time multiplier the sound plays at; 0 for the every-frame speeds, which play silent as in Maya. */
    static double soundSpeed(const anim::Playback& p) { return anim::playsEveryFrame(p.speed) ? 0.0 : anim::speedScale(p.speed, p.customSpeed); }

    void startTimer()
    {
        speed = anim::playback().speed;
        customSpeed = anim::playback().customSpeed;
        // Free playback ticks whenever the event loop is idle; the clocked speeds poll often enough to hit each frame.
        timer->start(speed == anim::PlaybackSpeed::EveryFrame && viewportDisplay::frameInterval() <= 0.0 ? 0 : 2);
    }

    void stop()
    {
        direction = 0;
        if (timer)
            timer->stop();
        audio::playbackStopped();
        notifyTimeChanged();
    }

    void tick()
    {
        LILA_ZONE();

        const anim::Playback& p = anim::playback();
        int64_t first = std::llround(anim::frameOf(p.minTime));
        int64_t last = std::llround(anim::frameOf(p.maxTime));
        const double before = Graph::current().time();
        const int64_t shown = std::llround(anim::frameOf(before));
        if (shown != clock.frame)
        {
            clock.reset(shown, now());
            audio::playbackJumped(anim::secondsOf(double(shown)));
        }
        if (p.speed != speed || p.customSpeed != customSpeed)
        {
            clock.reset(clock.frame, now());
            startTimer();
            audio::playbackStopped();
            audio::playbackStarted(anim::secondsOf(double(clock.frame)), direction, soundSpeed(p));
        }
        // While sound plays it is the clock, so the picture shows the frame being heard.
        if (const double heard = audio::audioClock(); heard >= 0.0)
        {
            const double f = anim::frameOf(heard);
            const int64_t next = direction > 0 ? int64_t(std::floor(f + 1e-6)) : int64_t(std::ceil(f - 1e-6));
            if (next == clock.frame)
                return;
            clock.frame = next;
        }
        else
        {
            // Every-frame speeds step no faster than the viewport's frame cap draws, or the frames between draws never show.
            const double interval = viewportDisplay::frameInterval();
            if (anim::playsEveryFrame(p.speed) && now() - lastStep < interval)
                return;
            if (!clock.advance(p.speed, p.fps, direction, now(), p.customSpeed))
                return;
            // On the cap's schedule, so the timer's late ticks don't add up to a rate under it.
            lastStep = now() - lastStep < 2.0 * interval ? lastStep + interval : now();
        }
        int64_t frame = clock.frame;
        bool wrapped = false;
        if (frame > last || frame < first)
        {
            if (!p.loop)
            {
                setTime(anim::secondsOf(double(direction > 0 ? last : first)));
                anim::stop();
                return;
            }
            frame = direction > 0 ? first : last;
            clock.reset(frame, now());
            audio::playbackJumped(anim::secondsOf(double(frame)));
            wrapped = true;
        }
        setTime(anim::secondsOf(double(frame)));
        if (direction > 0 && !wrapped)
            audio::playbackAdvanced(before, Graph::current().time());
    }
};

Player& player()
{
    static Player p;
    return p;
}

void stepFrame(int delta)
{
    const anim::Playback& p = anim::playback();
    double frame = std::round(anim::frameOf(Graph::current().time())) + delta;
    double first = std::round(anim::frameOf(p.minTime)), last = std::round(anim::frameOf(p.maxTime));
    if (frame > last)
        frame = first;
    else if (frame < first)
        frame = last;
    setTime(anim::secondsOf(frame));
    audio::scrubbed(anim::secondsOf(frame));
}

void stepKey(const char* which)
{
    try
    {
        json t = ops::execute("findKeyframe", {{"which", which}});
        if (t.is_number())
            setTime(t.get<double>());
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}
} // namespace

void onTimeChanged(QObject* owner, std::function<void()> fn)
{
    hub().listeners.emplace_back(owner, std::move(fn));
}

void notifyTimeChanged()
{
    hub().fire();
}

void registerAnimationUi()
{
    hub();
    anim::setPlaybackDriver({.start = [](int dir) { player().start(dir); }, .stop = [] { player().stop(); }});

    auto add = [](const char* id, const char* label, std::function<void()> run, const char* doc, std::string python) {
        actions::add({.id = id, .label = label, .category = "Animation", .run = std::move(run), .doc = doc, .python = std::move(python)});
    };
    add("window.graphEditor", "Graph Editor", [] { toggleDock(findDock(kGraphEditorDock)); },"Curves of the selection's animated channels.", "lila.ui.run_action(\"window.graphEditor\")");
    add("window.timeSlider", "Time Slider", [] {
            if (QDockWidget* d = findDock(kTimeSliderDock))
                d->setVisible(!d->isVisible());
        }, "Show or hide the time and range sliders.", "lila.ui.run_action(\"window.timeSlider\")");
    add("anim.play", "Play / Stop", [] {
            if (anim::playDirection() != 0)
                runUi("stop");
            else
                runUi("play");
        }, "Toggle forward playback.", "play()");
    add("anim.playBackward", "Play Backwards", [] { runUi("play", {{"forward", false}}); }, "Play in reverse.", "play(forward=False)");
    add("anim.stop", "Stop", [] {
            if (anim::playDirection() != 0)
                runUi("stop");
        }, "Stop playback.", "stop()");
    for (anim::PlaybackSpeed speed : anim::kPlaybackSpeeds)
    {
        if (speed == anim::PlaybackSpeed::Custom)
            continue;
        std::string name(anim::speedName(speed));
        std::string id = "anim.speed." + name;
        actions::add({.id = id, .label = std::string(anim::speedLabel(speed)), .category = "Animation",
                      .run = [name] { runUi("playbackOptions", {{"playbackSpeed", name}}); },
                      .doc = "Playback speed (also in Preferences > Time Slider and the time slider's right-click menu).",
                      .python = "playbackOptions(playbackSpeed='" + name + "')"});
    }
    actions::add({.id = "anim.speed.custom", .label = "Custom Speed...", .category = "Animation",
                  .run = [] {
                      bool ok = false;
                      double v = QInputDialog::getDouble(QApplication::activeWindow(), "Custom Playback Speed", "Real-time multiplier:",
                                                         anim::playback().customSpeed, anim::kMinCustomSpeed, anim::kMaxCustomSpeed, 2,
                                                         &ok, Qt::WindowFlags(), 0.25);
                      if (ok)
                          runUi("playbackOptions", {{"playbackSpeed", "custom"}, {"speedMultiplier", v}});
                  },
                  .doc = "Plays at real time times a multiplier you enter, e.g. 0.25 or 1.5.",
                  .python = "playbackOptions(speedMultiplier=0.5)"});
    add("anim.nextFrame", "Next Frame", [] { stepFrame(1); }, "One frame forward, wrapping at the range end.", "");
    add("anim.prevFrame", "Previous Frame", [] { stepFrame(-1); }, "One frame back, wrapping at the range start.", "");
    add("anim.firstFrame", "Go to Start", [] { setTime(anim::playback().minTime); }, "Start of the playback range.", "");
    add("anim.lastFrame", "Go to End", [] { setTime(anim::playback().maxTime); }, "End of the playback range.", "");
    add("anim.nextKey", "Next Key", [] { stepKey("next"); }, "Next key of the selection.", "currentTime(findKeyframe(which='next'))");
    add("anim.prevKey", "Previous Key", [] { stepKey("previous"); }, "Previous key of the selection.",
        "currentTime(findKeyframe(which='previous'))");
    add("anim.setKey", "Set Key", [] { runUi("setKeyframe"); }, "Key every keyable channel of the selection.", "setKeyframe()");
    add("anim.setKeyTranslate", "Key Translate", [] { runUi("setKeyframe", {{"attribute", "translate"}}); }, "Key translate.",
        "setKeyframe(attribute='translate')");
    add("anim.setKeyRotate", "Key Rotate", [] { runUi("setKeyframe", {{"attribute", "rotate"}}); }, "Key rotate.",
        "setKeyframe(attribute='rotate')");
    add("anim.setKeyScale", "Key Scale", [] { runUi("setKeyframe", {{"attribute", "scale"}}); }, "Key scale.",
        "setKeyframe(attribute='scale')");
    add("anim.autoKeyToggle", "Auto Key", [] { runUi("autoKeyframe", {{"state", !anim::autoKey()}}); },
        "Key animated channels whenever they change.", "autoKeyframe(state=not autoKeyframe())");
    add("anim.deleteKeys", "Delete Keys", [] {
            if (GraphEditor* ge = GraphEditor::instance())
                ge->deleteSelectedKeys();
        }, "Delete the keys selected in the graph editor.", "");
    add("graphEditor.frameSelected", "Frame Selected Keys", [] {
            if (GraphEditor* ge = GraphEditor::instance())
                ge->frame(true);
        }, "Fit the graph editor to the selected keys.", "");
    add("graphEditor.frameAll", "Frame All Curves", [] {
            if (GraphEditor* ge = GraphEditor::instance())
                ge->frame(false);
        }, "Fit the graph editor to every curve shown.", "");
}
} // namespace lila::ui
