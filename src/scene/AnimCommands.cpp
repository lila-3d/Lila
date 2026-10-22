#include "scene/Anim.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>

namespace lila::scene::anim
{
namespace
{
constexpr double kTimeEps = 1e-6;
constexpr double kInf = std::numeric_limits<double>::infinity();

Graph& graph() { return Graph::current(); }

struct State
{
    Playback playback;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
    PlaybackDriver driver;
    int direction = 0;
    std::optional<bool> autoKey;
};

State& state()
{
    static State s;
    return s;
}

/** Clipboard of copyKey; times relative to the earliest copied key. */
struct ClipChannel
{
    std::string attr;
    std::vector<Key> keys;
};
std::vector<ClipChannel>& clipboard()
{
    static std::vector<ClipChannel> c;
    return c;
}

template <class F> auto guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}

bool stepped(const AttrDef& a)
{
    return a.type == PlugType::Bool || a.type == PlugType::Enum;
}

const char* curveTypeFor(const AttrDef& a)
{
    if (a.type == PlugType::Vec3 || a.type == PlugType::Float)
    {
        const std::string& n = a.name;
        if (n.starts_with("rotate") && n.find("Pivot") == std::string::npos)
            return "animCurveTA";
        if (n.find("translate") != std::string::npos || n.find("Translate") != std::string::npos)
            return "animCurveTL";
    }
    return "animCurveTU";
}

/** "translateX" for a component plug, else the attribute name. */
std::string channelName(Graph& g, const PlugRef& p)
{
    const AttrDef& a = g.attr(p);
    return p.component >= 0 ? a.childNames[p.component] : a.name;
}

double currentValue(Graph& g, const PlugRef& p)
{
    g.evaluate();
    const PlugValue& v = g.storedValue(p);
    if (p.component >= 0)
        return std::get<glm::dvec3>(v)[p.component];
    return std::visit(
        [](const auto& x) -> double {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_arithmetic_v<T>)
                return double(x);
            else
                return 0.0;
        },
        v);
}

struct Target
{
    PlugRef plug; // invalid when a curve node was named directly
    NodeId curve = kNoNode;
};

std::vector<std::string> attrList(const Args& a)
{
    std::vector<std::string> out;
    if (!a.has("attribute"))
        return out;
    const json& v = a.flags["attribute"];
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& x : v)
            if (x.is_string())
                out.push_back(x.get<std::string>());
    return out;
}

