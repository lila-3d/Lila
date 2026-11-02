#include "media/SoundCues.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/ObjectSets.h"
#include "seq/RigidPieces.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace lila::media
{
namespace
{
using scene::Node;
using scene::PlugType;
using T = ArgType;
namespace fs = std::filesystem;

constexpr uint32_t kCueStored = scene::AttrInput | scene::AttrStorable;
const std::vector<std::string> kEventTypeNames = {"any", "contact", "break"};

std::string shortName(const std::string& path)
{
    const size_t bar = path.find_last_of('|');
    return bar == std::string::npos ? path : path.substr(bar + 1);
}

uint64_t mixHash(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t hashString(const std::string& s)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

std::vector<std::string> splitWords(const std::string& s)
{
    std::vector<std::string> out;
    std::string word;
    for (char c : s + " ")
        if (c == ' ' || c == ',' || c == '\n' || c == '\r' || c == '\t' || c == ';')
        {
            if (!word.empty())
                out.push_back(std::exchange(word, {}));
        }
        else
            word += c;
    return out;
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string line; std::getline(in, line);)
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        size_t first = line.find_first_not_of(' ');
        if (first != std::string::npos)
            out.push_back(line.substr(first));
    }
    return out;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& l : lines)
        out += (out.empty() ? "" : "\n") + l;
    return out;
}

json triggerJson(const Trigger& t)
{
    return {{"time", t.time},
            {"frame", scene::anim::frameOf(t.time)},
            {"sound", t.sound},
            {"gain", t.gain},
            {"cue", t.cue},
            {"event", t.event.type == EventType::Break ? "break" : "contact"},
            {"object", t.event.object},
            {"piece", t.event.piece},
            {"strength", t.event.strength},
            {"position", {t.event.position.x, t.event.position.y, t.event.position.z}}};
}

std::pair<double, double> rangeOf(const Args& a)
{
    const auto& p = scene::anim::playback();
    const double start = a.has("start") ? scene::anim::secondsOf(a.get<double>("start")) : p.minTime;
    const double end = a.has("end") ? scene::anim::secondsOf(a.get<double>("end")) : p.maxTime;
    // Events at the first frame belong to the range, and the window is open at its start.
    return {start - 0.5 / std::max(1.0, p.fps), end};
}
} // namespace

Registry<EventSource>& eventSources()
{
    static Registry<EventSource> r;
    return r;
}

std::vector<SimEvent> collectEvents(Graph& g, double t0, double t1)
{
    LILA_ZONE();

    std::vector<SimEvent> out;
    for (EventSource* s : eventSources().all())
        if (s->events)
        {
            std::vector<SimEvent> e = s->events(g, t0, t1);
            out.insert(out.end(), e.begin(), e.end());
        }
    std::stable_sort(out.begin(), out.end(), [](const SimEvent& a, const SimEvent& b) { return a.time < b.time; });
    return out;
}

bool matches(const Cue& cue, const SimEvent& e)
{
    if (!(cue.types & (1 << int(e.type))) || e.strength < cue.minStrength)
        return false;
    if (cue.objects.empty())
        return true;
    const std::string own = shortName(e.object);
    return std::ranges::any_of(cue.objects, [&](const std::string& o) { return o == e.object || shortName(o) == own; });
}

std::vector<Trigger> triggers(const Cue& cue, const std::vector<SimEvent>& events)
{
    std::vector<Trigger> out;
    if (cue.sounds.empty())
        return out;
    std::map<std::pair<std::string, int>, double> last;
    const double span = std::max(cue.fullStrength - cue.minStrength, 1e-9);
    for (const SimEvent& e : events)
    {
        if (!matches(cue, e))
            continue;
        const auto key = std::make_pair(e.object, e.piece);
        if (auto it = last.find(key); it != last.end() && e.time - it->second < cue.minInterval)
            continue;
        last[key] = e.time;
        const double k = std::clamp((e.strength - cue.minStrength) / span, 0.0, 1.0);
        uint64_t h = mixHash(uint64_t(cue.seed), uint64_t(std::llround(e.time * 1000.0)));
        h = mixHash(h, hashString(e.object));
        h = mixHash(h, uint64_t(int64_t(e.piece)));
        h = mixHash(h, uint64_t(e.type));
        Trigger t;
        t.time = e.time;
        t.sound = cue.sounds[size_t(h % cue.sounds.size())];
        t.gain = cue.volume * (cue.minVolume + (1.0 - cue.minVolume) * k);
        t.cue = cue.name;
        t.event = e;
        out.push_back(std::move(t));
    }
    return out;
}

