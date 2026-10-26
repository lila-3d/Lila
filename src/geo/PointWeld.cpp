#include "geo/PointWeld.h"

#include "core/Profile.h"

#include <array>
#include <bit>

namespace lila::geo
{
namespace
{
template <class V> void weld(std::span<const V> pos, UninitVector<Index>& pointOf, UninitVector<glm::dvec3>& points, bool signedZero)
{
    LILA_ZONE();

    using Scalar = typename V::value_type;
    using Bits = std::conditional_t<sizeof(Scalar) == 4, uint32_t, uint64_t>;
    using Key = std::array<Bits, 3>;
    auto clean = [&](V p) { return signedZero ? p : p + V(0); };
    UninitVector<Key> keys(pos.size());
    tbb::parallel_for(size_t(0), pos.size(), [&](size_t i) {
        const V p = clean(pos[i]);
        keys[i] = {std::bit_cast<Bits>(p.x), std::bit_cast<Bits>(p.y), std::bit_cast<Bits>(p.z)};
    });
    UninitVector<Index> first;
    numberDistinct<Key>(keys, pointOf, first);
    points.resize(first.size());
    tbb::parallel_for(size_t(0), first.size(), [&](size_t k) { points[k] = glm::dvec3(clean(pos[first[k]])); });
}
} // namespace

void weldIdentical(std::span<const glm::vec3> pos, UninitVector<Index>& pointOf, UninitVector<glm::dvec3>& points, bool signedZero)
{
    weld(pos, pointOf, points, signedZero);
}

void weldIdentical(std::span<const glm::dvec3> pos, UninitVector<Index>& pointOf, UninitVector<glm::dvec3>& points, bool signedZero)
{
    weld(pos, pointOf, points, signedZero);
}
} // namespace lila::geo
