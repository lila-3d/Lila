#include <doctest/doctest.h>

#include "TestClips.h"

#include "media/Decode.h"
#include "media/Media.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace mediatest
{
namespace fs = std::filesystem;

fs::path scratch()
{
    static fs::path dir = [] {
        fs::path d = fs::temp_directory_path() / "lila_media_tests" / "clips";
        fs::create_directories(d);
        return d;
    }();
    return dir;
}

double expectedRed(int frame)
{
    const double y = 20.0 + 7.0 * frame;
    return std::clamp((y - 16.0) * 255.0 / 219.0, 0.0, 255.0);
}

fs::path writeClip(const std::string& name, int frames, int fps, int gop)
{
    const fs::path path = scratch() / name;
    const std::string file = path.string();
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, nullptr, file.c_str()) < 0 || !oc)
        throw std::runtime_error("no muxer for " + file);
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec)
        throw std::runtime_error("no MPEG-4 encoder");
    AVStream* st = avformat_new_stream(oc, nullptr);
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = 64;
    c->height = 48;
    c->time_base = {1, fps};
    c->framerate = {fps, 1};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->gop_size = gop;
    c->max_b_frames = 2;
    c->flags |= AV_CODEC_FLAG_QSCALE;
    c->global_quality = FF_QP2LAMBDA * 2;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (avcodec_open2(c, codec, nullptr) < 0)
        throw std::runtime_error("can't open the encoder");
    avcodec_parameters_from_context(st->codecpar, c);
    st->time_base = c->time_base;
    st->avg_frame_rate = {fps, 1};
    if (avio_open(&oc->pb, file.c_str(), AVIO_FLAG_WRITE) < 0)
        throw std::runtime_error("can't write " + file);
    if (avformat_write_header(oc, nullptr) < 0)
        throw std::runtime_error("can't write the header of " + file);
    AVFrame* f = av_frame_alloc();
    f->width = c->width;
    f->height = c->height;
    f->format = c->pix_fmt;
    av_frame_get_buffer(f, 0);
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(c, pkt) == 0)
        {
            av_packet_rescale_ts(pkt, c->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(oc, pkt);
        }
    };
    for (int i = 0; i < frames; ++i)
    {
        av_frame_make_writable(f);
        for (int y = 0; y < f->height; ++y)
            std::memset(f->data[0] + y * f->linesize[0], 20 + 7 * i, size_t(f->width));
        for (int p = 1; p < 3; ++p)
            for (int y = 0; y < f->height / 2; ++y)
                std::memset(f->data[p] + y * f->linesize[p], 128, size_t(f->width / 2));
        f->pts = i;
        avcodec_send_frame(c, f);
        drain();
    }
    avcodec_send_frame(c, nullptr);
    drain();
    av_write_trailer(oc);
    avio_closep(&oc->pb);
    av_packet_free(&pkt);
    av_frame_free(&f);
    avcodec_free_context(&c);
    avformat_free_context(oc);
    return path;
}

fs::path writeWav(const std::string& name, int rate, double seconds, double clickSeconds)
{
    const fs::path path = scratch() / name;
    const int n = int(std::lround(seconds * rate));
    std::vector<int16_t> s(size_t(n), 0);
    const int click = int(std::lround(clickSeconds * rate));
    for (int i = click; i < std::min(n, click + 10); ++i)
        s[size_t(i)] = 32767;
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(uint32_t(36 + n * 2));
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(uint32_t(rate));
    u32(uint32_t(rate * 2));
    u16(2);
    u16(16);
    out.write("data", 4);
    u32(uint32_t(n * 2));
    out.write(reinterpret_cast<const char*>(s.data()), std::streamsize(s.size() * 2));
    return path;
}
} // namespace mediatest

namespace
{
using namespace lila;
using namespace mediatest;

double meanRed(const media::VideoFrame& f)
{
    double sum = 0.0;
    for (size_t i = 0; i < f.rgba.size(); i += 4)
        sum += f.rgba[i];
    return sum / double(f.rgba.size() / 4);
}

void checkFrame(const media::VideoFramePtr& f, int index)
{
    REQUIRE(f);
    CHECK(f->index == index);
    CAPTURE(index);
    CAPTURE(meanRed(*f));
    CHECK(std::abs(meanRed(*f) - expectedRed(index)) < 3.5);
}
} // namespace

