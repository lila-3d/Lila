// SPDX-License-Identifier: Apache-2.0
#include "groomlab/frame_time.h"

#include <cmath>

namespace groomlab {

namespace {

// The whole rate n an NTSC rate (n * 1000 / 1001) is near, else 0.
double ntscBase(double fps) {
    if (std::abs(fps - std::round(fps)) < 1e-9) return 0.0;  // a whole rate is itself
    const double n = std::round(fps * 1.001);
    return n >= 1.0 && std::abs(fps * 1.001 - n) < 1e-4 * n ? n : 0.0;
}

double goodRate(double fps) { return fps > 0.0 && std::isfinite(fps) ? fps : 24.0; }

}  // namespace

double canonicalFramesPerSecond(double fps) {
    fps = goodRate(fps);
    const double n = ntscBase(fps);
    return n > 0.0 ? n * 1000.0 / 1001.0 : fps;
}

double frameToSeconds(double frame, double fps) {
    fps = goodRate(fps);
    const double n = ntscBase(fps);
    return n > 0.0 ? frame * 1001.0 / (n * 1000.0) : frame / fps;
}

double secondsToFrame(double seconds, double fps) {
    fps = goodRate(fps);
    const double n = ntscBase(fps);
    return n > 0.0 ? seconds * (n * 1000.0) / 1001.0 : seconds * fps;
}

}  // namespace groomlab
