#include "media/Media.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/ImagePlane.h"
#include "scene/SceneData.h"
#include "scene/Take.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>

namespace lila::media
{
namespace
{
using scene::Node;
using scene::PlugRef;
using scene::PlugType;
using scene::anim::TakeNode;

constexpr uint32_t kStored = scene::AttrInput | scene::AttrStorable;

int mediaAttr(const Node& n)
{
    return isAudio(n) ? int(AudioNode::kMedia) : int(scene::ImagePlane::kMedia);
}

struct SoundEntry
{
    AudioBufferPtr buffer;
    fs::file_time_type stamp{};
    bool loading = false;
    std::string error;
};

std::map<std::string, SoundEntry>& sounds()
{
    static std::map<std::string, SoundEntry> m;
    return m;
}

std::map<int, std::function<void()>>& soundListeners()
{
    static std::map<int, std::function<void()>> m;
    return m;
}

fs::file_time_type stampOf(const fs::path& p)
{
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(p, ec);
    return ec ? fs::file_time_type{} : t;
}

Node* nodeNamed(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return n;
}

json mediaJson(Graph& g, NodeId id)
{
    Node& n = *g.node(id);
    json takes = json::array();
    for (NodeId t : takesOf(g, id))
        takes.push_back(g.node(t)->path());
    const bool audio = isAudio(n);
    const std::string file = audio ? n.as<std::string>(AudioNode::kFileName) : n.as<std::string>(scene::ImagePlane::kImageName);
    const double startFrame = audio ? n.as<double>(AudioNode::kOffset) : n.as<double>(scene::ImagePlane::kStartFrame);
    return {{"name", n.path()}, {"type", n.type().name}, {"file", file}, {"live", isLive(g, id)},
            {"takes", takes}, {"start", startSeconds(g, id, startFrame)}};
}
} // namespace

bool isAudio(const Node& n)
{
    return n.type().isA("audio");
}

bool isMedia(const Node& n)
{
    return isAudio(n) || n.type().isA("imagePlane");
}

std::vector<NodeId> takesOf(Graph& g, NodeId media)
{
    std::vector<NodeId> out;
    Node* n = g.node(media);
    if (!n || !isMedia(*n))
        return out;
    for (const PlugRef& d : g.destinations({media, mediaAttr(*n), -1}))
        if (Node* t = g.node(d.node); t && scene::anim::isTake(*t) && d.attr == TakeNode::kMedia)
            out.push_back(d.node);
    std::vector<NodeId> order = scene::anim::takes(g);
    std::ranges::sort(out, [&](NodeId a, NodeId b) { return std::ranges::find(order, a) < std::ranges::find(order, b); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<NodeId> mediaOf(Graph& g, NodeId take)
{
    std::vector<NodeId> out;
    for (const PlugRef& e : g.elements({take, TakeNode::kMedia, -1}))
        if (auto src = g.source(e))
            out.push_back(src->node);
    return out;
}

void bind(Graph& g, NodeId take, NodeId media)
{
    Node* t = g.node(take);
    Node* m = g.node(media);
    if (!t || !scene::anim::isTake(*t))
        throw std::invalid_argument("bindMedia: not a take");
    if (!m || !isMedia(*m))
        throw std::invalid_argument("bindMedia: " + (m ? m->path() : std::string("?")) + " is not an audio node or image plane");
    if (std::vector<NodeId> has = mediaOf(g, take); std::ranges::find(has, media) != has.end())
        return;
    g.connect({media, mediaAttr(*m), -1}, {take, TakeNode::kMedia, -1});
}

void unbind(Graph& g, NodeId take, NodeId media)
{
    for (const PlugRef& e : g.elements({take, TakeNode::kMedia, -1}))
        if (auto src = g.source(e); src && src->node == media)
            g.removeElement(e);
}

bool isLive(Graph& g, NodeId media)
{
    std::vector<NodeId> takes = takesOf(g, media);
    return takes.empty() || std::ranges::any_of(takes, [&](NodeId t) { return g.node(t)->as<bool>(TakeNode::kActive); });
}

double startSeconds(Graph& g, NodeId media, double startFrame)
{
    double base = 0.0;
    std::vector<NodeId> takes = takesOf(g, media);
    auto active = std::ranges::find_if(takes, [&](NodeId t) { return g.node(t)->as<bool>(TakeNode::kActive); });
    if (active != takes.end())
        base = g.node(*active)->as<double>(TakeNode::kStart);
    else if (!takes.empty())
        base = g.node(takes.front())->as<double>(TakeNode::kStart);
    return base + scene::anim::secondsOf(startFrame);
}

std::vector<NodeId> audibleSounds(Graph& g)
{
    std::vector<NodeId> out;
    const scene::data::json picked = scene::data::get(kSoundKey);
    if (picked.is_string() && !picked.get<std::string>().empty())
    {
        if (Node* n = g.find(picked.get<std::string>()); n && isAudio(*n) && !n->as<bool>(AudioNode::kMute))
            out.push_back(n->id());
        return out;
    }
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); isAudio(*n) && !n->as<bool>(AudioNode::kMute) && isLive(g, id))
            out.push_back(id);
    return out;
}

AudioBufferPtr loadSound(const std::string& stored, bool wait, std::string* err)
{
    if (stored.empty())
        return nullptr;
    const fs::path path = paths::resolvePath(stored);
    const std::string key = paths::toUtf8(path);
    SoundEntry& e = sounds()[key];
    const fs::file_time_type stamp = stampOf(path);
    if (e.buffer && e.stamp == stamp)
        return e.buffer;
    if (!e.error.empty() && e.stamp == stamp)
    {
        if (err)
            *err = e.error;
        return nullptr;
    }
    if (wait)
    {
        std::string why;
        e.buffer = decodeAudio(path, kSampleRate, &why);
        e.stamp = stamp;
        e.error = e.buffer ? std::string() : why;
        if (err)
            *err = e.error;
        return e.buffer;
    }
    if (e.loading)
        return e.buffer;
    e.loading = true;
    std::thread([key, path, stamp] {
        std::string why;
        AudioBufferPtr b = decodeAudio(path, kSampleRate, &why);
        mainq::post([key, b, why, stamp] {
            SoundEntry& d = sounds()[key];
            d.loading = false;
            d.buffer = b;
            d.stamp = stamp;
            d.error = b ? std::string() : why;
            if (!b)
                log::warn("audio: {}", why);
            auto copy = soundListeners();
            for (auto& [id, fn] : copy)
                fn();
        });
    }).detach();
    return e.buffer;
}

int addSoundListener(std::function<void()> fn)
{
    static int next = 1;
    soundListeners().emplace(next, std::move(fn));
    return next++;
}

void removeSoundListener(int id)
{
    soundListeners().erase(id);
}

std::vector<Voice> voices(Graph& g, bool wait)
{
    std::vector<Voice> out;
    for (NodeId id : audibleSounds(g))
    {
        const Node& n = *g.node(id);
        AudioBufferPtr b = loadSound(n.as<std::string>(AudioNode::kFileName), wait);
        if (!b)
            continue;
        out.push_back({b, startSeconds(g, id, n.as<double>(AudioNode::kOffset)), n.as<double>(AudioNode::kGain), n.path()});
    }
    return out;
}

void mix(const std::vector<Voice>& voices, double t0, double speed, int rate, float* out, int frames)
{
    std::fill(out, out + size_t(frames) * 2, 0.0f);
    const double step = speed / double(rate);
    for (const Voice& v : voices)
    {
        if (!v.buffer || v.buffer->channels != 2 || v.gain == 0.0)
            continue;
        const AudioBuffer& b = *v.buffer;
        const int64_t last = b.frames() - 1;
        const float gain = float(v.gain);
        const double perSecond = double(b.rate);
        for (int i = 0; i < frames; ++i)
        {
            const double s = (t0 + i * step - v.start) * perSecond;
            if (s < 0.0 || s >= double(last))
                continue;
            const int64_t k = int64_t(s);
            const float f = float(s - double(k));
            const float* a = b.samples.data() + size_t(k) * 2;
            out[i * 2] += gain * (a[0] + (a[2] - a[0]) * f);
            out[i * 2 + 1] += gain * (a[1] + (a[3] - a[1]) * f);
        }
    }
}

int64_t videoFrame(const VideoTiming& timing, double fileFps, double sceneSeconds)
{
    const double fps = timing.fps > 0.0 ? timing.fps : (fileFps > 0.0 ? fileFps : 24.0);
    return int64_t(std::floor((sceneSeconds - timing.start) * fps + 1e-6)) + timing.offset;
}

void registerMediaTypes()
{
    if (scene::types::find("audio"))
        return;
    scene::NodeType t;
    t.name = "audio";
    t.icon = "music";
    t.doc = "A sound file on the timeline: heard during playback and scrubbing, drawn as a waveform in the time slider.";
    t.ownAttrs = {
        {.name = "fileName", .type = PlugType::String, .defaultValue = std::string(), .flags = kStored | scene::AttrFile,
         .doc = "Sound file (WAV, MP3, OGG, FLAC, AAC, M4A, AIFF, OPUS) or the sound of a video. Relative paths resolve from the workspace root."},
        {.name = "offset", .type = PlugType::Float, .defaultValue = 0.0, .flags = kStored,
         .doc = "Frame the sound starts at; counted from the take's start while it belongs to takes."},
        {.name = "gain", .type = PlugType::Float, .defaultValue = 1.0, .flags = kStored, .doc = "Volume multiplier.", .minValue = 0.0,
         .maxValue = 4.0},
        {.name = "mute", .type = PlugType::Bool, .defaultValue = false, .flags = kStored, .doc = "Silent, but still drawn."},
        {.name = "media", .type = PlugType::Bool, .defaultValue = false, .flags = scene::AttrOutput | scene::AttrHidden,
         .doc = "Connected to the media of the takes it belongs to."},
    };
    t.create = [] { return std::make_unique<AudioNode>(); };
    scene::types::registerType(std::move(t));
}

void registerMediaCommands()
{
    using T = ArgType;
    if (ops::find("audio"))
        return;

    ops::registerCommand({
        .name = "audio",
        .doc = "Create an audio node from a sound file, or edit the one named. Returns its name.\n\n"
               "Audio plays under the time slider during playback and scrubbing. With take, it belongs to that take: heard only "
               "while the take is active, its offset counted from the take's start.",
        .positionalDoc = "audio node to edit (omit to create one)",
        .flags = {{"fileName", "f", T::String, nullptr, "Sound file; stored relative to the workspace root."},
                  {"offset", "o", T::Float, nullptr, "Frame the sound starts at."},
                  {"gain", "g", T::Float, nullptr, "Volume multiplier.", {}, {}, 0.0, 4.0},
                  {"mute", "m", T::Bool, nullptr, "Silence it."},
                  {"take", "t", T::String, nullptr, "Take it belongs to."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            Node* n = nullptr;
            if (!a.positional.empty())
            {
                n = nodeNamed(g, a.positional[0].get<std::string>(), "audio");
                if (!isAudio(*n))
                    throw CommandError("audio: " + n->path() + " is not an audio node");
            }
            else
            {
                std::string name = a.get<std::string>("name");
                if (name.empty() && a.has("fileName"))
                {
                    name = paths::toUtf8(paths::fromUtf8(a.get<std::string>("fileName")).stem());
                    if (!Graph::validName(name))
                        name.clear();
                }
                n = g.createNode(*scene::types::find("audio"), name.empty() ? "audio" : name);
            }
            const NodeId id = n->id();
            if (a.has("fileName"))
            {
                const std::string file = a.get<std::string>("fileName");
                g.setValue({id, AudioNode::kFileName}, paths::storedPath(paths::fromUtf8(file)));
            }
            if (a.has("offset"))
                g.setValue({id, AudioNode::kOffset}, a.get<double>("offset"));
            if (a.has("gain"))
                g.setValue({id, AudioNode::kGain}, std::clamp(a.get<double>("gain"), 0.0, 4.0));
            if (a.has("mute"))
                g.setValue({id, AudioNode::kMute}, a.get<bool>("mute"));
            if (a.has("take"))
            {
                Node* t = nodeNamed(g, a.get<std::string>("take"), "audio");
                try
                {
                    bind(g, t->id(), id);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("audio: ") + e.what());
                }
            }
            return g.node(id)->path();
        },
        .examples = {"audio(fileName='sound/dialogue.wav', offset=12)"},
    });

    ops::registerCommand({
        .name = "bindMedia",
        .doc = "Give a take its own audio and reference video: the media plays only while the take is active and starts "
               "from the take's start. remove takes them off the take again.",
        .positionalDoc = "take, then audio nodes or image planes",
        .flags = {{"remove", "rm", T::Bool, false, "Unbind instead."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.size() < 2)
                throw CommandError("bindMedia: give a take and one or more media nodes");
            Node* t = nodeNamed(g, a.positional[0].get<std::string>(), "bindMedia");
            if (!scene::anim::isTake(*t))
                throw CommandError("bindMedia: " + t->path() + " is not a take");
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                Node* m = nodeNamed(g, a.positional[i].get<std::string>(), "bindMedia");
                try
                {
                    if (a.get<bool>("remove"))
                        unbind(g, t->id(), m->id());
                    else
                        bind(g, t->id(), m->id());
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(e.what());
                }
            }
            json out = json::array();
            for (NodeId m : mediaOf(g, t->id()))
                out.push_back(g.node(m)->path());
            return out;
        },
        .examples = {"bindMedia('Walk', 'walkVO', 'walkRef')"},
    });

    ops::registerCommand({
        .name = "timeSliderSound",
        .doc = "The sound the time slider plays and draws (Maya's Time Slider > Audio). Without sound, returns it; an empty "
               "sound plays every audio node of the active takes.",
        .flags = {{"sound", "s", T::String, nullptr, "Audio node to play alone, or empty for all."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.has("sound"))
            {
                const std::string s = a.get<std::string>("sound");
                if (s.empty())
                    scene::data::set(kSoundKey, nullptr);
                else
                {
                    Node* n = nodeNamed(g, s, "timeSliderSound");
                    if (!isAudio(*n))
                        throw CommandError("timeSliderSound: " + n->path() + " is not an audio node");
                    scene::data::set(kSoundKey, n->path());
                }
                scene::anim::playbackChanged();
            }
            json v = scene::data::get(kSoundKey);
            return v.is_string() ? v : json(std::string());
        },
    });

    ops::registerCommand({
        .name = "listMedia",
        .doc = "Every audio node and image plane with its file, whether it plays now (live), the takes it belongs to and "
               "the scene time its start falls on (seconds).",
        .undoable = false,
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (NodeId id : g.nodes())
                if (isMedia(*g.node(id)))
                    out.push_back(mediaJson(g, id));
            return out;
        },
    });
}
} // namespace lila::media
