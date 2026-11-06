// SPDX-License-Identifier: Apache-2.0
#include "groomlab/transfer_markers_edit.h"

#include <algorithm>
#include <stdexcept>

#include "groomlab/scalp_mesh.h"

namespace groomlab {

// ---- MarkerLists ----------------------------------------------------------------------------------

const MarkerPoint& MarkerLists::point(MarkerSide side, std::size_t i) const {
    const auto& list = points(side);
    if (i >= list.size()) throw std::out_of_range("no marker point " + std::to_string(i + 1) + " on that side");
    return list[i];
}

std::size_t MarkerLists::add(MarkerSide side, const MarkerPoint& p) {
    auto& list = sides[index(side)];
    MarkerPoint q = p;
    q.group = 0;
    list.push_back(q);
    return list.size() - 1;
}

std::size_t MarkerLists::addPair(MarkerSide side, const MarkerPoint& p, const MarkerPoint& mirror, std::uint32_t group) {
    auto& list = sides[index(side)];
    MarkerPoint a = p, b = mirror;
    a.group = b.group = group;
    list.push_back(a);
    list.push_back(b);
    return list.size() - 2;
}

std::vector<std::size_t> MarkerLists::removalSet(MarkerSide side, std::size_t i) const {
    const auto& list = points(side);
    const std::uint32_t group = point(side, i).group;
    std::vector<std::size_t> out;
    for (std::size_t j = 0; j < list.size(); ++j) {
        if (j == i || (group != 0 && list[j].group == group)) out.push_back(j);
    }
    return out;
}

std::vector<std::size_t> MarkerLists::remove(MarkerSide side, std::size_t i) {
    const std::vector<std::size_t> gone = removalSet(side, i);
    for (auto& list : sides) {
        for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
            if (*it < list.size()) list.erase(list.begin() + static_cast<std::ptrdiff_t>(*it));
        }
    }
    return gone;
}

void MarkerLists::set(MarkerSide side, std::size_t i, const MarkerPoint& p) {
    const std::uint32_t group = point(side, i).group;
    MarkerPoint q = p;
    q.group = group;
    sides[index(side)][i] = q;
}

std::string MarkerLists::summary(const char* none) const {
    const std::size_t n = pairCount(), pending = pendingCount();
    if (n == 0 && pending == 0) return none;
    std::string out = std::to_string(n) + (n == 1 ? " pair" : " pairs");
    if (pending) out += ", " + std::to_string(pending) + " unmatched";
    return out;
}

// ---- TransferMarkerEditor ---------------------------------------------------------------------------

void TransferMarkerEditor::reset(const std::vector<TransferMarker>& pairs) {
    state_ = State{};
    for (const TransferMarker& m : pairs) {
        state_.sides[0].push_back({m.oldRoot, m.oldPoint, 0});
        state_.sides[1].push_back({m.newRoot, m.newPoint, 0});
    }
    undo_.clear();
    redo_.clear();
    ++revision_;
}

void TransferMarkerEditor::setPairs(const std::vector<TransferMarker>& pairs) {
    record();
    State next;
    for (const TransferMarker& m : pairs) {
        next.sides[0].push_back({m.oldRoot, m.oldPoint, 0});
        next.sides[1].push_back({m.newRoot, m.newPoint, 0});
    }
    state_ = std::move(next);
}

const MarkerPoint& TransferMarkerEditor::point(MarkerSide side, std::size_t i) const { return state_.point(side, i); }

void TransferMarkerEditor::record() {
    undo_.push_back(state_);
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
    redo_.clear();
    ++revision_;
}

std::size_t TransferMarkerEditor::add(MarkerSide side, const MarkerPoint& p) {
    record();
    return state_.add(side, p);
}

std::size_t TransferMarkerEditor::addMirrored(MarkerSide side, const MarkerPoint& p, const MarkerPoint& mirror,
                                              float mergeDistance) {
    if ((mirror.point - p.point).length() <= mergeDistance) return add(side, p);
    record();
    return state_.addPair(side, p, mirror, nextGroup_++);
}

std::vector<std::size_t> TransferMarkerEditor::removalSet(MarkerSide side, std::size_t i) const {
    return state_.removalSet(side, i);
}

std::vector<std::size_t> TransferMarkerEditor::remove(MarkerSide side, std::size_t i) {
    (void)state_.point(side, i);  // throws before anything is recorded
    record();
    return state_.remove(side, i);
}

void TransferMarkerEditor::move(MarkerSide side, std::size_t i, const MarkerPoint& p, bool newStep) {
    (void)state_.point(side, i);
    if (newStep || undo_.empty()) {
        record();
    } else {
        redo_.clear();
        ++revision_;
    }
    state_.set(side, i, p);
}

void TransferMarkerEditor::clear() {
    if (state_.empty()) return;
    record();
    state_ = State{};
}

bool TransferMarkerEditor::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(state_));
    state_ = std::move(undo_.back());
    undo_.pop_back();
    ++revision_;
    return true;
}

bool TransferMarkerEditor::redo() {
    if (redo_.empty()) return false;
    undo_.push_back(std::move(state_));
    state_ = std::move(redo_.back());
    redo_.pop_back();
    ++revision_;
    return true;
}

std::size_t TransferMarkerEditor::pairCount() const { return state_.pairCount(); }

std::size_t TransferMarkerEditor::pendingCount() const { return state_.pendingCount(); }

std::vector<TransferMarker> TransferMarkerEditor::pairs() const {
    std::vector<TransferMarker> out;
    const std::size_t n = pairCount();
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const MarkerPoint& a = state_.sides[0][i];
        const MarkerPoint& b = state_.sides[1][i];
        out.push_back({a.root, a.point, b.root, b.point});
    }
    return out;
}

std::string TransferMarkerEditor::summary() const { return state_.summary(); }

std::string markerRequirement(TransferMethod method, std::size_t pairs) {
    std::size_t need = 0;
    const char* name = "";
    if (method == TransferMethod::MarkersAlign) {
        need = 3;
        name = "Markers: align";
    } else if (method == TransferMethod::MarkersWarp) {
        need = 4;
        name = "Markers: warp";
    }
    if (pairs >= need) return {};
    return std::string(name) + " needs " + std::to_string(need) + " or more pairs (" + std::to_string(pairs) + " placed)";
}

bool pickMarkerPoint(const ScalpMesh& mesh, const Vec3f& origin, const Vec3f& direction, MarkerPoint* hit) {
    if (mesh.triangleCount() == 0) return false;
    SurfacePoint sp;
    if (!mesh.intersectRay(origin, direction, &sp)) return false;
    if (hit) *hit = MarkerPoint{sp.root, sp.position, 0};
    return true;
}

bool snapMarkerPoint(const ScalpMesh& mesh, const Vec3f& p, MarkerPoint* out) {
    if (mesh.triangleCount() == 0) return false;
    const SurfacePoint sp = mesh.closestPoint(p);
    if (sp.root.face < 0) return false;
    if (out) *out = MarkerPoint{sp.root, sp.position, 0};
    return true;
}

}  // namespace groomlab
