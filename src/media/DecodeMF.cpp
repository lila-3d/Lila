#ifdef _WIN32

#include "media/DecodeBackend.h"

#include "core/Paths.h"
#include "core/Profile.h"

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
#include <propvarutil.h>
#include <wrl/client.h>

#include <format>

namespace lila::media
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr DWORD kVideo = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

bool mfFail(std::string* err, const std::string& what, HRESULT h)
{
    if (err)
        *err = std::format("{} (0x{:08x})", what, uint32_t(h));
    return false;
}

/** COM on this thread (an STA thread already has it, which MF works with too) and MF for the process. */
bool startMf()
{
    thread_local const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)com;
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return started;
}

struct MfVideo final : VideoDecoder::Impl
{
    ComPtr<IMFSourceReader> reader;
    /** The best sample at or before the last target, and one decoded past it that the next request starts from. */
    ComPtr<IMFSample> held, pending;
    int64_t heldIndex = -1, pendingIndex = -1;
    LONGLONG firstTime = 0; // 100 ns units
    int srcW = 0, srcH = 0;
    LONG stride = 0;

    void readType()
    {
        ComPtr<IMFMediaType> t;
        if (FAILED(reader->GetCurrentMediaType(kVideo, &t)))
            return;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        srcW = int(w);
        srcH = int(h);
        UINT32 s = 0;
        stride = SUCCEEDED(t->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? LONG(int32_t(s)) : LONG(w * 4);
    }

    /** 1 with the next sample and its time, 0 at the end, -1 on an error. */
    int next(ComPtr<IMFSample>& out, LONGLONG& time, std::string* err)
    {
        for (;;)
        {
            DWORD stream = 0, flags = 0;
            ComPtr<IMFSample> s;
            if (HRESULT h = reader->ReadSample(kVideo, 0, &stream, &flags, &time, &s); FAILED(h))
            {
                mfFail(err, "Media Foundation can't decode the video", h);
                return -1;
            }
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
                readType();
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                return 0;
            if (s)
            {
                out = std::move(s);
                return 1;
            }
        }
    }

    int64_t indexOf(LONGLONG time) const { return std::llround(double(time - firstTime) * 1e-7 * info.fps); }

    void seek(int64_t index, double backSeconds) override
    {
        const double seconds = std::max(0.0, double(index) / info.fps - backSeconds);
        PROPVARIANT at;
        InitPropVariantFromInt64(firstTime + LONGLONG(seconds * 1e7), &at);
        reader->SetCurrentPosition(GUID_NULL, at);
        PropVariantClear(&at);
        held.Reset();
        pending.Reset();
        heldIndex = pendingIndex = -1;
        ++seeks;
    }

    int64_t heldAt() const override { return held ? heldIndex : -1; }

    bool holdPending(int64_t index) override
    {
        if (!pending)
            return false;
        held = std::move(pending);
        heldIndex = index;
        return true;
    }

    bool decodeTo(int64_t index, std::string* err) override
    {
        bool have = held != nullptr;
        for (;;)
        {
            ComPtr<IMFSample> s;
            int64_t at = 0;
            if (pending)
            {
                s = std::move(pending);
                at = pendingIndex;
            }
            else
            {
                LONGLONG time = 0;
                const int r = next(s, time, err);
                if (r <= 0)
                    return r == 0 ? have : false;
                at = indexOf(time);
            }
            if (at > index)
            {
                pending = std::move(s);
                pendingIndex = at;
                return have;
            }
            held = std::move(s);
            heldIndex = at;
            have = true;
            if (at == index)
                return true;
        }
    }

    VideoFramePtr convertHeld(int64_t index) override
    {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(held->ConvertToContiguousBuffer(&buf)))
            return nullptr;
        BYTE* data = nullptr;
        LONG pitch = stride;
        ComPtr<IMF2DBuffer> twoD;
        bool locked2D = SUCCEEDED(buf.As(&twoD)) && SUCCEEDED(twoD->Lock2D(&data, &pitch));
        if (!locked2D)
        {
            DWORD len = 0;
            if (FAILED(buf->Lock(&data, nullptr, &len)))
                return nullptr;
            // A negative stride is a bottom-up image: its top row is the last one in memory.
            if (pitch < 0)
                data += size_t(-pitch) * size_t(srcH - 1);
        }
        // The decoder pads its pictures to whole macroblocks; the visible part is the container's size, at the top left.
        const int visW = std::min(srcW, probedW), visH = std::min(srcH, probedH);
        auto out = std::make_shared<VideoFrame>();
        out->index = index;
        out->width = outW;
        out->height = outH;
        out->rgba.resize(size_t(outW) * size_t(outH) * 4);
        for (int y = 0; y < outH; ++y)
        {
            const BYTE* row = data + ptrdiff_t(pitch) * ptrdiff_t(int64_t(y) * visH / outH);
            uint8_t* dst = out->rgba.data() + size_t(y) * size_t(outW) * 4;
            for (int x = 0; x < outW; ++x)
            {
                const BYTE* p = row + size_t(int64_t(x) * visW / outW) * 4;
                dst[x * 4 + 0] = p[2];
                dst[x * 4 + 1] = p[1];
                dst[x * 4 + 2] = p[0];
                dst[x * 4 + 3] = 255;
            }
        }
        if (locked2D)
            twoD->Unlock2D();
        else
            buf->Unlock();
        return out;
    }

    int probedW = 0, probedH = 0;
};
} // namespace

std::unique_ptr<VideoDecoder::Impl> openMediaFoundation(const fs::path& path, const VideoInfo& probed, int maxSize, std::string* err)
{
    LILA_ZONE();

    const std::string name = paths::toUtf8(path);
    if (!startMf())
    {
        mfFail(err, name + ": Media Foundation is unavailable", E_FAIL);
        return nullptr;
    }
    auto d = std::make_unique<MfVideo>();
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (HRESULT h = MFCreateSourceReaderFromURL(path.wstring().c_str(), attrs.Get(), &d->reader); FAILED(h))
    {
        mfFail(err, name + ": Media Foundation can't open it", h);
        return nullptr;
    }
    d->reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    d->reader->SetStreamSelection(kVideo, TRUE);
    ComPtr<IMFMediaType> rgb;
    MFCreateMediaType(&rgb);
    rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (HRESULT h = d->reader->SetCurrentMediaType(kVideo, nullptr, rgb.Get()); FAILED(h))
    {
        mfFail(err, name + (probed.codec == "hevc" ? ": Windows has no HEVC decoder (install HEVC Video Extensions)" : ": Windows has no decoder for it"), h);
        return nullptr;
    }
    d->readType();
    d->info = probed;
    d->info.decoder = "mediaFoundation";
    d->probedW = probed.width;
    d->probedH = probed.height;
    fitOutput(*d, maxSize);
    // Frame 0 is the first picture, whatever time the container stamps it with.
    LONGLONG time = 0;
    ComPtr<IMFSample> first;
    if (d->next(first, time, err) == 1)
    {
        d->firstTime = time;
        d->pending = std::move(first);
        d->pendingIndex = 0;
    }
    return d;
}
} // namespace lila::media

#endif
