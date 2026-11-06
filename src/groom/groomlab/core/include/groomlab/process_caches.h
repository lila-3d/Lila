// SPDX-License-Identifier: Apache-2.0
#pragma once

// Caches kept for the whole process, not by a session: a distance field per collider surface
// (sharedDistanceField), surface graphs, subdivision plans, scalp mirrors and pieces, referenced
// grooms, decoded textures, the large arrays StrandSets let go of (detail::ArrayPool). They speed
// up the next evaluation of the same scene and outlive every session that used them, so a host
// lets them go when its scene closes (Maya: before a new or opened scene); what a session still
// holds stays alive with it.

#include <cstddef>
#include <functional>

namespace groomlab {

// A cache's release, registered by the cache itself on its first use (once). Thread-safe.
// `bytes` (optional) tells what it holds now, for processCacheBytes.
void registerProcessCache(const char* name, std::function<void()> release, std::function<std::size_t()> bytes = {});

// Lets every registered cache (and the array pools) go. Thread-safe; an evaluation running
// meanwhile keeps what it took and makes again what it asks for next.
void releaseProcessCaches();
// The array pools alone (detail::ArrayPool: the large arrays freed last, kept for the next of
// their size).
void releaseArrayPools();

// What the caches that can tell hold, in bytes (an estimate: a distance field's bricks, the
// pools' arrays, the decoded textures).
std::size_t processCacheBytes();

}  // namespace groomlab
