// SPDX-License-Identifier: Apache-2.0
#include "groomlab/scalp_history.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace groomlab {

void ScalpHistorySampler::clear() {
    points_.clear();
    last_.reset();
    ++generation_;
}

ScalpHistoryPtr ScalpHistorySampler::history(std::int64_t nowKey, const std::vector<std::int64_t>& keys, double step,
                                             const std::vector<Vec3f>& current,
                                             const std::function<std::vector<Vec3f>(std::size_t k)>& read, bool reread,
                                             const std::vector<double>& loopSeconds) {
    if (keys.empty() || current.empty()) return nullptr;

    // The current frame: remembered, and a check that the animation is what it was.
    auto here = points_.find(nowKey);
    if (here != points_.end() && here->second != current) clear();
    if (!points_.empty() && points_.begin()->second.size() != current.size()) clear();
    if (points_.emplace(nowKey, current).second) ++generation_;

    bool animationChanged = false;
    for (std::size_t k = 0; k < keys.size(); ++k) {
        auto found = points_.find(keys[k]);
        if (found != points_.end() && !reread) continue;
        std::vector<Vec3f> points = read(k);
        ++reads_;
        if (points.size() != current.size()) points = current;  // another topology then: no motion from it
        if (found == points_.end()) {
            points_.emplace(keys[k], std::move(points));
            ++generation_;
        } else if (found->second != points) {
            found->second = std::move(points);
            ++generation_;
            animationChanged = true;
        }
    }
    // A remembered frame read differently: the animation changed, so the other remembered frames
    // may be stale too. Keep only what was just read.
    if (animationChanged) {
        for (auto it = points_.begin(); it != points_.end();) {
            const bool used = it->first == nowKey || std::find(keys.begin(), keys.end(), it->first) != keys.end();
            it = used ? std::next(it) : points_.erase(it);
        }
    }
    // Forget frames far from now (a long scrub would otherwise keep them all), but those asked for
    // (a loop's, from the end of its range).
    const auto window = static_cast<std::int64_t>(std::llround((static_cast<double>(keys.size()) + 48) * step * 1e6));
    for (auto it = points_.begin(); it != points_.end();) {
        const bool far = std::llabs(it->first - nowKey) > window && std::find(keys.begin(), keys.end(), it->first) == keys.end();
        it = far ? points_.erase(it) : std::next(it);
    }

    if (last_ && lastNow_ == nowKey && lastKeys_ == keys && lastGeneration_ == generation_ && last_->loopSeconds == loopSeconds) {
        return last_;
    }
    std::vector<std::vector<Vec3f>> past;
    past.reserve(keys.size());
    for (const std::int64_t k : keys) past.push_back(points_.at(k));
    last_ = ScalpHistory::make(step, std::move(past), loopSeconds);
    lastNow_ = nowKey;
    lastKeys_ = keys;
    lastGeneration_ = generation_;
    return last_;
}

}  // namespace groomlab
