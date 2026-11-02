#pragma once

#include "core/Export.h"

#include "media/Media.h"

#include <memory>
#include <optional>
#include <vector>

namespace lila::media
{
/**
 * Sound output: the timeline's voices during playback and scrubbing, plus one-shot cue sounds, through the default
 * output device (miniaudio: WASAPI on Windows, Core Audio on macOS, PulseAudio/ALSA on Linux).
 *
 * While playing, the samples the device has taken are the clock: clock() is the scene time being heard, which the
 * time slider follows so picture and sound stay locked. Main thread, except the device's own callback.
 */
class LILA_SCENE_API AudioEngine
{
public:
    static AudioEngine& get();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /** Opens the default output on first use; false when there is none, after which nothing plays and clock() is empty. */
    bool ensureDevice();
    bool hasDevice() const;
    /** Closes the device; for shutdown. */
    void close();

    void setVoices(std::vector<Voice> voices);
    /** Plays from sceneTime, speed scene seconds per second (negative plays backwards). */
    void play(double sceneTime, double speed = 1.0);
    void stop();
    bool playing() const;
    /** Plays [sceneTime, sceneTime + seconds) once at normal speed: the sound under the playhead while scrubbing. */
    void scrub(double sceneTime, double seconds);
    /** A sound now, once, over whatever else plays. */
    void fire(AudioBufferPtr buffer, double gain);

    /** The scene time being heard now while playing with a device; nullopt otherwise. */
    std::optional<double> clock() const;

    /** Renders frames of output (interleaved stereo) without a device, advancing as the device would; for tests. */
    void render(float* out, int frames);
    /** Output frames rendered so far. */
    int64_t renderedFrames() const;

    struct Impl;

private:
    AudioEngine();
    std::unique_ptr<Impl> mImpl;
};
} // namespace lila::media