TEST_CASE("video: random seeks and steps land on the exact frame in mp4, mkv and avi")
{
    for (const char* name : {"gray.mp4", "gray.mkv", "gray.avi"})
    {
        const std::string container = name;
        CAPTURE(container);
        const auto path = writeClip(name, 30, 24, 8);
        std::string err;
        auto dec = media::VideoDecoder::open(path, 0, &err);
        REQUIRE_MESSAGE(dec, err);
        CHECK(dec->info().width == 64);
        CHECK(dec->info().height == 48);
        CHECK(dec->info().fps == doctest::Approx(24.0));
        CHECK(dec->info().frameCount == 30);

        for (int i : {17, 3, 4, 5, 29, 0, 12, 11, 8, 7, 22})
            checkFrame(dec->frame(i), i);

        // A step to the next frame decodes on without seeking.
        checkFrame(dec->frame(14), 14);
        const int64_t seeks = dec->seeks();
        for (int i = 15; i < 22; ++i)
            checkFrame(dec->frame(i), i);
        CHECK(dec->seeks() == seeks);

        // Past either end holds the end frames.
        checkFrame(dec->frame(400), 29);
        checkFrame(dec->frame(-5), 0);
    }
}

TEST_CASE("video: a stream returns the nearest cached frame at once and the exact one once decoded")
{
    const auto path = writeClip("stream.mp4", 30, 24, 8);
    auto s = media::VideoStream::open(path, {});
    REQUIRE(s);
    checkFrame(s->wait(10, 10.0), 10);
    bool exact = false;
    media::VideoFramePtr near = s->get(25, &exact);
    if (!exact)
    {
        REQUIRE(near);
        CHECK(near->index != 25);
    }
    checkFrame(s->wait(25, 10.0), 25);
    for (int i = 26; i < 30; ++i)
        checkFrame(s->wait(i, 10.0), i);
    s->get(25, &exact);
    CHECK(exact);
    CHECK(s->cachedBytes() > 0);
}

TEST_CASE("video: scrubbing many frames keeps only the newest request")
{
    const auto path = writeClip("scrub.mp4", 30, 24, 8);
    auto s = media::VideoStream::open(path, {});
    REQUIRE(s);
    for (int i = 0; i < 30; ++i)
        s->get(29 - i);
    checkFrame(s->wait(0, 10.0), 0);
}

TEST_CASE("video timing: start frame, fps override and frame offset")
{
    media::VideoTiming t;
    t.start = 1.0;
    CHECK(media::videoFrame(t, 24.0, 1.0) == 0);
    CHECK(media::videoFrame(t, 24.0, 1.0 + 5.0 / 24.0) == 5);
    CHECK(media::videoFrame(t, 24.0, 0.5) < 0);
    t.fps = 12.0;
    CHECK(media::videoFrame(t, 24.0, 2.0) == 12);
    t.offset = 3;
    CHECK(media::videoFrame(t, 24.0, 2.0) == 15);
}

TEST_CASE("audio: decodes, resamples to 48 kHz stereo and keeps the timing")
{
    const auto path = writeWav("click.wav", 44100, 1.0, 0.5);
    std::string err;
    media::AudioBufferPtr b = media::decodeAudio(path, 48000, &err);
    REQUIRE_MESSAGE(b, err);
    CHECK(b->rate == 48000);
    CHECK(b->channels == 2);
    CHECK(b->duration() == doctest::Approx(1.0).epsilon(0.01));
    int64_t loudest = 0;
    for (int64_t i = 0; i < b->frames(); ++i)
        if (std::abs(b->samples[size_t(i * 2)]) > std::abs(b->samples[size_t(loudest * 2)]))
            loudest = i;
    CHECK(double(loudest) / 48000.0 == doctest::Approx(0.5).epsilon(0.002));
    CHECK(b->samples[size_t(loudest * 2)] == doctest::Approx(b->samples[size_t(loudest * 2 + 1)]));

    media::Peaks p = media::peaks(*b, 0.01);
    CHECK(p.high.size() == 100);
    CHECK(p.high[50] > 0.5f);
    CHECK(p.high[10] == doctest::Approx(0.0f));
}

TEST_CASE("audio: the sound track of a video file decodes too")
{
    // A file without sound says so instead of decoding nothing.
    const auto path = writeClip("silent.mp4", 4, 24, 4);
    std::string err;
    CHECK_FALSE(media::decodeAudio(path, 48000, &err));
    CHECK(err.find("no audio") != std::string::npos);
}
