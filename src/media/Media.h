#pragma once

#include "core/Export.h"

#include "media/Decode.h"
#include "scene/Node.h"

#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Sound and video on the timeline, Maya style: audio nodes play under the time slider, image planes play video, and
 * both can belong to takes so each clip brings its own reference sound and footage.
 *
 * A media node bound to takes (connected to a take's `media` array) is live only while one of them is active, and its
 * start frame counts from that take's start; an unbound one is always live and counts from time 0.
 */
namespace lila::media
{
using scene::Graph;
using scene::NodeId;

/** Maya's audio node: a sound file placed on the timeline. */
class AudioNode : public scene::Node
{
public:
    enum Attr
    {
        kFileName,
        kOffset,
        kGain,
        kMute,
        kMedia,
        kCount
    };
};

LILA_SCENE_API bool isAudio(const scene::Node& n);
/** Audio nodes and image planes: the nodes a take can carry. */
LILA_SCENE_API bool isMedia(const scene::Node& n);

/** Takes the media node is bound to, in take order. */
LILA_SCENE_API std::vector<NodeId> takesOf(Graph& g, NodeId media);
/** Media nodes bound to the take. */
LILA_SCENE_API std::vector<NodeId> mediaOf(Graph& g, NodeId take);
/** Undoable. Throws std::invalid_argument for a node that isn't media or a take. */
LILA_SCENE_API void bind(Graph& g, NodeId take, NodeId media);
LILA_SCENE_API void unbind(Graph& g, NodeId take, NodeId media);
/** Unbound media always; bound media while one of its takes is active. */
LILA_SCENE_API bool isLive(Graph& g, NodeId media);
/** Scene seconds where the media's own time 0 falls: startFrame frames after its active take's start (or time 0). */
LILA_SCENE_API double startSeconds(Graph& g, NodeId media, double startFrame);

/** Scene data key of the time slider's sound (an audio node's path); absent plays every live audio node. */
inline constexpr const char* kSoundKey = "timeSlider.sound";
/** The audio nodes heard now: the time slider's sound if it names one, else every live, unmuted audio node. */
LILA_SCENE_API std::vector<NodeId> audibleSounds(Graph& g);

/** A sound placed on the timeline: the buffer's sample 0 plays at `start` scene seconds. */
struct Voice
{
    AudioBufferPtr buffer;
    double start = 0.0;
    double gain = 1.0;
    std::string node;
};

/**
 * A sound file decoded at the output rate, by stored path (resolved from the workspace). Without wait it decodes on a
 * worker and returns null until done, then tells the sound listeners on the main thread. Reloads a changed file. Main
 * thread.
 */
LILA_SCENE_API AudioBufferPtr loadSound(const std::string& stored, bool wait = true, std::string* err = nullptr);
LILA_SCENE_API int addSoundListener(std::function<void()> fn);
LILA_SCENE_API void removeSoundListener(int id);
/** The output sample rate sounds are decoded at. */
inline constexpr int kSampleRate = 48000;
/** The voices of audibleSounds whose files have loaded (wait: load them first). */
LILA_SCENE_API std::vector<Voice> voices(Graph& g, bool wait = false);

/**
 * Mixes voices into out (interleaved stereo at rate) from scene time t0, one output frame advancing speed / rate
 * seconds of scene time. out is overwritten.
 */
LILA_SCENE_API void mix(const std::vector<Voice>& voices, double t0, double speed, int rate, float* out, int frames);

/** Which frame of a video an image plane shows at a scene time: start and fps override as on the plane. */
struct VideoTiming
{
    double start = 0.0;    // scene seconds of the video's first frame
    double fps = 0.0;      // the plane's override; 0 uses the file's
    int64_t offset = 0;    // frames added
};
LILA_SCENE_API int64_t videoFrame(const VideoTiming& timing, double fileFps, double sceneSeconds);

LILA_SCENE_API void registerMediaTypes();
LILA_SCENE_API void registerMediaCommands();
} // namespace lila::media
