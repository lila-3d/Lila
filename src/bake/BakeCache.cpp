#include "bake/BakeCache.h"

#include "bake/BakeInputs.h"
#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cstring>
#include <list>
#include <mutex>

namespace lila::bake
{
namespace
{
constexpr uint64_t kMul = 0x9e3779b97f4a7c15ull;
/** Large inputs hash as fixed chunks combined in order, so the result never depends on the thread count. */
constexpr size_t kChunk = size_t(1) << 20;
/** Snapshots kept between bakes, by their soups' bytes. */
constexpr size_t kCacheBytes = size_t(3) << 30;

uint64_t mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

uint64_t hashChunk(const unsigned char* p, size_t n, uint64_t seed)
{
    uint64_t a = seed ^ (n * kMul), b = mix(seed + 1);
    size_t i = 0;
    for (; i + 16 <= n; i += 16)
    {
        uint64_t w0, w1;
        std::memcpy(&w0, p + i, 8);
        std::memcpy(&w1, p + i + 8, 8);
        a = (a ^ w0) * kMul;
        a = (a << 31) | (a >> 33);
        b = (b ^ w1) * 0xc2b2ae3d27d4eb4full;
        b = (b << 29) | (b >> 35);
    }
    const size_t rest = n - i;
    uint64_t t0 = 0, t1 = 0;
    std::memcpy(&t0, p + i, std::min<size_t>(rest, 8));
    if (rest > 8)
        std::memcpy(&t1, p + i + 8, rest - 8);
    return mix(a ^ mix(b ^ t0 ^ mix(t1 + rest)));
}

size_t soupBytes(const MeshSoup& s)
{
    size_t n = s.positions.size() * sizeof(glm::vec3) * 2 + s.uvs.size() * sizeof(glm::vec2) + s.colors.size() * sizeof(glm::vec4) +
               s.materials.size() * sizeof(uint32_t);
    for (const auto& c : s.moreColors)
        n += c.size() * sizeof(glm::vec4);
    return n;
}

template <class T> struct Cache
{
    struct Entry
    {
        SnapshotKey key;
        std::shared_ptr<const T> value;
        size_t bytes = 0;
    };
    std::mutex mutex;
    std::list<Entry> entries;
    size_t bytes = 0;

    bool same(const SnapshotKey& a, const SnapshotKey& b) { return a.sourcePtr == b.sourcePtr && a.digest == b.digest; }

    void dropExpired()
    {
        for (auto it = entries.begin(); it != entries.end();)
            if (it->key.source.expired())
            {
                bytes -= it->bytes;
                it = entries.erase(it);
            }
            else
                ++it;
    }

    std::shared_ptr<const T> get(const SnapshotKey& key)
    {
        std::scoped_lock lock(mutex);
        dropExpired();
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (same(it->key, key))
            {
                entries.splice(entries.begin(), entries, it);
                return entries.front().value;
            }
        return nullptr;
    }

    void put(const SnapshotKey& key, std::shared_ptr<const T> value, size_t size)
    {
        if (!key.sourcePtr || size > kCacheBytes)
            return;
        std::scoped_lock lock(mutex);
        dropExpired();
        std::erase_if(entries, [&](const Entry& e) {
            if (!same(e.key, key))
                return false;
            bytes -= e.bytes;
            return true;
        });
        entries.push_front({key, std::move(value), size});
        bytes += size;
        while (bytes > kCacheBytes && entries.size() > 1)
        {
            bytes -= entries.back().bytes;
            entries.pop_back();
        }
    }

    void clear()
    {
        std::scoped_lock lock(mutex);
        entries.clear();
        bytes = 0;
    }
};

Cache<HighInput>& highs()
{
    static Cache<HighInput> c;
    return c;
}

Cache<LowInput>& lows()
{
    static Cache<LowInput> c;
    return c;
}

std::mutex gHookMutex;

std::vector<std::function<void()>>& hooks()
{
    static std::vector<std::function<void()>> h;
    return h;
}
} // namespace

uint64_t hashBytes(const void* data, size_t size, uint64_t seed)
{
    const auto* p = static_cast<const unsigned char*>(data);
    if (size <= kChunk)
        return hashChunk(p, size, seed);
    LILA_ZONE();

    const size_t chunks = (size + kChunk - 1) / kChunk;
    std::vector<uint64_t> part(chunks);
    tbb::parallel_for(size_t(0), chunks, [&](size_t c) {
        const size_t at = c * kChunk;
        part[c] = hashChunk(p + at, std::min(kChunk, size - at), seed + c);
    });
    return hashChunk(reinterpret_cast<const unsigned char*>(part.data()), part.size() * sizeof(uint64_t), seed ^ size);
}

uint64_t soupHash(const MeshSoup& s)
{
    uint64_t h = hashVector(s.positions);
    h = hashVector(s.normals, h);
    h = hashVector(s.uvs, h);
    h = hashVector(s.materials, h);
    h = hashVector(s.colors, h);
    for (size_t k = 0; k < s.moreColors.size(); ++k)
        h = hashVector(s.moreColors[k], h + k);
    for (const std::string& n : s.colorSetNames)
        h = hashBytes(n.data(), n.size(), h);
    return h;
}

SnapshotKey snapshotKey(const std::shared_ptr<const geo::Mesh>& source, const glm::dmat4& world, const std::string& name, const json& modifiers,
                        uint64_t extra)
{
    SnapshotKey k;
    k.source = source;
    k.sourcePtr = source.get();
    uint64_t h = hashBytes(&world, sizeof(world), extra);
    h = hashBytes(name.data(), name.size(), h);
    const std::string m = modifiers.dump();
    k.digest = hashBytes(m.data(), m.size(), h);
    return k;
}

std::shared_ptr<const HighInput> cachedHigh(const SnapshotKey& key) { return highs().get(key); }

void cacheHigh(const SnapshotKey& key, std::shared_ptr<const HighInput> high)
{
    const size_t size = high ? soupBytes(high->soup) : 0;
    highs().put(key, std::move(high), size);
}

std::shared_ptr<const LowInput> cachedLow(const SnapshotKey& key) { return lows().get(key); }

void cacheLow(const SnapshotKey& key, std::shared_ptr<const LowInput> low)
{
    const size_t size = low ? soupBytes(low->soup) * 2 : 0;
    lows().put(key, std::move(low), size);
}

void onClearBakeCaches(std::function<void()> fn)
{
    std::scoped_lock lock(gHookMutex);
    hooks().push_back(std::move(fn));
}

void clearBakeCaches()
{
    highs().clear();
    lows().clear();
    std::vector<std::function<void()>> h;
    {
        std::scoped_lock lock(gHookMutex);
        h = hooks();
    }
    for (const auto& fn : h)
        fn();
}
} // namespace lila::bake
