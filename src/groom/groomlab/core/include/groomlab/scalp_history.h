// SPDX-License-Identifier: Apache-2.0
#pragma once

// The scalp's earlier frames for Jiggle (graph.h ScalpHistory), kept by time for any host: a
// frame read once (or seen as the current frame) is not read again, so playing forward reads
// at most one new frame (none: the frame before was the current one), and a scrub reuses what
// it has. Hosts say how to read a frame (Maya: the inMesh plug in another time's context;
// GroomScene: its scalp cache) and how times map to keys.

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "groomlab/graph.h"

namespace groomlab {

class ScalpHistorySampler {
public:
    // The history at `nowKey` (a host's key for the evaluated time, e.g. microseconds) of the
    // frames at `keys` (frames before now, nearest first) `step` seconds apart, `current` being the
    // scalp's points now. `read(k)` reads the points of frame keys[k] (a host returns `current`'s
    // size, or the whole history is still for that frame). Frames this sampler holds are reused,
    // unless `reread` (the animation may have changed: every frame is read again, and when one
    // reads differently the other remembered frames are dropped); the current frame is
    // remembered, and when it reads differently from what was remembered for that time the
    // animation changed and everything is dropped. Frames past `keys.size() + 48` frames from
    // now are forgotten (not the ones asked for). The same request with nothing changed returns
    // the same history. The last `loopSeconds.size()` keys are a looping Jiggle's frames
    // (ScalpHistory's loop frames, at those times), the ones before them the frames before now.
    ScalpHistoryPtr history(std::int64_t nowKey, const std::vector<std::int64_t>& keys, double step,
                            const std::vector<Vec3f>& current, const std::function<std::vector<Vec3f>(std::size_t k)>& read,
                            bool reread, const std::vector<double>& loopSeconds = {});
    void clear();
    bool empty() const { return points_.empty(); }
    // Frames read so far (stats, tests: Maya's inMesh evaluations in other contexts).
    std::size_t reads() const { return reads_; }

private:
    std::map<std::int64_t, std::vector<Vec3f>> points_;  // by key
    std::uint64_t generation_ = 0;                        // bumped whenever an entry changes
    std::size_t reads_ = 0;
    ScalpHistoryPtr last_;  // the last history made, reused while nothing it came from changed
    std::int64_t lastNow_ = 0;
    std::vector<std::int64_t> lastKeys_;
    std::uint64_t lastGeneration_ = 0;
};

}  // namespace groomlab
