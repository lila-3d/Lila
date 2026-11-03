#pragma once

#include "media/Decode.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace lila::media
{
/**
 * A video decoder backend. VideoDecoder::frame drives it the same way for each: decode on from the frame held when the
 * target is a short step ahead, otherwise seek to the keyframe before and decode forward, so every backend returns the
 * frame that shows at the asked time.
 */
struct VideoDecoder::Impl
{
    VideoInfo info;
    int outW = 0, outH = 0;
    VideoFramePtr last;
    int64_t seeks = 0;

    virtual ~Impl() = default;
    /** Seeks to the keyframe at or before index's time less backSeconds, dropping the held and pending frames. */
    virtual void seek(int64_t index, double backSeconds) = 0;
    /**
     * Decodes forward, holding the last frame at or before index and keeping the first one past it pending; false when
     * nothing at or before index came out.
     */
    virtual bool decodeTo(int64_t index, std::string* err) = 0;
    /** The held frame's index, -1 when none is held. */
    virtual int64_t heldAt() const = 0;
    /** Holds the pending frame as index; false when there is none. */
    virtual bool holdPending(int64_t index) = 0;
    /** The held frame as RGBA at outW x outH. */
    virtual VideoFramePtr convertHeld(int64_t index) = 0;
};

/** Sets outW and outH from info's size, the longer side capped at maxSize (even sizes), and info's size to match. */
inline void fitOutput(VideoDecoder::Impl& d, int maxSize)
{
    d.outW = d.info.width;
    d.outH = d.info.height;
    if (maxSize > 0 && std::max(d.info.width, d.info.height) > maxSize)
    {
        const double s = double(maxSize) / std::max(d.info.width, d.info.height);
        d.outW = std::max(2, int(std::lround(d.info.width * s)) & ~1);
        d.outH = std::max(2, int(std::lround(d.info.height * s)) & ~1);
    }
    d.info.width = d.outW;
    d.info.height = d.outH;
}

#ifdef _WIN32
/**
 * H.264 and HEVC through Media Foundation, the codecs Windows licenses: FFmpeg ships without those decoders. probed holds
 * the container's rate, length and size as FFmpeg read them.
 */
std::unique_ptr<VideoDecoder::Impl> openMediaFoundation(const fs::path& path, const VideoInfo& probed, int maxSize, std::string* err);
#endif
} // namespace lila::media