Cue cueOf(Graph& g, NodeId id)
{
    const Node& n = *g.node(id);
    Cue c;
    c.name = n.path();
    const int64_t type = n.as<int64_t>(SoundCueNode::kEventType);
    c.types = type == 1 ? 0b01 : type == 2 ? 0b10 : 0b11;
    c.minStrength = n.as<double>(SoundCueNode::kMinStrength);
    c.fullStrength = n.as<double>(SoundCueNode::kFullStrength);
    for (const std::string& name : splitWords(n.as<std::string>(SoundCueNode::kTargets)))
    {
        Node* o = g.find(name);
        if (o && scene::objectSets::isSet(*o))
            for (NodeId m : scene::objectSets::nodesOf(g, o->id()))
                c.objects.push_back(g.node(m)->path());
        else
            c.objects.push_back(o ? o->path() : name);
    }
    c.sounds = splitLines(n.as<std::string>(SoundCueNode::kSounds));
    c.volume = n.as<double>(SoundCueNode::kVolume);
    c.minVolume = n.as<double>(SoundCueNode::kMinVolume);
    c.minInterval = n.as<double>(SoundCueNode::kMinInterval);
    c.seed = n.as<int64_t>(SoundCueNode::kSeed);
    return c;
}

std::vector<Trigger> sceneTriggers(Graph& g, double t0, double t1)
{
    std::vector<NodeId> cues;
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n->type().isA("soundCue") && !n->as<bool>(SoundCueNode::kMute))
            cues.push_back(id);
    std::vector<Trigger> out;
    if (cues.empty())
        return out;
    const std::vector<SimEvent> events = collectEvents(g, t0, t1);
    for (NodeId id : cues)
    {
        std::vector<Trigger> t = triggers(cueOf(g, id), events);
        out.insert(out.end(), t.begin(), t.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const Trigger& a, const Trigger& b) { return a.time < b.time; });
    return out;
}

void registerSoundCueTypes()
{
    if (scene::types::find("soundCue"))
        return;
    scene::NodeType t;
    t.name = "soundCue";
    t.icon = "volume";
    t.doc = "Sounds fired by simulation events (pieces hitting, links breaking), one picked at random per event, louder the harder it was.";
    auto num = [](std::string name, double def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {}) {
        return scene::AttrDef{.name = std::move(name), .type = PlugType::Float, .defaultValue = def, .flags = kCueStored, .doc = std::move(doc),
                              .minValue = lo, .maxValue = hi};
    };
    t.ownAttrs = {
        {.name = "eventType", .type = PlugType::Enum, .defaultValue = int64_t(0), .flags = kCueStored, .enumNames = kEventTypeNames,
         .doc = "Events it answers: any, contact (pieces hitting) or break (links giving way)."},
        num("minStrength", 0.0, "Weaker events are silent: a break's impulse in N s, a contact's speed change in cm/s.", 0.0),
        num("fullStrength", 1000.0, "Events this strong or stronger play at full volume.", 0.0),
        {.name = "targets", .type = PlugType::String, .defaultValue = std::string(), .flags = kCueStored,
         .doc = "Nodes or object sets whose events it answers, space separated; empty for all."},
        {.name = "sounds", .type = PlugType::String, .defaultValue = std::string(), .flags = kCueStored | scene::AttrMultiline,
         .doc = "Sound files, one per line; each event picks one at random."},
        num("volume", 1.0, "Loudest volume.", 0.0, 4.0),
        num("minVolume", 0.2, "Volume of the weakest event that plays, as a fraction of volume.", 0.0, 1.0),
        num("minInterval", 0.05, "Seconds an object stays quiet after a sound, so a rattle doesn't fire every frame.", 0.0),
        {.name = "seed", .type = PlugType::Int, .defaultValue = int64_t(0), .flags = kCueStored, .doc = "Changes which sound each event picks."},
        {.name = "mute", .type = PlugType::Bool, .defaultValue = false, .flags = kCueStored, .doc = "Silent."},
    };
    t.create = [] { return std::make_unique<SoundCueNode>(); };
    scene::types::registerType(std::move(t));
}