/** Objects or plugs from the args (default the selection), expanded to scalar channels. */
std::vector<Target> targets(Graph& g, const Args& a, bool animatedOnly)
{
    std::vector<Target> out;
    auto add = [&](const PlugRef& p) {
        NodeId c = curveFor(g, p);
        if (animatedOnly && !c)
            return;
        for (const Target& t : out)
            if (t.plug == p)
                return;
        out.push_back({p, c});
    };
    auto expand = [&](const PlugRef& p) {
        if (p.component < 0 && g.attr(p).type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                add({p.node, p.attr, c});
        else
            add(p);
    };
    std::vector<std::string> attrs = attrList(a);
    for (const std::string& obj : a.objects())
    {
        if (obj.find('.') != std::string::npos)
        {
            expand(guarded([&] { return g.plug(obj); }));
            continue;
        }
        Node* n = g.find(obj);
        if (!n)
            throw CommandError("no node named '" + obj + "'");
        if (isCurve(*n))
        {
            out.push_back({{}, n->id()});
            continue;
        }
        if (!attrs.empty())
        {
            for (const std::string& at : attrs)
                expand(guarded([&] { return g.plug(obj + "." + at); }));
            continue;
        }
        if (animatedOnly)
            for (const AnimatedPlug& ap : animatedPlugs(g, n->id()))
                add(ap.plug);
        else
            for (const PlugRef& p : keyablePlugs(g, n->id()))
                add(p);
    }
    return out;
}

/** time=t or time=(start, end), None for an open end. Inclusive, with a little slack for float frames. */
std::pair<double, double> timeRange(const Args& a)
{
    if (!a.has("time"))
        return {-kInf, kInf};
    const json& t = a.flags["time"];
    auto num = [](const json& v, double fallback) { return v.is_number() ? v.get<double>() : fallback; };
    if (t.is_number())
        return {t.get<double>() - kTimeEps, t.get<double>() + kTimeEps};
    if (t.is_array() && t.size() == 1)
        return {num(t[0], -kInf) - kTimeEps, num(t[0], kInf) + kTimeEps};
    if (t.is_array() && t.size() == 2)
        return {num(t[0], -kInf) - kTimeEps, num(t[1], kInf) + kTimeEps};
    throw CommandError("time must be a number or (start, end)");
}

Tangent tangentArg(const Args& a, const char* flag)
{
    return guarded([&] { return tangentFromName(a.get<std::string>(flag)); });
}

double degreesOf(double slope)
{
    return std::atan(slope) * 180.0 / 3.14159265358979323846;
}

int copyKeys(Graph& g, const Args& a)
{
    LILA_ZONE();

    auto [t0, t1] = timeRange(a);
    std::vector<ClipChannel> clip;
    double start = kInf;
    int count = 0;
    for (const Target& t : targets(g, a, true))
    {
        ClipChannel ch{t.plug.valid() ? channelName(g, t.plug) : g.node(t.curve)->name(), {}};
        for (const Key& k : curveOf(*g.node(t.curve)).keys)
            if (k.time >= t0 && k.time <= t1)
            {
                ch.keys.push_back(k);
                start = std::min(start, k.time);
            }
        count += int(ch.keys.size());
        if (!ch.keys.empty())
            clip.push_back(std::move(ch));
    }
    if (clip.empty())
        return 0;
    for (ClipChannel& ch : clip)
        for (Key& k : ch.keys)
            k.time -= start;
    clipboard() = std::move(clip);
    return count;
}

void notify()
{
    auto listeners = state().listeners;
    for (auto& [id, fn] : listeners)
        fn();
}
} // namespace

json Playback::toJson() const
{
    return {{"minTime", minTime}, {"maxTime", maxTime}, {"animationStartTime", animStart}, {"animationEndTime", animEnd},
            {"fps", fps}, {"loop", loop}, {"playbackSpeed", std::string(speedName(speed))}, {"everyFrame", playsEveryFrame(speed)},
            {"speedMultiplier", customSpeed}};
}

void Playback::fromJson(const json& j)
{
    if (!j.is_object())
        return;
    minTime = j.value("minTime", minTime);
    maxTime = j.value("maxTime", maxTime);
    animStart = j.value("animationStartTime", animStart);
    animEnd = j.value("animationEndTime", animEnd);
    fps = std::max(1.0, j.value("fps", fps));
    loop = j.value("loop", loop);
    maxTime = std::max(maxTime, minTime);
    animStart = std::min(animStart, minTime);
    animEnd = std::max(animEnd, maxTime);
}

Playback& playback()
{
    return state().playback;
}

void playbackChanged()
{
    notify();
}

int addPlaybackListener(std::function<void()> fn)
{
    State& s = state();
    s.listeners.emplace(s.nextListener, std::move(fn));
    return s.nextListener++;
}

void removePlaybackListener(int id)
{
    state().listeners.erase(id);
}

double frameOf(double seconds)
{
    return seconds * playback().fps;
}

double secondsOf(double frame)
{
    return frame / playback().fps;
}

double snapToFrame(double seconds)
{
    return secondsOf(std::round(frameOf(seconds)));
}

void setPlaybackDriver(PlaybackDriver driver)
{
    state().driver = std::move(driver);
}

void play(int direction)
{
    State& s = state();
    s.direction = direction >= 0 ? 1 : -1;
    if (s.driver.start)
        s.driver.start(s.direction);
    notify();
}

void stop()
{
    State& s = state();
    if (s.direction == 0)
        return;
    s.direction = 0;
    if (s.driver.stop)
        s.driver.stop();
    notify();
}

int playDirection()
{
    return state().direction;
}

bool autoKey()
{
    State& s = state();
    if (!s.autoKey)
        s.autoKey = PrefStore::get().value<bool>("anim", "/autoKey", false);
    return *s.autoKey;
}

void setAutoKey(bool on)
{
    state().autoKey = on;
    PrefStore::get().set("anim", "/autoKey", on);
    notify();
}

