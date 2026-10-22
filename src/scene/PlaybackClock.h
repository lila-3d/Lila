#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lila::scene::anim
{
/** Maya's Preferences > Time Slider > Playback speed. */
enum class PlaybackSpeed : uint8_t
{
    RealTime,
    Half,
    Twice,
    /** Every frame, as fast as the scene evaluates. */
    EveryFrame,
    /** Every frame, never faster than fps. */
    EveryFrameMaxRealTime,
    /** Real time scaled by the user's own multiplier (Playback::customSpeed). */
    Custom,
};

constexpr std::string_view speedName(PlaybackSpeed s)
{
    switch (s)
    {
    case PlaybackSpeed::RealTime: return "realTime";
    case PlaybackSpeed::Half: return "half";
    case PlaybackSpeed::Twice: return "twice";
    case PlaybackSpeed::EveryFrame: return "everyFrame";
    case PlaybackSpeed::EveryFrameMaxRealTime: return "everyFrameMaxRealTime";
    case PlaybackSpeed::Custom: return "custom";
    }
    return "realTime";
}

constexpr PlaybackSpeed kPlaybackSpeeds[] = {PlaybackSpeed::EveryFrame, PlaybackSpeed::RealTime, PlaybackSpeed::EveryFrameMaxRealTime,
                                              PlaybackSpeed::Half, PlaybackSpeed::Twice, PlaybackSpeed::Custom};

/** Bounds of the custom multiplier. */
constexpr double kMinCustomSpeed = 0.01;
constexpr double kMaxCustomSpeed = 100.0;

/** Menu text, as Maya words it. */
constexpr std::string_view speedLabel(PlaybackSpeed s)
{
    switch (s)
    {
    case PlaybackSpeed::RealTime: return "Real-time";
    case PlaybackSpeed::Half: return "Half Speed";
    case PlaybackSpeed::Twice: return "Twice Speed";
    case PlaybackSpeed::EveryFrame: return "Play Every Frame";
    case PlaybackSpeed::EveryFrameMaxRealTime: return "Play Every Frame, Max Real-time";
    case PlaybackSpeed::Custom: return "Custom Speed";
    }
    return "Real-time";
}

inline std::optional<PlaybackSpeed> speedFromName(std::string_view name)
{
    for (PlaybackSpeed s : kPlaybackSpeeds)
        if (speedName(s) == name)
            return s;
    return std::nullopt;
}

constexpr bool playsEveryFrame(PlaybackSpeed s)
{
    return s == PlaybackSpeed::EveryFrame || s == PlaybackSpeed::EveryFrameMaxRealTime;
}

/** Real-time multiplier of the frame-dropping speeds; custom is the user's multiplier. */
constexpr double speedScale(PlaybackSpeed s, double custom = 1.0)
{
    return s == PlaybackSpeed::Half ? 0.5 : s == PlaybackSpeed::Twice ? 2.0 : s == PlaybackSpeed::Custom ? custom : 1.0;
}

/**
 * Which frame playback shows as wall-clock time passes. Times are seconds of wall clock; frames are whole scene
 * frames. The caller handles the range ends and calls reset when it wraps.
 */
struct PlaybackClock
{
    int64_t frame = 0;
    /** Frame-dropping speeds: the frame shown at originSeconds. */
    int64_t originFrame = 0;
    /** Frame-dropping speeds: when originFrame was shown. Max real time: when the current frame became due. */
    double originSeconds = 0.0;

    void reset(int64_t at, double now)
    {
        frame = originFrame = at;
        originSeconds = now;
    }

    /** Restarts from the frame on screen when something else (a scrub, a script) moved the time. */
    void resync(int64_t shown, double now)
    {
        if (shown != frame)
            reset(shown, now);
    }

    /** Moves to the frame due at now; returns whether it changed. custom is the PlaybackSpeed::Custom multiplier. */
    bool advance(PlaybackSpeed speed, double fps, int direction, double now, double custom = 1.0)
    {
        if (speed == PlaybackSpeed::EveryFrame)
        {
            frame += direction;
            return true;
        }
        if (speed == PlaybackSpeed::EveryFrameMaxRealTime)
        {
            double period = 1.0 / fps;
            if (now - originSeconds < period - 1e-9)
                return false;
            frame += direction;
            originSeconds += period;
            // A slow frame pushes the schedule back instead of bursting to catch up.
            if (now - originSeconds >= period)
                originSeconds = now;
            return true;
        }
        double f = double(originFrame) + direction * (now - originSeconds) * fps * speedScale(speed, custom);
        int64_t next = direction > 0 ? int64_t(std::floor(f + 1e-6)) : int64_t(std::ceil(f - 1e-6));
        if (next == frame)
            return false;
        frame = next;
        return true;
    }
};
} // namespace lila::scene::anim
