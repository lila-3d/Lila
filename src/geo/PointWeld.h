#pragma once

#include "core/Export.h"
#include "core/UninitVector.h"
#include "geo/Mesh.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_scan.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <span>
#include <type_traits>

namespace lila::geo
{
/**
 * Points for a list of corner positions, identical ones (bit for bit) sharing one: pointOf[i] is corner i's point and
 * points holds each position once, in order of first use, as adding them one at a time would. With signedZero off,
 * -0 and +0 count as the same.
 */
LILA_SCENE_API void weldIdentical(std::span<const glm::vec3> pos, UninitVector<Index>& pointOf, UninitVector<glm::dvec3>& points,
                                  bool signedZero = false);
LILA_SCENE_API void weldIdentical(std::span<const glm::dvec3> pos, UninitVector<Index>& pointOf, UninitVector<glm::dvec3>& points,
                                  bool signedZero = false);

/**
 * Numbers keys that are the same byte for byte, in parallel: idOf[i] is key i's number, counting distinct keys in order of
 * first appearance, and first[n] is where number n first appears. Key must be trivially copyable with no padding.
 */
template <class Key> void numberDistinct(std::span<const Key> keys, UninitVector<Index>& idOf, UninitVector<Index>& first)
{
    static_assert(std::is_trivially_copyable_v<Key> && std::has_unique_object_representations_v<Key>);
    struct Item
    {
        Key key;
        Index at;
    };
    const size_t n = keys.size();
    // Keys go to buckets by hash, so equal keys share a bucket, each sorted on its own.
    constexpr int kBucketBits = 12;
    constexpr size_t kBuckets = size_t(1) << kBucketBits;
    auto bucketOf = [](const Key& k) {
        unsigned char b[sizeof(Key)];
        std::memcpy(b, &k, sizeof(Key));
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(Key); i += 4)
        {
            uint32_t w = 0;
            std::memcpy(&w, b + i, std::min<size_t>(4, sizeof(Key) - i));
            h = (h ^ w) * 0x100000001B3ull;
        }
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        return size_t(h >> (64 - kBucketBits));
    };
    const size_t blocks = std::clamp<size_t>(n / 65536, 1, 512);
    auto blockRange = [&](size_t b) { return std::pair(n * b / blocks, n * (b + 1) / blocks); };
    UninitVector<uint32_t> bucket(n);
    std::vector<size_t> count(blocks * kBuckets, 0); // block-major
    tbb::parallel_for(size_t(0), blocks, [&](size_t b) {
        auto [lo, hi] = blockRange(b);
        size_t* c = count.data() + b * kBuckets;
        for (size_t i = lo; i < hi; ++i)
            ++c[bucket[i] = uint32_t(bucketOf(keys[i]))];
    });
    // Offsets bucket-major, then block, so each bucket holds its keys in order.
    std::vector<size_t> start(blocks * kBuckets), bucketStart(kBuckets + 1, 0);
    size_t at = 0;
    for (size_t k = 0; k < kBuckets; ++k)
    {
        bucketStart[k] = at;
        for (size_t b = 0; b < blocks; ++b)
        {
            start[b * kBuckets + k] = at;
            at += count[b * kBuckets + k];
        }
    }
    bucketStart[kBuckets] = at;
    UninitVector<Item> items(n);
    tbb::parallel_for(size_t(0), blocks, [&](size_t b) {
        auto [lo, hi] = blockRange(b);
        size_t* s = start.data() + b * kBuckets;
        for (size_t i = lo; i < hi; ++i)
            items[s[bucket[i]]++] = {keys[i], Index(i)};
    });

    // In each bucket a run of one key starts with its first appearance.
    UninitVector<Index> firstOf(n), number(n);
    auto less = [](const Item& a, const Item& b) {
        const int c = std::memcmp(&a.key, &b.key, sizeof(Key));
        return c != 0 ? c < 0 : a.at < b.at;
    };
    auto same = [](const Item& a, const Item& b) { return std::memcmp(&a.key, &b.key, sizeof(Key)) == 0; };
    tbb::parallel_for(size_t(0), kBuckets, [&](size_t k) {
        Item* lo = items.data() + bucketStart[k];
        Item* hi = items.data() + bucketStart[k + 1];
        std::sort(lo, hi, less);
        for (Item* run = lo; run < hi;)
        {
            Item* end = run + 1;
            while (end < hi && same(*end, *run))
                ++end;
            for (Item* j = run; j < end; ++j)
                firstOf[j->at] = run->at;
            run = end;
        }
    });
    const Index total = tbb::parallel_scan(
        tbb::blocked_range<size_t>(0, n), Index(0),
        [&](const tbb::blocked_range<size_t>& r, Index sum, bool final) {
            for (size_t i = r.begin(); i < r.end(); ++i)
            {
                if (final)
                    number[i] = sum;
                sum += firstOf[i] == Index(i) ? 1 : 0;
            }
            return sum;
        },
        std::plus<Index>());
    idOf.resize(n);
    first.resize(total);
    tbb::parallel_for(size_t(0), n, [&](size_t i) {
        idOf[i] = number[firstOf[i]];
        if (firstOf[i] == Index(i))
            first[number[i]] = Index(i);
    });
}
} // namespace lila::geo