void registerSoundCueCommands()
{
    if (ops::find("soundCue"))
        return;

    ops::registerCommand({
        .name = "soundCue",
        .doc = "Create a sound cue, or edit the one named: sounds fired by simulation events (contact, break) above a "
               "strength, for the targets given. Returns its name.",
        .positionalDoc = "soundCue to edit (omit to create one)",
        .flags = {{"eventType", "et", T::String, nullptr, "any, contact or break.", kEventTypeNames},
                  {"minStrength", "min", T::Float, nullptr, "Weaker events are silent (break: N s; contact: cm/s).", {}, {}, 0.0},
                  {"fullStrength", "full", T::Float, nullptr, "Strength that plays at full volume.", {}, {}, 0.0},
                  {"targets", "tg", T::String, nullptr, "Nodes or object sets, space separated; empty for all."},
                  {"sounds", "snd", T::List, nullptr, "Sound files; one is picked per event."},
                  {"volume", "v", T::Float, nullptr, "Loudest volume.", {}, {}, 0.0, 4.0},
                  {"minVolume", "mv", T::Float, nullptr, "Quietest volume, as a fraction of volume.", {}, {}, 0.0, 1.0},
                  {"minInterval", "mi", T::Float, nullptr, "Seconds an object stays quiet after a sound.", {}, {}, 0.0},
                  {"seed", "sd", T::Int, nullptr, "Changes which sound each event picks."},
                  {"mute", "m", T::Bool, nullptr, "Silence it."},
                  {"name", "n", T::String, nullptr, "Name; made unique."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* n = nullptr;
            if (!a.positional.empty())
            {
                n = g.find(a.positional[0].get<std::string>());
                if (!n || !n->type().isA("soundCue"))
                    throw CommandError("soundCue: " + a.positional[0].get<std::string>() + " is not a sound cue");
            }
            else
                n = g.createNode(*scene::types::find("soundCue"), a.get<std::string>("name", "soundCue"));
            const NodeId id = n->id();
            if (a.has("eventType"))
            {
                const std::string v = a.get<std::string>("eventType");
                auto it = std::ranges::find(kEventTypeNames, v);
                if (it == kEventTypeNames.end())
                    throw CommandError("soundCue: eventType is any, contact or break");
                g.setValue({id, SoundCueNode::kEventType}, int64_t(it - kEventTypeNames.begin()));
            }
            auto setNum = [&](const char* flag, int attr) {
                if (a.has(flag))
                    g.setValue({id, attr}, a.get<double>(flag));
            };
            setNum("minStrength", SoundCueNode::kMinStrength);
            setNum("fullStrength", SoundCueNode::kFullStrength);
            setNum("volume", SoundCueNode::kVolume);
            setNum("minVolume", SoundCueNode::kMinVolume);
            setNum("minInterval", SoundCueNode::kMinInterval);
            if (a.has("targets"))
                g.setValue({id, SoundCueNode::kTargets}, a.get<std::string>("targets"));
            if (a.has("sounds"))
            {
                std::vector<std::string> files;
                for (const json& f : a.flags["sounds"])
                    if (f.is_string())
                        files.push_back(paths::storedPath(paths::fromUtf8(f.get<std::string>())));
                g.setValue({id, SoundCueNode::kSounds}, joinLines(files));
            }
            if (a.has("seed"))
                g.setValue({id, SoundCueNode::kSeed}, a.get<int64_t>("seed"));
            if (a.has("mute"))
                g.setValue({id, SoundCueNode::kMute}, a.get<bool>("mute"));
            return g.node(id)->path();
        },
        .examples = {"soundCue(eventType='break', minStrength=50, sounds=['sfx/crack1.wav', 'sfx/crack2.wav'], targets='wall')"},
    });

    ops::registerCommand({
        .name = "soundCueTriggers",
        .doc = "The sounds the scene's cues fire over a frame range, from what the simulations have already cached: time, "
               "frame, sound, gain, cue, event, object, piece, strength and position.",
        .flags = {{"start", "s", T::Float, nullptr, "First frame. Default: the playback range's."},
                  {"end", "e", T::Float, nullptr, "Last frame. Default: the playback range's."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto [t0, t1] = rangeOf(a);
            json out = json::array();
            for (const Trigger& t : sceneTriggers(Graph::current(), t0, t1))
                out.push_back(triggerJson(t));
            return out;
        },
    });

    ops::registerCommand({
        .name = "exportSoundCues",
        .doc = "Write the sounds the cues fire, to play them in Unreal.\n\n"
               "One row per sound by time: Frame, Time (seconds), X, Y, Z (cm, in the axis profile's space), Sound (file name "
               "without extension, to match an imported Sound Wave), File, Volume, Cue, Event, Object, Piece and Strength. A "
               ".csv imports into Unreal as a Data Table; a .json is a list of objects. Reads cached simulation frames only. "
               "Returns {path, sounds}.",
        .flags = {{"path", "p", T::String, nullptr, "The .csv or .json file."},
                  {"start", "s", T::Float, nullptr, "First frame. Default: the playback range's."},
                  {"end", "e", T::Float, nullptr, "Last frame. Default: the playback range's."},
                  {"profile", "ax", T::String, "ue5", "Axis profile of the positions: ue5, ue6 or generic (Lila's own).", {"ue5", "ue6", "generic"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (!a.has("path"))
                throw CommandError("exportSoundCues: path is required");
            auto [t0, t1] = rangeOf(a);
            const std::vector<Trigger> all = sceneTriggers(Graph::current(), t0, t1);
            const std::string profile = a.get<std::string>("profile", "ue5");
            const glm::dmat3 basis = profile == "generic" ? glm::dmat3(1.0) : seq::unrealBasis(profile);
            const std::string file = a.get<std::string>("path");
            const fs::path path = paths::fromUtf8(file);
            std::ofstream out(path, std::ios::binary);
            if (!out)
                throw CommandError("exportSoundCues: can't write " + file);
            const bool csv = path.extension() != ".json";
            json rows = json::array();
            if (csv)
                out << "---,Frame,Time,X,Y,Z,Sound,File,Volume,Cue,Event,Object,Piece,Strength\n";
            for (size_t k = 0; k < all.size(); ++k)
            {
                const Trigger& t = all[k];
                const glm::dvec3 p = basis * t.event.position;
                const std::string sound = paths::toUtf8(paths::fromUtf8(t.sound).stem());
                const double frame = scene::anim::frameOf(t.time);
                const char* event = t.event.type == EventType::Break ? "Break" : "Contact";
                if (csv)
                    out << std::format("Sound_{:04},{},{:.4f},{:.3f},{:.3f},{:.3f},{},\"{}\",{:.4f},{},{},{},{},{:.3f}\n", k, std::llround(frame), t.time,
                                       p.x, p.y, p.z, sound, t.sound, t.gain, shortName(t.cue), event, shortName(t.event.object), t.event.piece,
                                       t.event.strength);
                else
                    rows.push_back({{"frame", std::llround(frame)}, {"time", t.time}, {"x", p.x}, {"y", p.y}, {"z", p.z}, {"sound", sound},
                                    {"file", t.sound}, {"volume", t.gain}, {"cue", shortName(t.cue)}, {"event", event},
                                    {"object", shortName(t.event.object)}, {"piece", t.event.piece}, {"strength", t.event.strength}});
            }
            if (!csv)
                out << rows.dump(2);
            return {{"path", file}, {"sounds", all.size()}};
        },
    });
}
} // namespace lila::media
