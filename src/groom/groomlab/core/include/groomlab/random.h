// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "groomlab/math.h"

namespace groomlab {

// SplitMix64 finalizer: a fast, well-mixed 64-bit hash. Used for stable
// strand IDs and per-strand seeds so results never depend on thread order.
constexpr std::uint64_t hash64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

constexpr std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) {
    return hash64(seed ^ (value + 0x9E3779B97F4A7C15ull + (seed << 6) + (seed >> 2)));
}

// Maps a hash to [0, 1) using its top 24 bits (exact in float).
constexpr float unitFloat(std::uint64_t h) {
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// Preview density filter (PG-4): a strand survives when its hashed ID falls
// below the density. Lowering density removes strands, never moves survivors.
constexpr bool keepAtDensity(std::uint64_t strandId, float density) {
    return unitFloat(hash64(strandId)) < density;
}

// Small deterministic generator, seeded per strand from its stable ID.
class Rng {
public:
    constexpr explicit Rng(std::uint64_t seed) : state_(hash64(seed)) {}

    constexpr std::uint64_t nextU64() {
        state_ += 0x9E3779B97F4A7C15ull;
        return hash64(state_);
    }

    // Uniform in [0, 1).
    constexpr float nextFloat() { return unitFloat(nextU64()); }

    // Uniform in [lo, hi).
    constexpr float uniform(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }

private:
    std::uint64_t state_;
};

// A random offset in [0, 1000) on each axis (Noise and Flyaway walk their strand-space noise
// from it), drawn z first, then y, then x, one statement each: C++ leaves the order of a call's
// arguments to the compiler, so `Vec3f(rng.uniform(..), rng.uniform(..), rng.uniform(..))` gave
// different grooms under different compilers. This is the order MSVC gave, kept everywhere.
inline Vec3f randomOffsetZYX(Rng& rng) {
    const float z = rng.uniform(0, 1000);
    const float y = rng.uniform(0, 1000);
    const float x = rng.uniform(0, 1000);
    return Vec3f(x, y, z);
}

// A float's bits, as they are (-0 and 0 differ).
inline std::uint32_t floatBits(float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bits;
}
// A float's bits with -0 taken as 0: a value's key, where -0 and 0 are the same number.
inline std::uint32_t floatBitsZero(float f) {
    if (f == 0.0f) f = 0.0f;
    return floatBits(f);
}

// Mixes a float's bits into a hash.
inline std::uint64_t hashFloat(std::uint64_t h, float f) { return hashCombine(h, floatBits(f)); }

// Mixes a double's bits into a hash.
inline std::uint64_t hashDouble(std::uint64_t h, double d) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof bits);
    return hashCombine(h, bits);
}

// Mixes a string into a hash: its length first, so consecutive strings can't
// run together ("ab" + "c" hashes differently from "a" + "bc").
inline std::uint64_t hashString(std::uint64_t h, const std::string& s) {
    h = hashCombine(h, s.size());
    for (const unsigned char c : s) h = hashCombine(h, c);
    return h;
}

// A string's characters alone, without hashString's length: for keys stored as they are (a
// cards bake's "Strands=" stamp) or always followed by something else.
inline std::uint64_t hashChars(std::uint64_t h, const std::string& s) {
    for (const unsigned char c : s) h = hashCombine(h, c);
    return h;
}

}  // namespace groomlab
