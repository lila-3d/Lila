// SPDX-License-Identifier: Apache-2.0
#pragma once

// The marker editor behind every host's Transfer Groom window (Wrap-style: two views, the groom's
// scalp on the left, the new scalp on the right, points clicked on each). The model alone, no
// views: two ordered point lists, edits with an undo stack, and the pairs a transfer takes.
//
// Rules (the same in Maya and the app):
//   - Each side keeps its own ordered list. Pair N is the Nth point on the left and the Nth on the
//     right; points are placed in any order (no alternation).
//   - A point whose index is at or past the other list's length is unmatched (pending): hosts draw
//     it hollow. Numbers are the pair index from 1.
//   - Deleting a point deletes the point at the same index on the other side too (its partner, if
//     there is one), and both lists renumber together, so the pairs stay matched. Deleting an
//     unmatched point deletes only it.
//   - Symmetry: a point added with its mirror (addMirrored) is added as two points, one after the
//     other, on the same side, in one symmetry group. Deleting either deletes both, and their
//     partners at the same two indices on the other side.
//   - Every edit (add, add mirrored, delete, move, set) is one undo step; a drag's moves after its
//     first join that step (move with newStep false). Undo and redo walk the stack; an edit after
//     an undo drops the redo steps.
//   - Only complete pairs go to the transfer (pairs()); pending points stay in the editor.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/strand_set.h"
#include "groomlab/transfer.h"

namespace groomlab {

// The left view's side (the groom's current scalp: a marker's old side) or the right view's (the
// new scalp: its new side).
enum class MarkerSide { Old = 0, New = 1 };

// One clicked point: where it was picked on its mesh (a triangle and barycentrics; face -1 when
// only the position is known) and its world position. `group`: nonzero for a point added with its
// mirror (both share it).
struct MarkerPoint {
    StrandRoot root;
    Vec3f point;
    std::uint32_t group = 0;
};

// Two ordered point lists and the pairing rules above, without a history: what the Transfer
// window's editor keeps (and the Reference window's landmarks, reference_edit.h: a photo point on
// one side, a mesh point on the other). Every editing call is a plain change of the lists.
struct MarkerLists {
    std::vector<MarkerPoint> sides[2];

    static std::size_t index(MarkerSide side) { return side == MarkerSide::Old ? 0 : 1; }
    static MarkerSide other(MarkerSide side) { return side == MarkerSide::Old ? MarkerSide::New : MarkerSide::Old; }
    const std::vector<MarkerPoint>& points(MarkerSide side) const { return sides[index(side)]; }
    std::size_t count(MarkerSide side) const { return points(side).size(); }
    const MarkerPoint& point(MarkerSide side, std::size_t i) const;  // throws std::out_of_range
    bool empty() const { return sides[0].empty() && sides[1].empty(); }

    std::size_t add(MarkerSide side, const MarkerPoint& p);  // at the end (no group); its index
    // A point and its mirror at the end in symmetry group `group`; the first one's index.
    std::size_t addPair(MarkerSide side, const MarkerPoint& p, const MarkerPoint& mirror, std::uint32_t group);
    std::vector<std::size_t> removalSet(MarkerSide side, std::size_t i) const;
    std::vector<std::size_t> remove(MarkerSide side, std::size_t i);  // the indices gone (removalSet)
    void set(MarkerSide side, std::size_t i, const MarkerPoint& p);   // its group kept; throws std::out_of_range

    std::size_t pairCount() const { return std::min(sides[0].size(), sides[1].size()); }
    std::size_t pendingCount() const {
        return sides[0].size() > sides[1].size() ? sides[0].size() - sides[1].size() : sides[1].size() - sides[0].size();
    }
    bool matched(MarkerSide side, std::size_t i) const { return i < count(other(side)); }
    // "6 pairs, 1 unmatched", "1 pair", or `none` when both lists are empty.
    std::string summary(const char* none = "No markers") const;
};

class TransferMarkerEditor {
public:
    TransferMarkerEditor() = default;
    // Both lists from stored pairs (each pair's old side on the left, new side on the right); no
    // undo history.
    explicit TransferMarkerEditor(const std::vector<TransferMarker>& pairs) { reset(pairs); }

