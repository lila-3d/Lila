#pragma once

#include <doctest/doctest.h>

#include "render/Renderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace stylisedtest
{
/** Starts the renderer; false (with a note naming the skipped tests) when this machine has no GPU. */
inline bool gpu(const char* tests)
{
    lila::render::Renderer& r = lila::render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for " << tests << " tests");
    return false;
}

inline int maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    int m = 0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}
} // namespace stylisedtest
