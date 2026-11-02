#include "ui/AudioUi.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "media/AudioEngine.h"
#include "media/Media.h"
#include "media/SoundCues.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/SceneData.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/FileDialog.h"

#include <QApplication>
#include <QMenu>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <map>

namespace lila::ui::audio
{
namespace
{
using scene::Graph;
namespace anim = scene::anim;

constexpr double kWavePeakSeconds = 1.0 / 400.0;
const char* const kAudioFilter = "Sound (*.wav *.mp3 *.ogg *.flac *.aac *.m4a *.aif *.aiff *.opus *.mp4 *.mov *.mkv *.webm);;All Files (*)";

struct WavePeaks
{
    std::weak_ptr<const media::AudioBuffer> buffer;
    media::Peaks peaks;
};

const media::Peaks& peaksOf(const media::AudioBufferPtr& b)
{
    static std::map<const media::AudioBuffer*, WavePeaks> cache;
    std::erase_if(cache, [](const auto& e) { return e.second.buffer.expired(); });
    WavePeaks& e = cache[b.get()];
    if (e.buffer.expired())
    {
        e.buffer = b;
        e.peaks = media::peaks(*b, kWavePeakSeconds);
    }
    return e.peaks;
}

struct SoundPlaying
{
    double speed = 0.0; // scene seconds per second; 0 while sound isn't playing
};

SoundPlaying& soundPlaying()
{
    static SoundPlaying p;
    return p;
}

void runAudioCommand(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** Loads every cue's sounds in the background, so the first event of a playback is heard. */
void preloadCueSounds(Graph& g)
{
    for (scene::NodeId id : g.nodes())
        if (g.node(id)->type().isA("soundCue"))
            for (const std::string& s : media::cueOf(g, id).sounds)
                media::loadSound(s, false);
}
} // namespace

void install()
{
    media::addSoundListener([] { notifyTimeChanged(); });
    actions::add({.id = "create.audio",
                  .label = "Audio...",
                  .category = "Create",
                  .run = [] { importAudio(QApplication::activeWindow()); },
                  .doc = "Imports a sound file to play under the time slider.",
                  .python = "audio(fileName='sound/dialogue.wav')"});
}

bool hasWaveform()
{
    return !media::voices(Graph::current(), false).empty();
}

void drawWaveform(QPainter& p, const QRectF& rect, double t0, double t1, const QColor& color)
{
    LILA_ZONE();

    if (rect.width() < 2.0 || !(t1 > t0))
        return;
    const std::vector<media::Voice> voices = media::voices(Graph::current(), false);
    if (voices.empty())
        return;
    const int columns = int(rect.width());
    std::vector<float> low(size_t(columns), 0.0f), high(size_t(columns), 0.0f);
    bool any = false;
    const double perPx = (t1 - t0) / rect.width();
    for (const media::Voice& v : voices)
    {
        const media::Peaks& pk = peaksOf(v.buffer);
        if (pk.low.empty())
            continue;
        const float gain = float(v.gain);
        for (int x = 0; x < columns; ++x)
        {
            const double a = (t0 + x * perPx - v.start) / pk.secondsPerBucket;
            const double b = (t0 + (x + 1) * perPx - v.start) / pk.secondsPerBucket;
            if (b < 0.0 || a >= double(pk.low.size()))
                continue;
            const size_t from = size_t(std::max(0.0, std::floor(a)));
            const size_t to = std::min(pk.low.size(), size_t(std::max(std::ceil(b), double(from + 1))));
            for (size_t k = from; k < to; ++k)
            {
                low[size_t(x)] = std::min(low[size_t(x)], pk.low[k] * gain);
                high[size_t(x)] = std::max(high[size_t(x)], pk.high[k] * gain);
            }
            any = true;
        }
    }
    if (!any)
        return;
    const double mid = rect.center().y(), half = rect.height() * 0.5;
    p.save();
    p.setPen(QPen(color, 1.0));
    for (int x = 0; x < columns; ++x)
    {
        if (high[size_t(x)] <= low[size_t(x)])
            continue;
        const double px = rect.left() + x + 0.5;
        p.drawLine(QPointF(px, mid - std::clamp(double(high[size_t(x)]), -1.0, 1.0) * half),
                   QPointF(px, mid - std::clamp(double(low[size_t(x)]), -1.0, 1.0) * half));
    }
    p.restore();
}

void fillMenu(QMenu* menu)
{
    Graph& g = Graph::current();
    const scene::data::json pickedJson = scene::data::get(media::kSoundKey);
    const std::string picked = pickedJson.is_string() ? pickedJson.get<std::string>() : std::string();
    QAction* all = menu->addAction("All", [] { runAudioCommand("timeSliderSound", {{"sound", ""}}); });
    all->setCheckable(true);
    all->setChecked(picked.empty());
    bool first = true;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!media::isAudio(*n))
            continue;
        if (std::exchange(first, false))
            menu->addSeparator();
        const std::string name = n->path();
        QAction* a = menu->addAction(QString::fromStdString(name), [name] { runAudioCommand("timeSliderSound", {{"sound", name}}); });
        a->setCheckable(true);
        a->setChecked(picked == name || (picked.empty() && media::isLive(g, id) && !n->as<bool>(media::AudioNode::kMute)));
    }
    menu->addSeparator();
    menu->addAction("Import Audio...", [menu] { importAudio(menu->parentWidget()); });
}

