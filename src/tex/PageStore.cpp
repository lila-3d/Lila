#include "tex/PageStore.h"

#include "core/Events.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "io/SceneFile.h"
#include "tex/Plan.h"

#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace lila::tex::store
{
namespace
{
constexpr uint32_t kMagic = 0x3147504cu; // "LPG1"

struct State
{
    std::mutex mutex;
    fs::path active;
    std::set<std::string> referenced;
    SaveStats stats;
    SaveStats last;
};

State& state()
{
    static State s;
    return s;
}

std::string hex(uint64_t v)
{
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

/** A file's path under the store, with forward slashes as chunk names are written. */
std::string relKey(const fs::path& file, const fs::path& root)
{
    std::string r = paths::toUtf8(file.lexically_relative(root));
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

void begin(const fs::path& scene)
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    s.active = storeFor(scene);
    s.referenced.clear();
    s.stats = {};
}

/** After a save: drops the chunk files nothing refers to any more, writes the index, and closes the store. */
void finish()
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    if (s.active.empty())
        return;
    std::error_code ec;
    if (fs::is_directory(s.active, ec))
    {
        std::vector<fs::path> stale;
        for (const auto& e : fs::recursive_directory_iterator(s.active, ec))
            if (e.is_regular_file(ec) && e.path().extension() == ".lpg" &&
                !s.referenced.contains(relKey(e.path(), s.active)))
                stale.push_back(e.path());
        for (const fs::path& p : stale)
            if (fs::remove(p, ec))
                ++s.stats.removed;
        if (s.referenced.empty() && s.stats.written == 0)
        {
            // Nothing refers to the store: no folder left behind.
            fs::remove(s.active / "index.json", ec);
            for (int pass = 0; pass < 8; ++pass)
                for (const auto& e : fs::recursive_directory_iterator(s.active, ec))
                    if (e.is_directory(ec) && fs::is_empty(e.path(), ec))
                        fs::remove(e.path(), ec);
            if (fs::is_empty(s.active, ec))
                fs::remove(s.active, ec);
        }
        else
        {
            json index = {{"format", "lilatex"}, {"version", 1}, {"chunks", json::array()}};
            for (const std::string& r : s.referenced)
                index["chunks"].push_back(r);
            std::ofstream(s.active / "index.json", std::ios::binary) << index.dump(1);
        }
    }
    s.stats.kept = s.referenced.size() >= s.stats.written ? s.referenced.size() - s.stats.written : 0;
    s.last = s.stats;
    s.active.clear();
    s.referenced.clear();
}
} // namespace

fs::path storeFor(const fs::path& scene)
{
    fs::path p = scene;
    p += ".lilatex";
    return p;
}

fs::path activeStore()
{
    std::scoped_lock lock(state().mutex);
    return state().active;
}

std::string putChunk(const std::string& keyPath, std::span<const uint32_t> pages, std::span<const uint8_t> raw, size_t pageBytes)
{
    if (raw.size() != pages.size() * pageBytes)
        throw std::invalid_argument("page store: chunk texels don't match its pages");
    uint64_t h = hash64(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
    h = hash64(std::string_view(reinterpret_cast<const char*>(pages.data()), pages.size() * sizeof(uint32_t)), h);
    const std::string rel = keyPath + "-" + hex(h) + ".lpg";
    State& s = state();
    fs::path dir;
    {
        std::scoped_lock lock(s.mutex);
        if (s.active.empty())
            throw std::runtime_error("page store: no save in progress");
        dir = s.active;
        s.referenced.insert(rel);
    }
    const fs::path file = dir / paths::fromUtf8(rel);
    std::error_code ec;
    if (fs::exists(file, ec))
        return rel;
    // Each page packed on its own: a reader decompresses only what it needs.
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) {
        const size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &v, 4);
    };
    put32(kMagic);
    put32(uint32_t(pages.size()));
    put32(uint32_t(pageBytes));
    std::vector<uint8_t> packed(ZSTD_compressBound(pageBytes));
    for (size_t i = 0; i < pages.size(); ++i)
    {
        const size_t n = ZSTD_compress(packed.data(), packed.size(), raw.data() + i * pageBytes, pageBytes, 3);
        if (ZSTD_isError(n))
            throw std::runtime_error(std::string("page store: ") + ZSTD_getErrorName(n));
        put32(pages[i]);
        put32(uint32_t(n));
        out.insert(out.end(), packed.begin(), packed.begin() + std::ptrdiff_t(n));
    }
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = fs::path(file).concat(".tmp");
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size())))
            throw std::runtime_error("page store: can't write " + paths::toUtf8(file));
    }
    fs::rename(tmp, file, ec);
    if (ec)
        throw std::runtime_error("page store: can't write " + paths::toUtf8(file) + ": " + ec.message());
    std::scoped_lock lock(s.mutex);
    ++s.stats.written;
    s.stats.bytesWritten += out.size();
    return rel;
}

void keepChunk(const std::string& relPath)
{
    std::scoped_lock lock(state().mutex);
    state().referenced.insert(relPath);
}

std::vector<uint8_t> readChunk(const fs::path& file, size_t pageBytes)
{
    std::ifstream f(file, std::ios::binary);
    if (!f)
        throw std::runtime_error("page store: missing " + paths::toUtf8(file));
    std::vector<uint8_t> in((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t at = 0;
    auto get32 = [&]() {
        if (at + 4 > in.size())
            throw std::runtime_error("page store: truncated " + paths::toUtf8(file));
        uint32_t v;
        std::memcpy(&v, in.data() + at, 4);
        at += 4;
        return v;
    };
    if (get32() != kMagic)
        throw std::runtime_error("page store: not a page chunk: " + paths::toUtf8(file));
    const uint32_t count = get32();
    if (get32() != pageBytes)
        throw std::runtime_error("page store: unexpected page size in " + paths::toUtf8(file));
    std::vector<uint8_t> out(size_t(count) * pageBytes);
    for (uint32_t i = 0; i < count; ++i)
    {
        get32(); // page index, for readers that want one page
        const uint32_t n = get32();
        if (at + n > in.size())
            throw std::runtime_error("page store: truncated " + paths::toUtf8(file));
        const size_t got = ZSTD_decompress(out.data() + size_t(i) * pageBytes, pageBytes, in.data() + at, n);
        if (ZSTD_isError(got) || got != pageBytes)
            throw std::runtime_error("page store: corrupt " + paths::toUtf8(file));
        at += n;
    }
    return out;
}

fs::path resolve(const std::string& storeName, const fs::path& storeAbs, const std::string& relPath)
{
    std::error_code ec;
    const fs::path scene = io::currentPath();
    if (!scene.empty())
    {
        const fs::path here = scene.parent_path() / paths::fromUtf8(storeName) / paths::fromUtf8(relPath);
        if (fs::exists(here, ec))
            return here;
        const fs::path own = storeFor(scene) / paths::fromUtf8(relPath);
        if (fs::exists(own, ec))
            return own;
    }
    if (!storeAbs.empty())
    {
        const fs::path there = storeAbs / paths::fromUtf8(relPath);
        if (fs::exists(there, ec))
            return there;
    }
    return {};
}

SaveStats lastSave()
{
    std::scoped_lock lock(state().mutex);
    return state().last;
}

void install()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    events::subscribe(events::Event::SceneBeforeSave, [](const events::EventData& e) { begin(e.path); });
    events::subscribe(events::Event::SceneSaved, [](const events::EventData&) { finish(); });
}
} // namespace lila::tex::store
