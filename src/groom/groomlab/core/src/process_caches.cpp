// SPDX-License-Identifier: Apache-2.0
#include "groomlab/process_caches.h"

#include <mutex>
#include <string>
#include <vector>

#include "groomlab/strand_set.h"

namespace groomlab {

namespace {

struct Registered {
    std::string name;
    std::function<void()> release;
    std::function<std::size_t()> bytes;
};

struct Registry {
    std::mutex mutex;
    std::vector<Registered> caches;
};

Registry& registry() {
    static Registry* r = new Registry;  // never destroyed: caches register from static locals
    return *r;
}

// The pools of the element types StrandSet arrays hold.
template <class T>
void releasePool(std::size_t* bytes) {
    if (bytes) {
        *bytes += detail::ArrayPool<T>::instance().bytes();
    } else {
        detail::ArrayPool<T>::instance().clear();
    }
}

void pools(std::size_t* bytes) {
    releasePool<Vec3f>(bytes);
    releasePool<float>(bytes);
    releasePool<StrandId>(bytes);
    releasePool<StrandRoot>(bytes);
    releasePool<std::size_t>(bytes);
}

}  // namespace

void registerProcessCache(const char* name, std::function<void()> release, std::function<std::size_t()> bytes) {
    Registry& r = registry();
    const std::lock_guard<std::mutex> lock(r.mutex);
    r.caches.push_back({name ? name : "", std::move(release), std::move(bytes)});
}

void releaseProcessCaches() {
    std::vector<Registered> caches;
    {
        Registry& r = registry();
        const std::lock_guard<std::mutex> lock(r.mutex);
        caches = r.caches;
    }
    for (const Registered& c : caches) {
        if (c.release) c.release();
    }
    pools(nullptr);
}

void releaseArrayPools() { pools(nullptr); }

std::size_t processCacheBytes() {
    std::vector<Registered> caches;
    {
        Registry& r = registry();
        const std::lock_guard<std::mutex> lock(r.mutex);
        caches = r.caches;
    }
    std::size_t total = 0;
    for (const Registered& c : caches) {
        if (c.bytes) total += c.bytes();
    }
    pools(&total);
    return total;
}

}  // namespace groomlab