    // Replaces both lists with these pairs and clears the history (a window opening on a groom).
    void reset(const std::vector<TransferMarker>& pairs);
    // Replaces both lists with these pairs: one undo step (markers imported from a file, Clear).
    void setPairs(const std::vector<TransferMarker>& pairs);

    const MarkerLists& lists() const { return state_; }
    const std::vector<MarkerPoint>& points(MarkerSide side) const { return state_.sides[index(side)]; }
    std::size_t count(MarkerSide side) const { return points(side).size(); }
    const MarkerPoint& point(MarkerSide side, std::size_t i) const;  // throws std::out_of_range

    // A point at the end of `side`'s list; returns its index.
    std::size_t add(MarkerSide side, const MarkerPoint& p);
    // A point and its mirror at the end of `side`'s list, in a new symmetry group (one step);
    // returns the first one's index. A mirror within `mergeDistance` of the point (a point on the
    // mirror plane) is not added: the point alone is, without a group.
    std::size_t addMirrored(MarkerSide side, const MarkerPoint& p, const MarkerPoint& mirror, float mergeDistance = 0.0f);
    // The indices remove(side, i) deletes from each list, ascending: i and the points of its
    // symmetry group on that side. (The other side loses those of them it has.)
    std::vector<std::size_t> removalSet(MarkerSide side, std::size_t i) const;
    // Deletes point i of `side`, its symmetry partners on that side and the points at the same
    // indices on the other side; both lists renumber. Returns the indices deleted (removalSet).
    // Throws std::out_of_range.
    std::vector<std::size_t> remove(MarkerSide side, std::size_t i);
    // Point i of `side` moved (its group kept). newStep false: joins the last step (a drag's moves
    // after its first). Throws std::out_of_range.
    void move(MarkerSide side, std::size_t i, const MarkerPoint& p, bool newStep = true);
    // Both lists emptied (one step; nothing when already empty).
    void clear();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    std::size_t undoDepth() const { return undo_.size(); }

    // Complete pairs: min(left, right) of them, in order (what a transfer and the groom keep).
    std::size_t pairCount() const;
    // Points without a partner: the longer list's tail.
    std::size_t pendingCount() const;
    // Whether point i of `side` has a partner (i < the other list's length).
    bool matched(MarkerSide side, std::size_t i) const { return i < count(other(side)); }
    std::vector<TransferMarker> pairs() const;
    // "6 pairs, 1 unmatched", "1 pair", "No markers".
    std::string summary() const;

    // A monotonic counter of edits (undo and redo count): hosts store and redraw when it moved.
    std::uint64_t revision() const { return revision_; }

    static MarkerSide other(MarkerSide side) { return side == MarkerSide::Old ? MarkerSide::New : MarkerSide::Old; }

private:
    using State = MarkerLists;
    static std::size_t index(MarkerSide side) { return MarkerLists::index(side); }
    void record();  // the state before an edit onto the undo stack (the redo steps dropped)

    State state_;
    std::vector<State> undo_, redo_;
    std::uint32_t nextGroup_ = 1;
    std::uint64_t revision_ = 0;
    static constexpr std::size_t kMaxUndo = 256;
};

// Why a method cannot run yet with `pairs` complete marker pairs, or "" when it can: markersAlign
// needs 3 or more pairs, markersWarp 4 or more; the other methods need none.
std::string markerRequirement(TransferMethod method, std::size_t pairs);

// A point mirrored across the plane x = planeX (Symmetry X: hosts mirror across their mesh's
// bounding-box center, x = 0 for a head centered on the origin, then snap the result onto it).
inline Vec3f mirrorX(const Vec3f& p, float planeX = 0.0f) { return {2.0f * planeX - p.x, p.y, p.z}; }

// The ray pick both views share: the hit of the ray origin + t * direction on `mesh` as a marker
// point (the triangle, barycentrics and position); false on a miss.
bool pickMarkerPoint(const ScalpMesh& mesh, const Vec3f& origin, const Vec3f& direction, MarkerPoint* hit);
// A point snapped onto `mesh` (its closest point) as a marker point; false on an empty mesh.
bool snapMarkerPoint(const ScalpMesh& mesh, const Vec3f& p, MarkerPoint* out);

}  // namespace groomlab
