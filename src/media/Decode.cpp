#include "media/Decode.h"

#include "core/Paths.h"
#include "core/Profile.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>

namespace lila::media
{
namespace
{
std::string extensionOf(const std::string& path)
{
    std::string ext = paths::toUtf8(paths::fromUtf8(path).extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

std::string avError(int code)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    av_strerror(code, buf.data(), buf.size());
    return buf.data();
}

bool fail(std::string* err, std::string message)
{
    if (err)
        *err = std::move(message);
    return false;
}

struct Packet
{
    AVPacket* p = av_packet_alloc();
    ~Packet() { av_packet_free(&p); }
};

struct Frame
{
    AVFrame* f = av_frame_alloc();
    ~Frame() { av_frame_free(&f); }
};

/** An opened file with one stream's decoder; shared by the video and audio readers. */
struct Input
{
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    int stream = -1;
    bool flushed = false; // the decoder has been sent its end of stream

    ~Input()
    {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
    }

    bool open(const fs::path& path, AVMediaType type, std::string* err)
    {
        const std::string utf8 = paths::toUtf8(path);
        if (int r = avformat_open_input(&fmt, utf8.c_str(), nullptr, nullptr); r < 0)
            return fail(err, utf8 + ": " + avError(r));
        if (int r = avformat_find_stream_info(fmt, nullptr); r < 0)
            return fail(err, utf8 + ": " + avError(r));
        const AVCodec* codec = nullptr;
        stream = av_find_best_stream(fmt, type, -1, -1, &codec, 0);
        if (stream < 0 || !codec)
            return fail(err, utf8 + (type == AVMEDIA_TYPE_VIDEO ? ": no video stream" : ": no audio stream"));
        dec = avcodec_alloc_context3(codec);
        if (!dec || avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0)
            return fail(err, utf8 + ": unsupported codec");
        dec->thread_count = 0;
        if (int r = avcodec_open2(dec, codec, nullptr); r < 0)
            return fail(err, utf8 + ": " + avError(r));
        return true;
    }

    AVStream* st() const { return fmt->streams[stream]; }

    /** The next decoded frame: 0, AVERROR_EOF at the end, or another error. */
    int receive(AVFrame* out, AVPacket* pkt)
    {
        for (;;)
        {
            int r = avcodec_receive_frame(dec, out);
            if (r != AVERROR(EAGAIN))
                return r;
            if (flushed)
                return AVERROR_EOF;
            r = av_read_frame(fmt, pkt);
            if (r < 0)
            {
                avcodec_send_packet(dec, nullptr);
                flushed = true;
                continue;
            }
            if (pkt->stream_index == stream)
                r = avcodec_send_packet(dec, pkt);
            av_packet_unref(pkt);
            if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA)
                return r;
        }
    }
};
} // namespace

bool isVideoFile(const std::string& path)
{
    static const char* const kExt[] = {".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".mpg", ".mpeg", ".mxf", ".wmv", ".flv", ".gif"};
    const std::string ext = extensionOf(path);
    return std::ranges::any_of(kExt, [&](const char* e) { return ext == e; });
}

bool isAudioFile(const std::string& path)
{
    static const char* const kExt[] = {".wav", ".mp3", ".ogg", ".flac", ".aac", ".m4a", ".aif", ".aiff", ".opus", ".wma"};
    const std::string ext = extensionOf(path);
    return std::ranges::any_of(kExt, [&](const char* e) { return ext == e; });
}

// ---------------------------------------------------------------- VideoDecoder

struct VideoDecoder::Impl
{
    Input in;
    Packet pkt;
    Frame decoded;
    /** The best frame at or before the last target, and a frame decoded past it that the next request starts from. */
    Frame held;
    Frame pending;
    bool hasPending = false;
    SwsContext* sws = nullptr;
    VideoInfo info;
    AVRational tb{1, 1};
    int64_t startPts = 0;
    int outW = 0, outH = 0;
    VideoFramePtr last;
    int64_t seeks = 0;
    /** The last decoded frame's index; a frame without a timestamp (a stream's delayed last picture) follows it. */
    int64_t lastAt = -1;

    ~Impl() { sws_freeContext(sws); }

    int64_t indexOf(const AVFrame* f)
    {
        int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        lastAt = pts == AV_NOPTS_VALUE ? lastAt + 1 : std::llround(double(pts - startPts) * av_q2d(tb) * info.fps);
        return lastAt;
    }

    void seek(int64_t index, double backSeconds)
    {
        const double seconds = std::max(0.0, double(index) / info.fps - backSeconds);
        const int64_t ts = startPts + int64_t(seconds / av_q2d(tb));
        av_seek_frame(in.fmt, in.stream, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(in.dec);
        in.flushed = false;
        hasPending = false;
        av_frame_unref(pending.f);
        av_frame_unref(held.f);
        lastAt = -1;
        ++seeks;
    }

    VideoFramePtr convert(const AVFrame* f, int64_t index)
    {
        sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), outW, outH, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                   nullptr, nullptr);
        if (!sws)
            return nullptr;
        const int* coeffs = sws_getCoefficients(f->colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT);
        sws_setColorspaceDetails(sws, coeffs, f->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        auto out = std::make_shared<VideoFrame>();
        out->index = index;
        out->width = outW;
        out->height = outH;
        out->rgba.resize(size_t(outW) * size_t(outH) * 4);
        uint8_t* dst[4] = {out->rgba.data(), nullptr, nullptr, nullptr};
        int stride[4] = {outW * 4, 0, 0, 0};
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst, stride);
        return out;
    }

    /** Decodes forward to the frame showing at index; false when nothing at or before it came out. */
    bool decodeTo(int64_t index, std::string* err)
    {
        bool have = held.f->buf[0] != nullptr;
        for (;;)
        {
            AVFrame* f = decoded.f;
            if (hasPending)
            {
                av_frame_unref(f);
                av_frame_move_ref(f, pending.f);
                hasPending = false;
            }
            else if (int r = in.receive(f, pkt.p); r == AVERROR_EOF)
                return have;
            else if (r < 0)
                return fail(err, avError(r));
            const int64_t at = indexOf(f);
            if (at > index)
            {
                av_frame_move_ref(pending.f, f);
                hasPending = true;
                return have;
            }
            av_frame_unref(held.f);
            av_frame_move_ref(held.f, f);
            held.f->pts = at;
            have = true;
            if (at == index)
                return true;
        }
    }
};

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : mImpl(std::move(impl)) {}
VideoDecoder::~VideoDecoder() = default;

std::unique_ptr<VideoDecoder> VideoDecoder::open(const fs::path& path, int maxSize, std::string* err)
{
    LILA_ZONE();

    auto impl = std::make_unique<Impl>();
    if (!impl->in.open(path, AVMEDIA_TYPE_VIDEO, err))
        return nullptr;
    AVStream* st = impl->in.st();
    AVCodecContext* dec = impl->in.dec;
    VideoInfo& info = impl->info;
    info.width = dec->width;
    info.height = dec->height;
    info.codec = avcodec_get_name(dec->codec_id);
    double fps = av_q2d(st->avg_frame_rate);
    if (!(fps > 0.0) || !std::isfinite(fps))
        fps = av_q2d(st->r_frame_rate);
    info.fps = fps > 0.0 && std::isfinite(fps) ? fps : 25.0;
    impl->tb = st->time_base;
    impl->startPts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        info.duration = double(st->duration) * av_q2d(st->time_base);
    else if (impl->in.fmt->duration != AV_NOPTS_VALUE)
        info.duration = double(impl->in.fmt->duration) / AV_TIME_BASE;
    info.frameCount = st->nb_frames > 0 ? st->nb_frames : std::max<int64_t>(1, std::llround(info.duration * info.fps));
    if (info.width <= 0 || info.height <= 0)
    {
        fail(err, paths::toUtf8(path) + ": no picture size");
        return nullptr;
    }
    impl->outW = info.width;
    impl->outH = info.height;
    if (maxSize > 0 && std::max(info.width, info.height) > maxSize)
    {
        const double s = double(maxSize) / std::max(info.width, info.height);
        impl->outW = std::max(2, int(std::lround(info.width * s)) & ~1);
        impl->outH = std::max(2, int(std::lround(info.height * s)) & ~1);
    }
    info.width = impl->outW;
    info.height = impl->outH;
    // Frame 0 is the first picture: containers without presentation times (AVI with B-frames) stamp it a frame late.
    if (impl->in.receive(impl->pending.f, impl->pkt.p) == 0)
    {
        const AVFrame* f = impl->pending.f;
        if (const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts; pts != AV_NOPTS_VALUE)
            impl->startPts = pts;
        impl->hasPending = true;
    }
    return std::unique_ptr<VideoDecoder>(new VideoDecoder(std::move(impl)));
}

const VideoInfo& VideoDecoder::info() const
{
    return mImpl->info;
}

int64_t VideoDecoder::seeks() const
{
    return mImpl->seeks;
}

VideoFramePtr VideoDecoder::frame(int64_t index, std::string* err)
{
    LILA_ZONE();

    Impl& d = *mImpl;
    index = std::clamp<int64_t>(index, 0, std::max<int64_t>(0, d.info.frameCount - 1));
    if (d.last && d.last->index == index)
        return d.last;
    // Decoding on is cheaper than a seek for a short step forward; a GOP is rarely longer than this.
    constexpr int64_t kForward = 48;
    const int64_t heldAt = d.held.f->buf[0] ? d.held.f->pts : -1;
    const bool ahead = heldAt >= 0 && index >= heldAt && index - heldAt <= kForward;
    bool found = false;
    if (ahead)
        found = d.decodeTo(index, err);
    for (double back : {0.0, 1.0, 5.0})
    {
        if (found)
            break;
        d.seek(index, back);
        found = d.decodeTo(index, err);
    }
    // A stream whose first picture is stamped after the asked time shows that picture.
    if (!found && d.hasPending)
    {
        av_frame_unref(d.held.f);
        av_frame_move_ref(d.held.f, d.pending.f);
        d.hasPending = false;
        d.held.f->pts = index;
        found = true;
    }
    if (!found)
    {
        if (err && err->empty())
            *err = "no frame at " + std::to_string(index);
        return nullptr;
    }
    VideoFramePtr out = d.convert(d.held.f, index);
    if (!out)
    {
        fail(err, "can't convert the picture");
        return nullptr;
    }
    d.last = out;
    return out;
}

// ---------------------------------------------------------------- VideoStream

std::shared_ptr<VideoStream> VideoStream::open(const fs::path& path, const Options& options, std::string* err)
{
    std::unique_ptr<VideoDecoder> dec = VideoDecoder::open(path, options.maxSize, err);
    if (!dec)
        return nullptr;
    std::shared_ptr<VideoStream> s(new VideoStream());
    s->mInfo = dec->info();
    s->mOptions = options;
    s->mDecoder = std::move(dec);
    s->mThread = std::thread([raw = s.get()] { raw->run(); });
    return s;
}

VideoStream::~VideoStream()
{
    {
        std::lock_guard lk(mMutex);
        mQuit = true;
    }
    mWake.notify_all();
    if (mThread.joinable())
        mThread.join();
}

void VideoStream::setOnFrame(std::function<void()> fn)
{
    std::lock_guard lk(mMutex);
    mOnFrame = std::move(fn);
}

size_t VideoStream::cachedBytes() const
{
    std::lock_guard lk(mMutex);
    return mBytes;
}

VideoFramePtr VideoStream::nearest(int64_t index) const
{
    if (mCache.empty())
        return nullptr;
    auto it = mCache.lower_bound(index);
    if (it == mCache.end())
        return std::prev(it)->second;
    if (it == mCache.begin())
        return it->second;
    auto before = std::prev(it);
    return index - before->first <= it->first - index ? before->second : it->second;
}

VideoFramePtr VideoStream::get(int64_t index, bool* exact)
{
    index = std::clamp<int64_t>(index, 0, std::max<int64_t>(0, mInfo.frameCount - 1));
    std::lock_guard lk(mMutex);
    if (index != mLastWanted)
    {
        if (mLastWanted >= 0)
            mDirection = index > mLastWanted ? 1 : -1;
        mLastWanted = index;
        mWake.notify_one();
    }
    if (auto it = mCache.find(index); it != mCache.end())
    {
        if (exact)
            *exact = true;
        return it->second;
    }
    mWanted = index;
    mWake.notify_one();
    if (exact)
        *exact = false;
    return nearest(index);
}

VideoFramePtr VideoStream::wait(int64_t index, double timeoutSeconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    bool exact = false;
    VideoFramePtr f = get(index, &exact);
    std::unique_lock lk(mMutex);
    const int64_t key = std::clamp<int64_t>(index, 0, std::max<int64_t>(0, mInfo.frameCount - 1));
    while (!exact)
    {
        if (auto it = mCache.find(key); it != mCache.end())
            return it->second;
        if (mLanded.wait_until(lk, until) == std::cv_status::timeout)
            return nullptr;
    }
    return f;
}

void VideoStream::store(VideoFramePtr f)
{
    const size_t bytes = f->rgba.size();
    auto [it, added] = mCache.try_emplace(f->index, f);
    if (!added)
        return;
    mBytes += bytes;
    while (mBytes > mOptions.cacheBytes && mCache.size() > 1)
    {
        auto front = mCache.begin(), back = std::prev(mCache.end());
        auto victim = std::abs(front->first - mLastWanted) >= std::abs(back->first - mLastWanted) ? front : back;
        mBytes -= victim->second->rgba.size();
        mCache.erase(victim);
    }
}

void VideoStream::run()
{
    std::unique_lock lk(mMutex);
    while (!mQuit)
    {
        int64_t job = -1;
        bool requested = false;
        if (mWanted >= 0)
        {
            job = std::exchange(mWanted, -1);
            requested = !mCache.contains(job);
            if (!requested)
                job = -1;
        }
        if (job < 0 && mLastWanted >= 0)
            for (int k = 1; k <= mOptions.readAhead; ++k)
            {
                const int64_t i = mLastWanted + int64_t(mDirection) * k;
                if (i < 0 || i >= mInfo.frameCount)
                    break;
                if (!mCache.contains(i))
                {
                    job = i;
                    break;
                }
            }
        if (job < 0)
        {
            mWake.wait(lk);
            continue;
        }
        lk.unlock();
        VideoFramePtr f = mDecoder->frame(job);
        lk.lock();
        if (f)
        {
            if (f->index != job)
            {
                auto copy = std::make_shared<VideoFrame>(*f);
                copy->index = job;
                f = std::move(copy);
            }
            store(f);
        }
        else
        {
            // An undecodable frame holds its neighbour, so the read-ahead moves past it instead of retrying forever.
            if (VideoFramePtr n = nearest(job))
            {
                auto copy = std::make_shared<VideoFrame>(*n);
                copy->index = job;
                store(copy);
            }
            else
                mLastWanted = -1;
        }
        mLanded.notify_all();
        if (requested && mOnFrame)
        {
            std::function<void()> fn = mOnFrame;
            lk.unlock();
            fn();
            lk.lock();
        }
    }
}

// ---------------------------------------------------------------- audio

AudioBufferPtr decodeAudio(const fs::path& path, int rate, std::string* err)
{
    LILA_ZONE();

    Input in;
    if (!in.open(path, AVMEDIA_TYPE_AUDIO, err))
        return nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout source{};
    if (av_channel_layout_check(&in.dec->ch_layout) && in.dec->ch_layout.nb_channels > 0)
        av_channel_layout_copy(&source, &in.dec->ch_layout);
    else
        av_channel_layout_default(&source, std::max(1, in.dec->ch_layout.nb_channels));
    SwrContext* swr = nullptr;
    int r = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, rate, &source, in.dec->sample_fmt, in.dec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&source);
    if (r < 0 || swr_init(swr) < 0)
    {
        swr_free(&swr);
        fail(err, paths::toUtf8(path) + ": can't resample the audio");
        return nullptr;
    }
    auto out = std::make_shared<AudioBuffer>();
    out->rate = rate;
    out->channels = 2;
    Packet pkt;
    Frame frame;
    auto append = [&](const uint8_t** data, int count) {
        const int room = swr_get_out_samples(swr, count);
        if (room <= 0)
            return;
        const size_t at = out->samples.size();
        out->samples.resize(at + size_t(room) * 2);
        uint8_t* dst = reinterpret_cast<uint8_t*>(out->samples.data() + at);
        const int got = swr_convert(swr, &dst, room, data, count);
        out->samples.resize(at + size_t(std::max(got, 0)) * 2);
    };
    for (;;)
    {
        r = in.receive(frame.f, pkt.p);
        if (r == AVERROR_EOF)
            break;
        if (r < 0)
        {
            swr_free(&swr);
            fail(err, paths::toUtf8(path) + ": " + avError(r));
            return nullptr;
        }
        append(const_cast<const uint8_t**>(frame.f->extended_data), frame.f->nb_samples);
        av_frame_unref(frame.f);
    }
    append(nullptr, 0);
    swr_free(&swr);
    return out;
}

Peaks peaks(const AudioBuffer& buffer, double secondsPerBucket)
{
    Peaks p;
    p.secondsPerBucket = secondsPerBucket;
    if (!(secondsPerBucket > 0.0) || buffer.channels <= 0)
        return p;
    const int64_t per = std::max<int64_t>(1, std::llround(secondsPerBucket * buffer.rate));
    const int64_t frames = buffer.frames();
    const size_t buckets = size_t((frames + per - 1) / per);
    p.low.assign(buckets, 0.0f);
    p.high.assign(buckets, 0.0f);
    for (size_t b = 0; b < buckets; ++b)
    {
        float lo = 0.0f, hi = 0.0f;
        const int64_t end = std::min(frames, int64_t(b + 1) * per);
        for (int64_t f = int64_t(b) * per; f < end; ++f)
            for (int c = 0; c < buffer.channels; ++c)
            {
                const float v = buffer.samples[size_t(f * buffer.channels + c)];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        p.low[b] = lo;
        p.high[b] = hi;
    }
    return p;
}
} // namespace lila::media
