#pragma once

#include "core/Export.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/**
 * Video and audio decoding through FFmpeg (LGPL, linked dynamically): frame-accurate video frames for image planes and
 * reference images, and whole sound files resampled for the timeline.
 */
namespace lila::media
{
namespace fs = std::filesystem;

/** By extension: .mp4 .mov .mkv .webm .avi .m4v .mpg .mpeg .mxf .wmv .flv .gif. */
LILA_SCENE_API bool isVideoFile(const std::string& path);
/** By extension: .wav .mp3 .ogg .flac .aac .m4a .aif .aiff .opus .wma. */
LILA_SCENE_API bool isAudioFile(const std::string& path);

struct VideoInfo
{
    int width = 0;
    int height = 0;
    /** The stream's frame rate; frame n shows from n / fps seconds after its first frame. */
    double fps = 0.0;
    int64_t frameCount = 0;
    double duration = 0.0; // seconds
    std::string codec;
};

/** One decoded picture: RGBA8, row 0 at the top. */
struct VideoFrame
{
    int64_t index = -1;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};
using VideoFramePtr = std::shared_ptr<const VideoFrame>;

/**
 * One video file's frames by index, decoded on the calling thread. Stepping to the next frame decodes on; any other
 * index seeks to the keyframe before it and decodes forward, so every index returns the frame that shows at that time.
 */
class LILA_SCENE_API VideoDecoder
{
public:
    /**
     * Null with err set when the file can't be read or holds no video. maxSize > 0 scales frames down so their longer
     * side is at most maxSize.
     */
    static std::unique_ptr<VideoDecoder> open(const fs::path& path, int maxSize = 0, std::string* err = nullptr);
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    const VideoInfo& info() const;
    /** Frame index, clamped to the stream; null with err set when decoding fails. */
    VideoFramePtr frame(int64_t index, std::string* err = nullptr);
    /** How many seeks frame() has made, for tests of the sequential path. */
    int64_t seeks() const;

    struct Impl;

private:
    explicit VideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> mImpl;
};

/**
 * A video decoded on a thread of its own into a frame cache, for drawing that must never wait: get returns at once
 * with the frame if it is cached, otherwise with the nearest cached one while the thread decodes the asked frame. The
 * newest request always wins, so scrubbing never queues up stale frames; while idle the thread reads ahead in the
 * direction of the requests.
 */
class LILA_SCENE_API VideoStream
{
public:
    struct Options
    {
        int maxSize = 0;
        /** Cache budget; the frames furthest from the last request go first. */
        size_t cacheBytes = size_t(384) << 20;
        /** Frames read ahead past each request while idle. */
        int readAhead = 12;
    };

    /** Opens on the calling thread; null with err set when the file can't be decoded. */
    static std::shared_ptr<VideoStream> open(const fs::path& path, const Options& options, std::string* err = nullptr);
    ~VideoStream();
    VideoStream(const VideoStream&) = delete;
    VideoStream& operator=(const VideoStream&) = delete;

    const VideoInfo& info() const { return mInfo; }
    /** Any thread. exact is set to whether the frame returned is the one asked for. */
    VideoFramePtr get(int64_t index, bool* exact = nullptr);
    /** Blocks until index is decoded or timeoutSeconds pass; for tests and offscreen captures. */
    VideoFramePtr wait(int64_t index, double timeoutSeconds);
    /** Called on the decode thread whenever a requested frame lands. */
    void setOnFrame(std::function<void()> fn);
    size_t cachedBytes() const;

private:
    VideoStream() = default;
    void run();
    void store(VideoFramePtr f);
    VideoFramePtr nearest(int64_t index) const;

    VideoInfo mInfo;
    Options mOptions;
    std::unique_ptr<VideoDecoder> mDecoder;
    mutable std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mLanded;
    std::map<int64_t, VideoFramePtr> mCache;
    size_t mBytes = 0;
    int64_t mWanted = -1;
    int64_t mLastWanted = -1;
    int mDirection = 1;
    bool mQuit = false;
    std::function<void()> mOnFrame;
    std::thread mThread;
};

/** Interleaved float samples at one rate. */
struct AudioBuffer
{
    int rate = 48000;
    int channels = 2;
    std::vector<float> samples;

    int64_t frames() const { return channels > 0 ? int64_t(samples.size()) / channels : 0; }
    double duration() const { return rate > 0 ? double(frames()) / rate : 0.0; }
};
using AudioBufferPtr = std::shared_ptr<const AudioBuffer>;

/** A whole sound file decoded and resampled to stereo float at rate; null with err set on failure. */
LILA_SCENE_API AudioBufferPtr decodeAudio(const fs::path& path, int rate = 48000, std::string* err = nullptr);

/** Waveform envelope: the lowest and highest sample (both channels) per bucket of `secondsPerBucket`. */
struct Peaks
{
    double secondsPerBucket = 0.0;
    std::vector<float> low;
    std::vector<float> high;
};
LILA_SCENE_API Peaks peaks(const AudioBuffer& buffer, double secondsPerBucket);
} // namespace lila::media
