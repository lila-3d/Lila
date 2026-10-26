#pragma once

#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace lila
{
/**
 * std::allocator, except that resize() and the count constructor leave new elements uninitialised instead of
 * value-initialising them, for trivial types and for plain aggregates (whose default member initialisers are skipped
 * too). For arrays of millions of elements that are filled straight after, often in parallel: initialising first is a
 * serial pass over memory that is about to be overwritten.
 */
template <class T>
struct UninitAllocator : std::allocator<T>
{
    using value_type = T;
    template <class U>
    struct rebind
    {
        using other = UninitAllocator<U>;
    };

    UninitAllocator() noexcept = default;
    template <class U>
    UninitAllocator(const UninitAllocator<U>&) noexcept
    {
    }

    template <class U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible_v<U>)
    {
        // Aggregates of trivially destructible members are implicit-lifetime types: the allocation already made them.
        if constexpr (!std::is_trivially_default_constructible_v<U> && !(std::is_aggregate_v<U> && std::is_trivially_destructible_v<U>))
            ::new (static_cast<void*>(p)) U;
    }
    template <class U, class... Args>
    void construct(U* p, Args&&... args)
    {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <class U>
    bool operator==(const UninitAllocator<U>&) const noexcept
    {
        return true;
    }
};

template <class T>
using UninitVector = std::vector<T, UninitAllocator<T>>;
} // namespace lila