NodeId curveFor(Graph& g, const PlugRef& plug)
{
    auto src = g.source(plug);
    if (!src)
        return kNoNode;
    Node* n = g.node(src->node);
    return n && isCurve(*n) ? n->id() : kNoNode;
}

std::vector<PlugRef> keyablePlugs(Graph& g, NodeId id)
{
    std::vector<PlugRef> out;
    Node* n = g.node(id);
    if (!n)
        return out;
    const auto& attrs = n->type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
    {
        const AttrDef& a = attrs[i];
        if (!(a.flags & AttrKeyable) || (a.flags & AttrHidden) || !(a.flags & AttrInput))
            continue;
        if (a.type == PlugType::Vec3)
            for (int c = 0; c < 3; ++c)
                out.push_back({id, i, c});
        else if (a.type == PlugType::Float || a.type == PlugType::Int || a.type == PlugType::Bool || a.type == PlugType::Enum)
            out.push_back({id, i, -1});
    }
    return out;
}

std::vector<AnimatedPlug> animatedPlugs(Graph& g, NodeId id)
{
    std::vector<AnimatedPlug> out;
    Node* n = g.node(id);
    if (!n)
        return out;
    for (int i = 0; i < int(n->type().attrs.size()); ++i)
        for (int c = -1; c < 3; ++c)
        {
            if (c >= 0 && n->type().attrs[i].type != PlugType::Vec3)
                break;
            PlugRef p{id, i, c};
            if (NodeId curve = curveFor(g, p))
                out.push_back({p, curve});
        }
    return out;
}

bool hasKeyAt(const Node& curve, double t)
{
    try
    {
        return curveOf(curve).find(t) >= 0;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::vector<double> keyTimes(Graph& g, const std::vector<NodeId>& nodes)
{
    LILA_ZONE();

    std::vector<double> out;
    for (NodeId id : nodes)
    {
        Node* n = g.node(id);
        if (!n)
            continue;
        std::vector<NodeId> curves;
        if (isCurve(*n))
            curves.push_back(id);
        for (const AnimatedPlug& ap : animatedPlugs(g, id))
            curves.push_back(ap.curve);
        for (NodeId c : curves)
            for (const Key& k : curveOf(*g.node(c)).keys)
                out.push_back(k.time);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::abs(a - b) <= kTimeEps; }), out.end());
    return out;
}

NodeId setKey(Graph& g, const PlugRef& plug, double time, double value, std::optional<Tangent> in, std::optional<Tangent> out)
{
    LILA_ZONE();

    const AttrDef& a = g.attr(plug);
    if (plug.component < 0 && a.type == PlugType::Vec3)
        throw std::invalid_argument("key the components of " + g.plugName(plug));
    if (!(a.flags & AttrInput))
        throw std::invalid_argument(g.plugName(plug) + " is an output");

    NodeId curve = curveFor(g, plug);
    Curve cv;
    if (!curve)
    {
        if (g.source(plug) || (plug.component >= 0 && g.source({plug.node, plug.attr, -1})))
            throw std::invalid_argument(g.plugName(plug) + " is driven by a connection");
        std::string name = g.node(plug.node)->name() + "_" + channelName(g, plug);
        std::replace(name.begin(), name.end(), ':', '_');
        curve = g.createNode(*types::find(curveTypeFor(a)), name, kNoNode, false)->id();
    }
    else
        cv = curveOf(*g.node(curve));

    bool fresh = cv.find(time) < 0;
    size_t i = cv.insert({.time = time, .value = value});
    if (fresh && stepped(a))
        cv.keys[i].out = Tangent::Step;
    if (in)
        cv.keys[i].in = *in;
    if (out)
        cv.keys[i].out = *out;
    setCurve(g, curve, cv);
    if (!g.source(plug))
        g.connect({curve, CurveNode::kOutput, -1}, plug);
    return curve;
}

