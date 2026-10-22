#pragma once

#include "scene/AnimCurve.h"
#include "scene/PlaybackClock.h"

#include <functional>
#include <optional>

namespace lila::scene
{
class Graph;
}

namespace lila::scene::anim
{
/** Scene playback settings. Times are seconds; the UI shows frames at fps. */
struct Playback
{
    double minTime = 0.0; // playback range
    double maxTime = 4.0;
    double animStart = 0.0; // outer range of the range slider
    double animEnd = 4.0;
    double fps = 30.0;
    bool loop = true;
    /** A user pref rather than scene data, so fromJson leaves it alone. */
    PlaybackSpeed speed = PlaybackSpeed::RealTime;
    /** Real-time multiplier of PlaybackSpeed::Custom; also a user pref. */
    double customSpeed = 1.0;

    json toJson() const;
    void fromJson(const json& j);
};

Playback& playback();
/** Notifies listeners after playback settings, the play state or auto key change. */
void playbackChanged();
int addPlaybackListener(std::function<void()> fn);
void removePlaybackListener(int id);

double frameOf(double seconds);
double secondsOf(double frame);
/** Nearest whole frame, in seconds. */
double snapToFrame(double seconds);

/** Runs playback; the UI installs a timer here. Without one, play only flips the state. */
struct PlaybackDriver
{
    std::function<void(int direction)> start;
    std::function<void()> stop;
};
void setPlaybackDriver(PlaybackDriver driver);
void play(int direction);
void stop();
/** 0 stopped, 1 forward, -1 backward. */
int playDirection();

bool autoKey();
void setAutoKey(bool on);

/** Curve node driving plug, or kNoNode. */
NodeId curveFor(Graph& g, const PlugRef& plug);

/** Keyable channels of a node as scalar plugs: Vec3 attrs become three component plugs. */
std::vector<PlugRef> keyablePlugs(Graph& g, NodeId node);

struct AnimatedPlug
{
    PlugRef plug;
    NodeId curve = kNoNode;
};
/** Channels of a node driven by curves, in attribute order. */
std::vector<AnimatedPlug> animatedPlugs(Graph& g, NodeId node);

/** True when the curve node has a key at t. Never throws, even on malformed keys. */
bool hasKeyAt(const Node& curve, double t);

/** Sorted, unique key times of every curve on the nodes. */
std::vector<double> keyTimes(Graph& g, const std::vector<NodeId>& nodes);

/**
 * Keys plug at time, creating and connecting a curve (animCurveTL/TA/TU by the attribute) when it has none.
 * Undoable through the graph. Returns the curve. Throws std::invalid_argument when something else drives the plug.
 */
NodeId setKey(Graph& g, const PlugRef& plug, double time, double value, std::optional<Tangent> in = {},
              std::optional<Tangent> out = {});

/** Removes keys in [t0, t1]; deletes the curve when none are left. Returns how many went. */
int removeKeys(Graph& g, NodeId curve, double t0, double t1);

void registerCommands();
} // namespace lila::scene::anim
