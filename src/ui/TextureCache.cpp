#include "ui/TextureCache.h"

#include "core/MainQueue.h"
#include "core/Profile.h"
#include "render/HdrImage.h"
#include "ui/ViewportTool.h"

#include <QThreadPool>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <unordered_map>

namespace lila::ui::textures
{
namespace
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr int kMaxSize = 8192;

struct Result
{
    render::TexturePtr image;
    QImage thumb;
    std::string error;
    std::vector<fs::file_time_type> times;
};

struct Slot
{
    bool loading = true;
    render::TexturePtr image;
    QImage thumb;
    std::string error;
    std::vector<std::string> files;
    std::vector<fs::file_time_type> times;
    Clock::time_point checked = Clock::now();
};

struct Cache
{
    std::unordered_map<std::string, Slot> entries;
    std::map<int, std::function<void()>> listeners;
    int nextListener = 1;
};

// Leaked on purpose: workers may still post results while the app shuts down.
Cache& cache()
{
    static Cache* c = new Cache;
    return *c;
}

fs::path toPath(const std::string& s)
{
    return fs::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

std::vector<fs::file_time_type> stamps(const std::vector<std::string>& files)
{
    std::vector<fs::file_time_type> out;
    for (const std::string& f : files)
    {
        std::error_code ec;
        out.push_back(f.empty() ? fs::file_time_type() : fs::last_write_time(toPath(f), ec));
    }
    return out;
}

/** Uncompressed or RLE truecolor TGA, which Qt can't read without its extra image formats. */
QImage readTga(const fs::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (d.size() < 18)
    {
        error = "not a TGA file";
        return {};
    }
    int idLen = d[0], cmapType = d[1], type = d[2];
    int w = d[12] | (d[13] << 8), h = d[14] | (d[15] << 8), bpp = d[16], desc = d[17];
    if (cmapType != 0 || (type != 2 && type != 3 && type != 10 && type != 11) || (bpp != 8 && bpp != 24 && bpp != 32) || w <= 0 || h <= 0)
    {
        error = "unsupported TGA (only 8, 24 and 32 bit truecolor or grayscale)";
        return {};
    }
    size_t bytes = size_t(bpp / 8);
    size_t pos = 18 + size_t(idLen);
    std::vector<uint8_t> px(size_t(w) * size_t(h) * bytes);
    if (type == 2 || type == 3)
    {
        if (pos + px.size() > d.size())
        {
            error = "truncated TGA";
            return {};
        }
        std::copy(d.begin() + std::ptrdiff_t(pos), d.begin() + std::ptrdiff_t(pos + px.size()), px.begin());
    }
    else
    {
        size_t out = 0;
        while (out < px.size() && pos < d.size())
        {
            int header = d[pos++];
            size_t count = size_t(header & 0x7f) + 1;
            bool run = header & 0x80;
            for (size_t i = 0; i < count && out < px.size(); ++i)
            {
                size_t src = run ? pos : pos + i * bytes;
                if (src + bytes > d.size())
                    break;
                std::copy(d.begin() + std::ptrdiff_t(src), d.begin() + std::ptrdiff_t(src + bytes), px.begin() + std::ptrdiff_t(out));
                out += bytes;
            }
            pos += run ? bytes : count * bytes;
        }
    }
    QImage img(w, h, QImage::Format_RGBA8888);
    bool topDown = desc & 0x20;
    for (int y = 0; y < h; ++y)
    {
        uchar* row = img.scanLine(topDown ? y : h - 1 - y);
        for (int x = 0; x < w; ++x)
        {
            const uint8_t* p = &px[(size_t(y) * size_t(w) + size_t(x)) * bytes];
            if (bytes == 1)
                row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = p[0];
            else
            {
                row[x * 4] = p[2];
                row[x * 4 + 1] = p[1];
                row[x * 4 + 2] = p[0];
            }
            row[x * 4 + 3] = bytes == 4 ? p[3] : 255;
        }
    }
    return img;
}

/** Radiance HDR, encoded to 8-bit sRGB; the viewport only needs it as a color map. */
QImage readHdr(const fs::path& path, std::string& error)
{
    render::HdrImage hdr;
    if (!render::loadHdr(path, hdr, error))
        return {};
    QImage img(hdr.width, hdr.height, QImage::Format_RGBA8888);
    auto encode = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
        return uchar(std::lround(v * 255.0f));
    };
    for (int y = 0; y < hdr.height; ++y)
    {
        uchar* row = img.scanLine(y);
        for (int x = 0; x < hdr.width; ++x)
        {
            glm::vec3 c = hdr.at(x, y);
            row[x * 4] = encode(c.r);
            row[x * 4 + 1] = encode(c.g);
            row[x * 4 + 2] = encode(c.b);
            row[x * 4 + 3] = 255;
        }
    }
    return img;
}

QImage readImage(const std::string& file, std::string& error)
{
    LILA_ZONE();

    fs::path path = toPath(file);
    std::error_code ec;
    if (!fs::exists(path, ec))
    {
        error = "file not found";
        return {};
    }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    QImage img;
    if (ext == ".tga")
        img = readTga(path, error);
    else if (ext == ".hdr")
        img = readHdr(path, error);
    else
    {
        img = QImage(QString::fromStdU16String(path.u16string()));
        if (img.isNull())
            error = "unsupported or unreadable image";
    }
    if (img.isNull())
        return {};
    if (img.width() > kMaxSize || img.height() > kMaxSize)
        img = img.scaled(kMaxSize, kMaxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img.convertToFormat(QImage::Format_RGBA8888);
}

render::TexturePtr toTexture(const QImage& rgba, bool srgb)
{
    auto t = std::make_shared<render::TextureImage>();
    t->width = rgba.width();
    t->height = rgba.height();
    t->srgb = srgb;
    t->rgba.resize(size_t(t->width) * size_t(t->height) * 4);
    for (int y = 0; y < t->height; ++y)
        std::memcpy(t->rgba.data() + size_t(y) * size_t(t->width) * 4, rgba.constScanLine(y), size_t(t->width) * 4);
    return t;
}

Result loadPacked(const std::vector<std::string>& files, std::array<int, 3> channels)
{
    LILA_ZONE();

    Result r;
    r.times = stamps(files);
    if (!files[0].empty())
    {
        QImage img = readImage(files[0], r.error);
        if (img.isNull())
            return r;
        if (channels != std::array<int, 3>{0, 1, 2})
            for (int y = 0; y < img.height(); ++y)
            {
                uchar* d = img.scanLine(y);
                for (int x = 0; x < img.width(); ++x)
                {
                    uchar* px = d + x * 4;
                    const uchar src[3] = {px[0], px[1], px[2]};
                    for (int i = 0; i < 3; ++i)
                        px[i] = src[channels[size_t(i)]];
                }
            }
        r.image = toTexture(img, false);
        return r;
    }
    QImage parts[3];
    int w = 1, h = 1;
    for (int i = 0; i < 3; ++i)
    {
        if (files[size_t(i) + 1].empty())
            continue;
        std::string err;
        parts[i] = readImage(files[size_t(i) + 1], err);
        if (parts[i].isNull())
            r.error += (r.error.empty() ? "" : "; ") + files[size_t(i) + 1] + ": " + err;
        w = std::max(w, parts[i].width());
        h = std::max(h, parts[i].height());
    }
    QImage out(w, h, QImage::Format_RGBA8888);
    out.fill(Qt::white);
    for (int i = 0; i < 3; ++i)
    {
        if (parts[i].isNull())
            continue;
        QImage src = parts[i].size() == out.size() ? parts[i] : parts[i].scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        for (int y = 0; y < h; ++y)
        {
            const uchar* s = src.constScanLine(y);
            uchar* d = out.scanLine(y);
            for (int x = 0; x < w; ++x)
                d[x * 4 + i] = s[x * 4];
        }
    }
    r.image = toTexture(out, false);
    return r;
}

void finish(const std::string& key, Result r)
{
    auto& c = cache();
    auto it = c.entries.find(key);
    if (it == c.entries.end())
        return;
    Slot& s = it->second;
    s.loading = false;
    s.times = std::move(r.times);
    s.error = std::move(r.error);
    // A failed reload keeps the last good image on screen.
    if (r.image || s.error.empty())
        s.image = std::move(r.image);
    if (!r.thumb.isNull() || !s.error.empty())
        s.thumb = std::move(r.thumb);
    auto listeners = c.listeners;
    for (auto& [id, fn] : listeners)
        fn();
    viewportTools::requestRedraw();
}

void start(const std::string& key, Slot& s, std::function<Result()> work)
{
    s.loading = true;
    s.checked = Clock::now();
    QThreadPool::globalInstance()->start([key, work = std::move(work)] {
        Result r = work();
        mainq::post([key, r = std::move(r)]() mutable { finish(key, std::move(r)); });
    });
}

/** The slot for key, started on first use and restarted when one of its files changed on disk. */
Slot& entry(const std::string& key, std::vector<std::string> files, const std::function<Result()>& work)
{
    auto& c = cache();
    auto [it, added] = c.entries.try_emplace(key);
    Slot& s = it->second;
    if (added)
    {
        s.files = std::move(files);
        start(key, s, work);
        return s;
    }
    if (!s.loading && Clock::now() - s.checked > std::chrono::milliseconds(1500))
    {
        s.checked = Clock::now();
        if (stamps(s.files) != s.times)
            start(key, s, work);
    }
    return s;
}
} // namespace

render::TexturePtr get(const std::string& path, bool srgb)
{
    if (path.empty())
        return nullptr;
    std::string key = (srgb ? "c|" : "d|") + path;
    return entry(key, {path}, [path, srgb] {
               Result r;
               r.times = stamps({path});
               QImage img = readImage(path, r.error);
               if (!img.isNull())
                   r.image = toTexture(img, srgb);
               return r;
           }).image;
}

render::TexturePtr packed(const std::string& orm, const std::string& occlusion, const std::string& roughness, const std::string& metallic,
                          std::array<int, 3> channels)
{
    if (orm.empty() && occlusion.empty() && roughness.empty() && metallic.empty())
        return nullptr;
    std::vector<std::string> files = {orm, orm.empty() ? occlusion : "", orm.empty() ? roughness : "", orm.empty() ? metallic : ""};
    if (orm.empty())
        channels = {0, 1, 2};
    std::string key = std::format("p{}{}{}|", channels[0], channels[1], channels[2]) + files[0] + "|" + files[1] + "|" + files[2] + "|" +
                      files[3];
    return entry(key, files, [files, channels] { return loadPacked(files, channels); }).image;
}

QImage thumbnail(const std::string& path, int size)
{
    if (path.empty())
        return {};
    std::string key = "t" + std::to_string(size) + "|" + path;
    return entry(key, {path}, [path, size] {
               Result r;
               r.times = stamps({path});
               QImage img = readImage(path, r.error);
               if (!img.isNull())
                   r.thumb = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
               return r;
           }).thumb;
}

void release(const std::string& path, bool srgb)
{
    cache().entries.erase((srgb ? "c|" : "d|") + path);
}

std::string error(const std::string& path)
{
    auto& c = cache();
    for (const char* prefix : {"c|", "d|"})
        if (auto it = c.entries.find(prefix + path); it != c.entries.end() && !it->second.error.empty())
            return it->second.error;
    return {};
}

int addListener(std::function<void()> fn)
{
    int id = cache().nextListener++;
    cache().listeners[id] = std::move(fn);
    return id;
}

void removeListener(int id)
{
    cache().listeners.erase(id);
}
} // namespace lila::ui::textures
