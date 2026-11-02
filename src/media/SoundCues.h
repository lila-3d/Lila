#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Node.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Sound cues: sounds fired by simulation events (pieces hitting, links breaking), picked at random from a cue's files
 * and as loud as the hit was hard. Events come from sources simulations register; they read only what is already
 * simulated, so cues preview during cached playback and export for Unreal without re-simulating.
 */
namespace lila::media
{
using scene::Graph;
using scene::NodeId;

enum class EventType : int
{
    Contact = 0,
    Break = 1,
};

/** One thing that happened in a simulation. */
struct SimEvent
{
    EventType type = EventType::Contact;
    double time = 0.0; // scene seconds
    /** The node that owns the bodies (a destruction mesh), by path, and the piece on it; -1 for the node as a whole. */
    std::string object;
    int piece = -1;
    /**
     * How hard: a break's impulse through the link (N s); a contact's speed change on impact (cm/s).
     */
    double strength = 0.0;
    glm::dvec3 position{0.0};
};

/**
 * Where events come from. events returns those with times in (t0, t1], from cached simulation results only: never
 * steps a simulation.
 */
struct EventSource
{
    std::string id;
    std::function<std::vector<SimEvent>(Graph& g, double t0, double t1)> events;
};
LILA_SCENE_API Registry<EventSource>& eventSources();
/** Every source's events in (t0, t1], by time. */
LILA_SCENE_API std::vector<SimEvent> collectEvents(Graph& g, double t0, double t1);

/** The soundCue node's settings. */
struct Cue
{
    std::string name;
    /** Bit per EventType; 0 matches none. */
    int types = 0b11;
    /** Events weaker than this are silent; at fullStrength and above the cue plays at volume. */
    double minStrength = 0.0;
    double fullStrength = 1000.0;
    /** Node names (or object set names, expanded by the caller) the event's object must be one of; empty for any. */
    std::vector<std::string> objects;
    std::vector<std::string> sounds;
    double volume = 1.0;
    /** The quietest a matching event plays, as a fraction of volume. */
    double minVolume = 0.2;
    /** Events closer than this to the cue's previous trigger on the same object are dropped (no machine-gun repeats). */
    double minInterval = 0.05;
    int64_t seed = 0;
};

struct Trigger
{
    double time = 0.0;
    std::string sound; // as stored on the cue
    double gain = 1.0;
    std::string cue;
    SimEvent event;
};

LILA_SCENE_API bool matches(const Cue& cue, const SimEvent& e);
/** The cue's triggers for events (sorted by time): filtered, thinned by minInterval, a sound picked per event by seed. */
LILA_SCENE_API std::vector<Trigger> triggers(const Cue& cue, const std::vector<SimEvent>& events);

class SoundCueNode : public scene::Node
{
public:
    enum Attr
    {
        kEventType,
        kMinStrength,
        kFullStrength,
        kObjects,
        kSounds,
        kVolume,
        kMinVolume,
        kMinInterval,
        kSeed,
        kMute,
        kCount
    };
};

/** A soundCue node's settings, its object sets expanded to their member nodes. */
LILA_SCENE_API Cue cueOf(Graph& g, NodeId cue);
/** The triggers of every unmuted cue for the events in (t0, t1]. */
LILA_SCENE_API std::vector<Trigger> sceneTriggers(Graph& g, double t0, double t1);

LILA_SCENE_API void registerSoundCueTypes();
LILA_SCENE_API void registerSoundCueCommands();
} // namespace lila::media
