// SPDX-License-Identifier: Apache-2.0
#include "incremental.h"

#include <atomic>
#include <string>

namespace groomlab {

void writeStrands(StrandSet& out, const StrandSet& from, const std::vector<std::uint32_t>& at) {
    if (at.empty()) return;
    std::vector<Vec3f>& positions = out.mutablePositions();
    std::vector<float>& widths = out.mutableWidths();
    const bool hasWidths = widths.size() == out.pointCount() && from.widths().size() == from.pointCount();
    parallelFor(0, at.size(), [&](std::size_t k) {
        const std::size_t to = out.strandBegin(at[k]), src = from.strandBegin(k), n = from.strandPointCount(k);
        for (std::size_t i = 0; i < n; ++i) positions[to + i] = from.positions()[src + i];
        if (hasWidths) {
            for (std::size_t i = 0; i < n; ++i) widths[to + i] = from.widths()[src + i];
        }
    }, 128);
    // Each channel taken for writing only where some strand's values differ.
    for (const std::string& name : from.strandChannelNames()) {
        const std::vector<float>& values = *from.strandChannel(name);
        const std::vector<float>* have = out.strandChannel(name);
        if (!have) continue;
        bool differs = false;
        for (std::size_t k = 0; k < at.size() && !differs; ++k) {
            differs = std::memcmp(&values[k], &(*have)[at[k]], sizeof(float)) != 0;
        }
        if (!differs) continue;
        std::vector<float>& to = *out.mutableStrandChannel(name);
        for (std::size_t k = 0; k < at.size(); ++k) to[at[k]] = values[k];
    }
    for (const std::string& name : from.pointChannelNames()) {
        const std::vector<float>& values = *from.pointChannel(name);
        const std::vector<float>* have = out.pointChannel(name);
        if (!have) continue;
        std::atomic<bool> differs{false};
        parallelFor(0, at.size(), [&](std::size_t k) {
            const std::size_t n = from.strandPointCount(k);
            if (n && std::memcmp(&values[from.strandBegin(k)], &(*have)[out.strandBegin(at[k])], n * sizeof(float)) != 0) {
                differs.store(true, std::memory_order_relaxed);
            }
        }, 128);
        if (!differs.load()) continue;
        std::vector<float>& to = *out.mutablePointChannel(name);
        parallelFor(0, at.size(), [&](std::size_t k) {
            const std::size_t dst = out.strandBegin(at[k]), src = from.strandBegin(k), n = from.strandPointCount(k);
            for (std::size_t i = 0; i < n; ++i) to[dst + i] = values[src + i];
        }, 128);
    }
}

}  // namespace groomlab
