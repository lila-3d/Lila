#include "core/PrefStore.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace lila
{
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr auto kWriteDelay = std::chrono::milliseconds(200);
constexpr auto kPollInterval = std::chrono::milliseconds(250);

enum LayerIndex
{
    Defaults,
    User,
    Workspace,
    LayerCount
};

struct Doc
{
    json data = json::object();
    bool loaded = false;
    bool dirty = false;
    Clock::time_point dirtyAt;
    fs::file_time_type mtime{};
    std::string onDisk; // exact text last read or written, to ignore our own writes
};

struct Layer
{
    fs::path dir;
    std::map<std::string, Doc, std::less<>> docs;
};

std::optional<std::string> readText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeAtomic(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        if (!out.flush())
            return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

json::json_pointer pointer(std::string_view key)
{
    if (key.empty() || key == "/")
        return json::json_pointer();
    return json::json_pointer(key.front() == '/' ? std::string(key) : "/" + std::string(key));
}
} // namespace

struct PrefStore::Impl
{
    mutable std::mutex mutex;
    std::condition_variable cv;
    mutable std::array<Layer, LayerCount> layers;
    std::map<int, ChangeFn> listeners;
    int nextListener = 1;
    std::jthread io;
    bool flushRequested = false;
    std::condition_variable flushed;

    fs::path pathFor(const Layer& layer, std::string_view file) const
    {
        return layer.dir / (std::string(file) + ".json");
    }

    Doc& doc(int layerIndex, std::string_view file) const
    {
        Layer& layer = layers[layerIndex];
        auto it = layer.docs.find(file);
        if (it == layer.docs.end())
            it = layer.docs.emplace(std::string(file), Doc{}).first;
        Doc& d = it->second;
        if (!d.loaded)
            load(layer, file, d);
        return d;
    }

    void load(const Layer& layer, std::string_view file, Doc& d) const
    {
        d.loaded = true;
        if (layer.dir.empty())
            return;
        fs::path path = pathFor(layer, file);
        std::error_code ec;
        d.mtime = fs::last_write_time(path, ec);
        if (auto text = readText(path))
        {
            d.onDisk = *text;
            parseInto(path, *text, d);
        }
    }

    static bool parseInto(const fs::path& path, const std::string& text, Doc& d)
    {
        json parsed = json::parse(text, nullptr, false, true);
        if (parsed.is_discarded() || !parsed.is_object())
        {
            log::warn("prefs: {} is not a valid JSON object, keeping previous values", path.string());
            return false;
        }
        d.data = std::move(parsed);
        return true;
    }

    void notify(const std::string& file)
    {
        mainq::post([this, file] {
            std::map<int, ChangeFn> copy;
            {
                std::scoped_lock lock(mutex);
                copy = listeners;
            }
            for (auto& [id, fn] : copy)
                fn(file);
        });
    }

    void ioLoop(std::stop_token stop)
    {
        LILA_THREAD_NAME("Prefs IO");
        std::unique_lock lock(mutex);
        while (!stop.stop_requested())
        {
            cv.wait_for(lock, kPollInterval);
            writeDirty(lock, flushRequested);
            pollExternal(lock);
            if (flushRequested)
            {
                flushRequested = false;
                flushed.notify_all();
            }
        }
        writeDirty(lock, true);
    }

    void writeDirty(std::unique_lock<std::mutex>& lock, bool force)
    {
        LILA_ZONE();

        struct Pending
        {
            fs::path path;
            std::string text;
            Doc* doc;
        };
        std::vector<Pending> pending;
        auto now = Clock::now();
        for (int li = User; li < LayerCount; ++li)
        {
            Layer& layer = layers[li];
            if (layer.dir.empty())
                continue;
            for (auto& [file, d] : layer.docs)
            {
                if (!d.dirty || (!force && now - d.dirtyAt < kWriteDelay))
                    continue;
                d.dirty = false;
                std::string text = d.data.dump(2) + "\n";
                d.onDisk = text;
                pending.push_back({pathFor(layer, file), std::move(text), &d});
            }
        }
        if (pending.empty())
            return;

        lock.unlock();
        for (Pending& p : pending)
            if (!writeAtomic(p.path, p.text))
                log::error("prefs: failed to write {}", p.path.string());
        lock.lock();

        for (Pending& p : pending)
        {
            std::error_code ec;
            p.doc->mtime = fs::last_write_time(p.path, ec);
        }
    }

    void pollExternal(std::unique_lock<std::mutex>& lock)
    {
        LILA_ZONE();

        std::vector<std::string> changed;
        for (int li = User; li < LayerCount; ++li)
        {
            Layer& layer = layers[li];
            if (layer.dir.empty())
                continue;
            for (auto& [file, d] : layer.docs)
            {
                if (d.dirty)
                    continue;
                fs::path path = pathFor(layer, file);
                std::error_code ec;
                auto mtime = fs::last_write_time(path, ec);
                if (ec || mtime == d.mtime)
                    continue;
                d.mtime = mtime;
                auto text = readText(path);
                if (!text || *text == d.onDisk)
                    continue;
                d.onDisk = *text;
                if (parseInto(path, *text, d))
                    changed.push_back(file);
            }
        }

        lock.unlock();
        for (const std::string& file : changed)
        {
            log::info("prefs: reloaded {}", file);
            notify(file);
        }
        lock.lock();
    }
};

PrefStore::PrefStore() : m(std::make_unique<Impl>())
{
    m->layers[Defaults].dir = paths::resourcesDir() / "defaults";
    m->layers[User].dir = paths::userConfigDir() / "prefs";
}

PrefStore::~PrefStore()
{
    stop();
}

PrefStore& PrefStore::get()
{
    static PrefStore store;
    return store;
}

void PrefStore::start()
{
    if (m->io.joinable())
        return;
    m->io = std::jthread([this](std::stop_token st) { m->ioLoop(st); });
}

void PrefStore::stop()
{
    if (!m->io.joinable())
        return;
    m->io.request_stop();
    m->cv.notify_all();
    m->io.join();
}

void PrefStore::setWorkspaceDir(const fs::path& lilaDir)
{
    {
        std::unique_lock lock(m->mutex);
        m->writeDirty(lock, true);
        m->layers[Workspace] = Layer{lilaDir, {}};
    }
}

json PrefStore::value(std::string_view file, std::string_view key, const json& fallback) const
{
    std::scoped_lock lock(m->mutex);
    auto ptr = pointer(key);
    for (int li = LayerCount - 1; li >= 0; --li)
    {
        if (m->layers[li].dir.empty())
            continue;
        const Doc& d = m->doc(li, file);
        if (d.data.contains(ptr))
            return d.data[ptr];
    }
    return fallback;
}

json PrefStore::merged(std::string_view file) const
{
    std::scoped_lock lock(m->mutex);
    json out = json::object();
    for (int li = 0; li < LayerCount; ++li)
        if (!m->layers[li].dir.empty())
            out.merge_patch(m->doc(li, file).data);
    return out;
}

void PrefStore::set(std::string_view file, std::string_view key, json value, PrefScope scope)
{
    {
        std::scoped_lock lock(m->mutex);
        int li = scope == PrefScope::Workspace && !m->layers[Workspace].dir.empty() ? Workspace : User;
        Doc& d = m->doc(li, file);
        auto ptr = pointer(key);
        if (d.data.contains(ptr) && d.data[ptr] == value)
            return;
        d.data[ptr] = std::move(value);
        d.dirty = true;
        d.dirtyAt = Clock::now();
    }
    m->cv.notify_all();
    m->notify(std::string(file));
}

void PrefStore::remove(std::string_view file, std::string_view key, PrefScope scope)
{
    {
        std::scoped_lock lock(m->mutex);
        int li = scope == PrefScope::Workspace && !m->layers[Workspace].dir.empty() ? Workspace : User;
        Doc& d = m->doc(li, file);
        auto ptr = pointer(key);
        if (!d.data.contains(ptr))
            return;
        d.data[ptr.parent_pointer()].erase(ptr.back());
        d.dirty = true;
        d.dirtyAt = Clock::now();
    }
    m->cv.notify_all();
    m->notify(std::string(file));
}

void PrefStore::flush()
{
    std::unique_lock lock(m->mutex);
    if (!m->io.joinable())
    {
        m->writeDirty(lock, true);
        return;
    }
    m->flushRequested = true;
    m->cv.notify_all();
    m->flushed.wait(lock, [this] { return !m->flushRequested; });
}

int PrefStore::onChanged(ChangeFn fn)
{
    std::scoped_lock lock(m->mutex);
    m->listeners.emplace(m->nextListener, std::move(fn));
    return m->nextListener++;
}

void PrefStore::removeOnChanged(int id)
{
    std::scoped_lock lock(m->mutex);
    m->listeners.erase(id);
}

fs::path PrefStore::userDir() const
{
    return m->layers[User].dir;
}
} // namespace lila