void importAudio(QWidget* parent)
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    const fileDialog::ContextScope scope("audio", "Sound");
    QString file = fileDialog::getOpenFileName(parent, "Import Audio", start, kAudioFilter);
    if (file.isEmpty())
        return;
    runAudioCommand("audio", {{"fileName", file.toStdString()}, {"offset", std::round(anim::frameOf(anim::playback().minTime))}});
}

void playbackStarted(double sceneTime, int dir, double speed)
{
    Graph& g = Graph::current();
    preloadCueSounds(g);
    soundPlaying().speed = 0.0;
    std::vector<media::Voice> v = media::voices(g, false);
    if (v.empty() || speed <= 0.0)
        return;
    media::AudioEngine& e = media::AudioEngine::get();
    if (!e.ensureDevice())
        return;
    e.setVoices(std::move(v));
    soundPlaying().speed = dir * speed;
    e.play(sceneTime, soundPlaying().speed);
}

void playbackStopped()
{
    soundPlaying().speed = 0.0;
    media::AudioEngine::get().stop();
}

double audioClock()
{
    if (soundPlaying().speed == 0.0)
        return -1.0;
    std::optional<double> t = media::AudioEngine::get().clock();
    return t ? *t : -1.0;
}

void playbackJumped(double sceneTime)
{
    if (soundPlaying().speed != 0.0)
        media::AudioEngine::get().play(sceneTime, soundPlaying().speed);
}

void playbackAdvanced(double t0, double t1)
{
    if (!(t1 > t0))
        return;
    Graph& g = Graph::current();
    std::vector<media::Trigger> fired = media::sceneTriggers(g, t0, t1);
    if (fired.empty())
        return;
    media::AudioEngine& e = media::AudioEngine::get();
    if (!e.ensureDevice())
        return;
    for (const media::Trigger& t : fired)
        if (media::AudioBufferPtr b = media::loadSound(t.sound, false))
            e.fire(b, t.gain);
}

void scrubbed(double sceneTime)
{
    if (anim::playDirection() != 0)
        return;
    std::vector<media::Voice> v = media::voices(Graph::current(), false);
    if (v.empty())
        return;
    media::AudioEngine& e = media::AudioEngine::get();
    if (!e.ensureDevice())
        return;
    e.setVoices(std::move(v));
    e.scrub(sceneTime, std::max(1.0 / std::max(1.0, anim::playback().fps), 0.06));
}
} // namespace lila::ui::audio