int removeKeys(Graph& g, NodeId curve, double t0, double t1)
{
    LILA_ZONE();

    Curve cv = curveOf(*g.node(curve));
    size_t before = cv.keys.size();
    std::erase_if(cv.keys, [&](const Key& k) { return k.time >= t0 && k.time <= t1; });
    int removed = int(before - cv.keys.size());
    if (removed == 0)
        return 0;
    if (cv.keys.empty())
        g.deleteNode(curve);
    else
        setCurve(g, curve, cv);
    return removed;
}

void registerCommands()
{
    using T = ArgType;
    registerTypes();

    {
        State& s = state();
        s.playback.fps = std::max(1.0, PrefStore::get().value<double>("anim", "/fps", 30.0));
        s.playback.customSpeed = std::clamp(PrefStore::get().value<double>("anim", "/playbackSpeedMultiplier", 1.0), kMinCustomSpeed,
                                            kMaxCustomSpeed);
        std::string speed = PrefStore::get().value<std::string>("anim", "/playbackSpeed", "");
        if (auto named = speedFromName(speed))
            s.playback.speed = *named;
        else if (PrefStore::get().value<bool>("anim", "/everyFrame", false))
            s.playback.speed = PlaybackSpeed::EveryFrameMaxRealTime;
    }

    graph().setDrivenSetHook([](Graph& g, const PlugRef& dst, const PlugRef& src, double v) {
        Node* s = g.node(src.node);
        if (!s || !isCurve(*s))
            return Graph::DrivenEdit::Reject;
        // Interactive drags run with undo off; they hold, and the committing set keys.
        if (autoKey() && UndoStack::get().enabled())
        {
            setKey(g, dst, g.time(), v);
            return Graph::DrivenEdit::Handled;
        }
        return Graph::DrivenEdit::Hold;
    });

    const FlagDef attrFlag{"attribute", "at", T::Any, nullptr, "Attribute name or list of names (translate keys all three)."};
    const FlagDef timeFlag{"time", "t", T::Any, nullptr, "Seconds, or (start, end) with None for an open end. Default: all keys."};

    cmds::registerCommand({
        .name = "setKeyframe",
        .doc = "Key channels at a time. With no objects, keys every keyable channel of the selection at the current "
               "time (Maya's S). Objects may be nodes, plugs ('cube.ty') or curve nodes. Returns the number of keys set.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"time", "t", T::Float, nullptr, "Seconds. Default: the current time."},
            {"value", "v", T::Float, nullptr, "Value. Default: the channel's current value."},
            {"inTangentType", "itt", T::String, nullptr, "auto | spline | linear | flat | step | clamped"},
            {"outTangentType", "ott", T::String, nullptr, "auto | spline | linear | flat | step | clamped"},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            double time = a.get<double>("time", g.time());
            std::optional<Tangent> in, out;
            if (a.has("inTangentType"))
                in = tangentArg(a, "inTangentType");
            if (a.has("outTangentType"))
                out = tangentArg(a, "outTangentType");
            int count = 0;
            for (const Target& t : targets(g, a, false))
            {
                if (!t.plug.valid())
                {
                    Curve cv = curveOf(*g.node(t.curve));
                    double v = a.has("value") ? a.get<double>("value") : cv.evaluate(time);
                    size_t i = cv.insert({.time = time, .value = v});
                    if (in)
                        cv.keys[i].in = *in;
                    if (out)
                        cv.keys[i].out = *out;
                    setCurve(g, t.curve, cv);
                    ++count;
                    continue;
                }
                double v;
                if (a.has("value"))
                    v = a.get<double>("value");
                else if (t.curve && std::abs(time - g.time()) > kTimeEps)
                    v = curveOf(*g.node(t.curve)).evaluate(time);
                else
                    v = currentValue(g, t.plug);
                guarded([&] { return setKey(g, t.plug, time, v, in, out); });
                ++count;
            }
            return count;
        },
    });

    cmds::registerCommand({
        .name = "cutKey",
        .doc = "Remove keys (all, or those in time) and copy them for pasteKey. A curve left with no keys is deleted "
               "and the channel keeps its value. Returns the number removed.",
        .positionalDoc = "objects",
        .flags = {attrFlag, timeFlag, {"clear", "cl", T::Bool, false, "Don't copy to the clipboard."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto [t0, t1] = timeRange(a);
            std::vector<Target> ts = targets(g, a, true);
            if (!a.get<bool>("clear"))
                copyKeys(g, a);
            int removed = 0;
            for (const Target& t : ts)
                removed += removeKeys(g, t.curve, t0, t1);
            return removed;
        },
    });

    cmds::registerCommand({
        .name = "copyKey",
        .doc = "Copy keys (all, or those in time) to the key clipboard. Returns the number copied.",
        .positionalDoc = "objects",
        .flags = {attrFlag, timeFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            return copyKeys(graph(), a);
        },
    });

    cmds::registerCommand({
        .name = "pasteKey",
        .doc = "Paste copied keys starting at time (default the current time). One copied channel pastes onto every "
               "target; several match by channel name. option: merge (replace keys at the same times) or replace "
               "(clear the pasted span first). Returns the number pasted.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"time", "t", T::Float, nullptr, "Seconds. Default: the current time."},
            {"option", "o", T::String, "merge", "merge | replace"},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const auto& clip = clipboard();
            if (clip.empty())
                throw CommandError("pasteKey: nothing copied");
            double at = a.get<double>("time", g.time());
            std::string option = a.get<std::string>("option", "merge");
            if (option != "merge" && option != "replace")
                throw CommandError("pasteKey: option is merge or replace");
            int count = 0;
            for (const Target& t : targets(g, a, false))
            {
                const ClipChannel* ch = nullptr;
                std::string name = t.plug.valid() ? channelName(g, t.plug) : g.node(t.curve)->name();
                for (const ClipChannel& c : clip)
                    if (clip.size() == 1 || c.attr == name)
                        ch = &c;
                if (!ch)
                    continue;
                NodeId curve = t.curve;
                if (!curve)
                    curve = guarded([&] { return setKey(g, t.plug, at + ch->keys[0].time, ch->keys[0].value); });
                Curve cv = curveOf(*g.node(curve));
                double span = ch->keys.back().time;
                if (option == "replace")
                    std::erase_if(cv.keys, [&](const Key& k) { return k.time >= at - kTimeEps && k.time <= at + span + kTimeEps; });
                for (Key k : ch->keys)
                {
                    k.time += at;
                    if (int i = cv.find(k.time); i >= 0)
                        cv.keys[size_t(i)] = k;
                    else
                        cv.insert(k);
                    ++count;
                }
                setCurve(g, curve, cv);
            }
            return count;
        },
    });

    cmds::registerCommand({
        .name = "keyframe",
        .doc = "Query or edit keys. query=True returns key times (valueChange=True: values; keyCount=True: the "
               "count). Without query, moves the keys in time by timeChange and/or sets valueChange; relative=True "
               "offsets instead of setting.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            timeFlag,
            {"query", "q", T::Bool, false, ""},
            {"keyCount", "kc", T::Bool, false, "Query the number of keys."},
            {"valueChange", "vc", T::Any, nullptr, "Value to set (or offset), or True with query."},
            {"timeChange", "tc", T::Any, nullptr, "Time to set (or offset), or True with query."},
            {"relative", "r", T::Bool, false, "Offset instead of set."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto [t0, t1] = timeRange(a);
            std::vector<Target> ts = targets(g, a, true);
            if (a.get<bool>("query"))
            {
                bool values = a.has("valueChange") && a.flags["valueChange"] == true;
                json out = json::array();
                int count = 0;
                for (const Target& t : ts)
                    for (const Key& k : curveOf(*g.node(t.curve)).keys)
                        if (k.time >= t0 && k.time <= t1)
                        {
                            ++count;
                            out.push_back(values ? k.value : k.time);
                        }
                return a.get<bool>("keyCount") ? json(count) : out;
            }
            bool tc = a.has("timeChange"), vc = a.has("valueChange");
            if (!tc && !vc)
                throw CommandError("keyframe: give timeChange and/or valueChange, or query=True");
            if ((tc && !a.flags["timeChange"].is_number()) || (vc && !a.flags["valueChange"].is_number()))
                throw CommandError("keyframe: timeChange and valueChange take numbers");
            bool rel = a.get<bool>("relative");
            int count = 0;
            for (const Target& t : ts)
            {
                Curve cv = curveOf(*g.node(t.curve));
                std::vector<Key> moved, kept;
                for (const Key& k : cv.keys)
                {
                    if (k.time < t0 || k.time > t1)
                    {
                        kept.push_back(k);
                        continue;
                    }
                    Key m = k;
                    if (tc)
                        m.time = rel ? m.time + a.get<double>("timeChange") : a.get<double>("timeChange");
                    if (vc)
                        m.value = rel ? m.value + a.get<double>("valueChange") : a.get<double>("valueChange");
                    moved.push_back(m);
                }
                if (moved.empty())
                    continue;
                count += int(moved.size());
                // Moved keys land on top of whatever was at their new time.
                cv.keys = kept;
                for (const Key& m : moved)
                {
                    if (int i = cv.find(m.time); i >= 0)
                        cv.keys[size_t(i)] = m;
                    else
                        cv.insert(m);
                }
                setCurve(g, t.curve, cv);
            }
            return count;
        },
    });

    cmds::registerCommand({
        .name = "keyTangent",
        .doc = "Query or set tangents of keys (all, or those in time). Setting an angle makes that side fixed. "
               "query=True returns [{time, in, out, inAngle, outAngle, inWeight, outWeight}].",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            timeFlag,
            {"query", "q", T::Bool, false, ""},
            {"inTangentType", "itt", T::String, nullptr, "auto | spline | linear | flat | step | clamped | fixed"},
            {"outTangentType", "ott", T::String, nullptr, "auto | spline | linear | flat | step | clamped | fixed"},
            {"inAngle", "ia", T::Float, nullptr, "Degrees."},
            {"outAngle", "oa", T::Float, nullptr, "Degrees."},
            {"inWeight", "iw", T::Float, nullptr, "Handle length, 1 = a third of the segment."},
            {"outWeight", "ow", T::Float, nullptr, "Handle length, 1 = a third of the segment."},
            {"weightedTangents", "wt", T::Bool, nullptr, "Make the curves weighted or not."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            auto [t0, t1] = timeRange(a);
            std::vector<Target> ts = targets(g, a, true);
            if (a.get<bool>("query"))
            {
                json out = json::array();
                for (const Target& t : ts)
                {
                    Curve cv = curveOf(*g.node(t.curve));
                    for (size_t i = 0; i < cv.keys.size(); ++i)
                    {
                        const Key& k = cv.keys[i];
                        if (k.time < t0 || k.time > t1)
                            continue;
                        out.push_back({{"time", k.time}, {"in", tangentName(k.in)}, {"out", tangentName(k.out)},
                                       {"inAngle", degreesOf(cv.inSlope(i))}, {"outAngle", degreesOf(cv.outSlope(i))},
                                       {"inWeight", k.inWeight}, {"outWeight", k.outWeight}});
                    }
                }
                return out;
            }
            std::optional<Tangent> in, out;
            if (a.has("inTangentType"))
                in = tangentArg(a, "inTangentType");
            if (a.has("outTangentType"))
                out = tangentArg(a, "outTangentType");
            int count = 0;
            for (const Target& t : ts)
            {
                Curve cv = curveOf(*g.node(t.curve));
                Curve before = cv;
                if (a.has("weightedTangents"))
                    cv.weighted = a.get<bool>("weightedTangents");
                for (size_t i = 0; i < cv.keys.size(); ++i)
                {
                    Key& k = cv.keys[i];
                    if (k.time < t0 || k.time > t1)
                        continue;
                    ++count;
                    // Fixing a side freezes the slope it has now.
                    if (in)
                    {
                        if (*in == Tangent::Fixed && k.in != Tangent::Fixed)
                            k.inAngle = degreesOf(before.inSlope(i));
                        k.in = *in;
                    }
                    if (out)
                    {
                        if (*out == Tangent::Fixed && k.out != Tangent::Fixed)
                            k.outAngle = degreesOf(before.outSlope(i));
                        k.out = *out;
                    }
                    if (a.has("inAngle"))
                    {
                        k.in = Tangent::Fixed;
                        k.inAngle = std::clamp(a.get<double>("inAngle"), -89.999, 89.999);
                    }
                    if (a.has("outAngle"))
                    {
                        k.out = Tangent::Fixed;
                        k.outAngle = std::clamp(a.get<double>("outAngle"), -89.999, 89.999);
                    }
                    if (a.has("inWeight"))
                        k.inWeight = std::clamp(a.get<double>("inWeight"), 0.0, 3.0);
                    if (a.has("outWeight"))
                        k.outWeight = std::clamp(a.get<double>("outWeight"), 0.0, 3.0);
                }
                setCurve(g, t.curve, cv);
            }
            return count;
        },
    });

    cmds::registerCommand({
        .name = "setInfinity",
        .doc = "Curve behaviour outside its keys: constant, linear, cycle, cycleRelative (cycle with offset), "
               "oscillate. query=True returns [{curve, pre, post}].",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"preInfinite", "pri", T::String, nullptr, "Before the first key."},
            {"postInfinite", "poi", T::String, nullptr, "After the last key."},
            {"query", "q", T::Bool, false, ""},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            json out = json::array();
            for (const Target& t : targets(g, a, true))
            {
                Curve cv = curveOf(*g.node(t.curve));
                if (a.get<bool>("query"))
                {
                    out.push_back({{"curve", g.node(t.curve)->name()}, {"pre", infinityName(cv.pre)}, {"post", infinityName(cv.post)}});
                    continue;
                }
                if (a.has("preInfinite"))
                    cv.pre = guarded([&] { return infinityFromName(a.get<std::string>("preInfinite")); });
                if (a.has("postInfinite"))
                    cv.post = guarded([&] { return infinityFromName(a.get<std::string>("postInfinite")); });
                setCurve(g, t.curve, cv);
            }
            return a.get<bool>("query") ? out : json(nullptr);
        },
    });

    cmds::registerCommand({
        .name = "findKeyframe",
        .doc = "Time of the next, previous, first or last key on the objects (default the selection), or None. "
               "next and previous wrap around.",
        .positionalDoc = "objects",
        .flags = {
            attrFlag,
            {"which", "w", T::String, "next", "next | previous | first | last"},
            {"time", "t", T::Float, nullptr, "Search from here. Default: the current time."},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            std::vector<double> times;
            for (const Target& t : targets(g, a, true))
                for (const Key& k : curveOf(*g.node(t.curve)).keys)
                    times.push_back(k.time);
            if (times.empty())
                return nullptr;
            std::sort(times.begin(), times.end());
            std::string which = a.get<std::string>("which", "next");
            double now = a.get<double>("time", g.time());
            if (which == "first")
                return times.front();
            if (which == "last")
                return times.back();
            if (which == "next")
            {
                for (double t : times)
                    if (t > now + kTimeEps)
                        return t;
                return times.front();
            }
            if (which == "previous")
            {
                for (auto it = times.rbegin(); it != times.rend(); ++it)
                    if (*it < now - kTimeEps)
                        return *it;
                return times.back();
            }
            throw CommandError("findKeyframe: which is next, previous, first or last");
        },
    });

    cmds::registerCommand({
        .name = "playbackOptions",
        .doc = "Query or set playback: ranges in seconds, fps (default 30), loop, playbackSpeed (realTime, half, twice, "
               "everyFrame, everyFrameMaxRealTime, custom), speedMultiplier for custom; speed is a user pref, not saved in "
               "the scene. Returns the settings.",
        .flags = {
            {"minTime", "min", T::Float, nullptr, "Playback start, seconds."},
            {"maxTime", "max", T::Float, nullptr, "Playback end, seconds."},
            {"animationStartTime", "ast", T::Float, nullptr, "Range slider start, seconds."},
            {"animationEndTime", "aet", T::Float, nullptr, "Range slider end, seconds."},
            {"fps", "fps", T::Float, nullptr, "Frames per second."},
            {"loop", "l", T::Any, nullptr, "True/'continuous' to loop, False/'once' to stop at the end."},
            {"playbackSpeed", "ps", T::String, nullptr,
             "realTime (drops frames to keep pace), half, twice, everyFrame (as fast as it evaluates), everyFrameMaxRealTime or "
             "custom (real time times speedMultiplier)."},
            {"speedMultiplier", "sm", T::Float, nullptr,
             "Real-time multiplier of the custom speed, e.g. 0.25 or 1.5. Selects custom unless playbackSpeed is given."},
            {"everyFrame", "ef", T::Bool, nullptr, "True plays every frame up to fps (everyFrameMaxRealTime), False real time."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            Playback& p = playback();
            if (a.get<bool>("query"))
                return p.toJson();
            json j = p.toJson();
            for (const char* f : {"minTime", "maxTime", "animationStartTime", "animationEndTime", "fps"})
                if (a.has(f))
                    j[f] = a.flags[f];
            if (a.has("loop"))
            {
                const json& l = a.flags["loop"];
                if (l.is_boolean())
                    j["loop"] = l.get<bool>();
                else if (l.is_string() && (l == "continuous" || l == "once"))
                    j["loop"] = l == "continuous";
                else
                    throw CommandError("playbackOptions: loop is True/False or 'continuous'/'once'");
            }
            if (a.has("fps") && a.get<double>("fps") <= 0.0)
                throw CommandError("playbackOptions: fps must be positive");
            // Moving the inner range past the outer one drags the outer along, as the range slider does.
            if (a.has("minTime") && !a.has("animationStartTime"))
                j["animationStartTime"] = std::min(j["animationStartTime"].get<double>(), j["minTime"].get<double>());
            if (a.has("maxTime") && !a.has("animationEndTime"))
                j["animationEndTime"] = std::max(j["animationEndTime"].get<double>(), j["maxTime"].get<double>());
            std::optional<PlaybackSpeed> speed;
            if (a.has("playbackSpeed"))
            {
                speed = speedFromName(a.get<std::string>("playbackSpeed"));
                if (!speed)
                    throw CommandError(
                        "playbackOptions: playbackSpeed is realTime, half, twice, everyFrame, everyFrameMaxRealTime or custom");
            }
            else if (a.has("speedMultiplier"))
                speed = PlaybackSpeed::Custom;
            else if (a.has("everyFrame"))
                speed = a.get<bool>("everyFrame") ? PlaybackSpeed::EveryFrameMaxRealTime : PlaybackSpeed::RealTime;
            std::optional<double> multiplier;
            if (a.has("speedMultiplier"))
            {
                multiplier = a.get<double>("speedMultiplier");
                if (!(*multiplier >= kMinCustomSpeed && *multiplier <= kMaxCustomSpeed))
                    throw CommandError(std::format("playbackOptions: speedMultiplier is between {} and {}", kMinCustomSpeed, kMaxCustomSpeed));
            }
            p.fromJson(j);
            if (multiplier && *multiplier != p.customSpeed)
            {
                p.customSpeed = *multiplier;
                PrefStore::get().set("anim", "/playbackSpeedMultiplier", *multiplier);
            }
            if (speed && *speed != p.speed)
            {
                p.speed = *speed;
                PrefStore::get().set("anim", "/playbackSpeed", std::string(speedName(*speed)));
            }
            playbackChanged();
            return p.toJson();
        },
    });

    cmds::registerCommand({
        .name = "play",
        .doc = "Start playback (forward=False plays backwards), or stop it with state=False. query=True returns "
               "whether it is playing.",
        .flags = {
            {"forward", "f", T::Bool, true, ""},
            {"state", "st", T::Bool, true, "False stops."},
            {"query", "q", T::Bool, false, ""},
        },
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("query"))
                return playDirection() != 0;
            if (!a.get<bool>("state", true))
                stop();
            else
                play(a.get<bool>("forward", true) ? 1 : -1);
            return playDirection() != 0;
        },
    });

    cmds::registerCommand({
        .name = "stop",
        .doc = "Stop playback.",
        .undoable = false,
        .run = [](const Args&) -> json {
            stop();
            return nullptr;
        },
    });

    cmds::registerCommand({
        .name = "autoKeyframe",
        .doc = "Auto key: when on, changing an animated channel keys it at the current time. With no flags, queries.",
        .flags = {{"state", "st", T::Bool, nullptr, "On or off."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("state"))
                setAutoKey(a.get<bool>("state"));
            return autoKey();
        },
    });
}
} // namespace lila::scene::anim
