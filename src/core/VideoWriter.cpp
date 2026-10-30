#include "core/VideoWriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#endif

namespace lila::video
{
#ifdef _WIN32
namespace
{
template <class T> struct Com
{
    T* p = nullptr;
    ~Com()
    {
        if (p)
            p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

std::string hr(std::string_view what, HRESULT h)
{
    return std::format("{} failed (0x{:08x})", what, uint32_t(h));
}

class MediaFoundationWriter final : public Writer
{
public:
    ~MediaFoundationWriter() override
    {
        if (mWriter)
            mWriter->Release();
        if (mStarted)
            MFShutdown();
    }

    bool init(const fs::path& path, const Options& o, std::string* err)
    {
        auto fail = [&](std::string e) {
            if (err)
                *err = std::move(e);
            return false;
        };
        mWidth = std::max(o.width & ~1, 2);
        mHeight = std::max(o.height & ~1, 2);
        mSourceWidth = o.width;
        const double fps = std::clamp(o.fps, 1.0, 240.0);
        // Frame rate as a ratio: whole numbers exactly, the NTSC ones as n000/1001.
        UINT32 num = UINT32(std::lround(fps * 1000.0)), den = 1000;
        if (std::abs(fps * 1.001 - std::round(fps * 1.001)) < 1e-3 && std::abs(fps - std::round(fps)) > 1e-3)
        {
            num = UINT32(std::lround(fps * 1.001)) * 1000;
            den = 1001;
        }
        mFrameTicks = LONGLONG(std::llround(1e7 * double(den) / double(num)));

        if (HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED); FAILED(h) && h != RPC_E_CHANGED_MODE)
            return fail(hr("CoInitializeEx", h));
        if (HRESULT h = MFStartup(MF_VERSION); FAILED(h))
            return fail(hr("MFStartup", h));
        mStarted = true;

        Com<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 2);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        if (HRESULT h = MFCreateSinkWriterFromURL(path.wstring().c_str(), nullptr, attrs.p, &mWriter); FAILED(h))
            return fail(hr("creating " + path.string(), h));

        const bool hevc = o.codec == "hevc" || o.codec == "h265";
        int64_t bitrate = o.bitrate;
        if (bitrate <= 0)
        {
            // Bits per pixel per frame, from a soft web encode up to near lossless.
            const double bpp = 0.05 + 0.35 * std::clamp(o.quality, 0.0, 1.0) * (hevc ? 0.7 : 1.0);
            bitrate = int64_t(double(mWidth) * mHeight * fps * bpp);
        }
        Com<IMFMediaType> out;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::clamp<int64_t>(bitrate, 100000, 400000000)));
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (!hevc)
            out->SetUINT32(MF_MT_MPEG2_PROFILE, 100); // eAVEncH264VProfile_High
        MFSetAttributeSize(out.p, MF_MT_FRAME_SIZE, UINT32(mWidth), UINT32(mHeight));
        MFSetAttributeRatio(out.p, MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(out.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (HRESULT h = mWriter->AddStream(out.p, &mStream); FAILED(h))
            return fail(hr(hevc ? "the HEVC encoder (install HEVC Video Extensions)" : "the H.264 encoder", h));

        Com<IMFMediaType> in;
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(mWidth * 4));
        MFSetAttributeSize(in.p, MF_MT_FRAME_SIZE, UINT32(mWidth), UINT32(mHeight));
        MFSetAttributeRatio(in.p, MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(in.p, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (HRESULT h = mWriter->SetInputMediaType(mStream, in.p, nullptr); FAILED(h))
            return fail(hr("setting the encoder's input", h));
        if (HRESULT h = mWriter->BeginWriting(); FAILED(h))
            return fail(hr("starting the encoder", h));
        return true;
    }

    bool add(const uint8_t* bgra, std::string* err) override
    {
        const DWORD bytes = DWORD(mWidth) * DWORD(mHeight) * 4;
        Com<IMFMediaBuffer> buffer;
        if (HRESULT h = MFCreateMemoryBuffer(bytes, &buffer); FAILED(h))
            return fail(err, hr("MFCreateMemoryBuffer", h));
        BYTE* dst = nullptr;
        buffer->Lock(&dst, nullptr, nullptr);
        for (int y = 0; y < mHeight; ++y)
            std::memcpy(dst + size_t(y) * mWidth * 4, bgra + size_t(y) * mSourceWidth * 4, size_t(mWidth) * 4);
        buffer->Unlock();
        buffer->SetCurrentLength(bytes);
        Com<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(buffer.p);
        sample->SetSampleTime(mFrames * mFrameTicks);
        sample->SetSampleDuration(mFrameTicks);
        if (HRESULT h = mWriter->WriteSample(mStream, sample.p); FAILED(h))
            return fail(err, hr("encoding a frame", h));
        ++mFrames;
        return true;
    }

    bool finish(std::string* err) override
    {
        if (!mWriter)
            return true;
        HRESULT h = mWriter->Finalize();
        mWriter->Release();
        mWriter = nullptr;
        return SUCCEEDED(h) || fail(err, hr("finishing the file", h));
    }

private:
    static bool fail(std::string* err, std::string e)
    {
        if (err)
            *err = std::move(e);
        return false;
    }

    IMFSinkWriter* mWriter = nullptr;
    DWORD mStream = 0;
    bool mStarted = false;
    int mWidth = 0, mHeight = 0, mSourceWidth = 0;
    LONGLONG mFrameTicks = 0, mFrames = 0;
};
} // namespace

std::unique_ptr<Writer> Writer::open(const fs::path& path, const Options& options, std::string* err)
{
    auto w = std::make_unique<MediaFoundationWriter>();
    if (!w->init(path, options, err))
        return nullptr;
    return w;
}

bool available() { return true; }
#else
std::unique_ptr<Writer> Writer::open(const fs::path&, const Options&, std::string* err)
{
    if (err)
        *err = "video encoding is not available on this platform yet; render an image sequence";
    return nullptr;
}

bool available() { return false; }
#endif
} // namespace lila::video
