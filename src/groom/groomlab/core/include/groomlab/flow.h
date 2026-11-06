// SPDX-License-Identifier: Apache-2.0
#pragma once

// Flow fields: direction arrows placed on the scalp that say which way hair
// lies there, blended smoothly in between. The Flow node lays strands along
// the field: short fur, vellus and eyebrows groomed without guides.
//
// Pins are bound to the scalp (face, barycentric) with their direction in the
// root triangle's frame, so the field follows a deforming scalp. Like parts, they
// are bound to the cage (the scalp before the Scalp node subdivides it) and
// carried onto a refined scalp (FlowSet::onRefined).
//
// Sinks are points hair turns around within a radius: whorls (a crown, a
// cowlick), a point hair gathers into, or one it parts away from. Each takes
// over the field near its center and fades out at its radius.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/point_index.h"
#include "groomlab/scalp_mesh.h"
#include "groomlab/strand_set.h"
#include "groomlab/transfer.h"

namespace groomlab {

class SubdivisionPlan;  // subdivision.h

struct FlowPin {
    StrandRoot root;
    Vec3f local;  // unit direction in the root triangle's frame (x first edge, y normal x x, z normal)
    // The stroke that laid it and its place along it (drawn as one curve with an
    // arrowhead at its end); -1: a pin on its own.
    int line = -1;
    int seq = 0;
};

struct FlowSink {
    StrandRoot root;
    // The heading around the center, degrees from pointing straight out:
    // 0 parts away, 180 gathers in, +-90 circles (positive counter-clockwise
    // seen from outside the scalp); a whorl spirals out in between.
    float angle = 70.0f;
    float radius = 1.0f;  // world units
};

// A sink kind's angle: whorl (70), whorlcw (-70), converge (180), part (0).
// Throws std::invalid_argument for another name.
float flowSinkAngle(const std::string& kind);

class FlowSet {
public:
    std::vector<FlowPin> pins;
    std::vector<FlowSink> sinks;

    bool empty() const { return pins.empty() && sinks.empty(); }
    // A pin at `at` pointing along `direction` (projected onto the surface there).
    // Throws std::invalid_argument off the scalp or for a direction along the normal.
    static FlowPin makePin(const ScalpMesh& scalp, const SurfacePoint& at, const Vec3f& direction);
    // The same pins and sinks on another scalp (GH-8): each re-rooted at the closest point to
    // where it was (after `fit`), pins keeping their world direction, sinks their radius (scaled
    // by the fit). Ones off `from` are dropped, and pins whose direction lies along the new
    // normal. Throws std::invalid_argument for a scalp without faces.
    FlowSet transferred(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit = {}) const;
    // The same by any mapping (transfer.h): each pin and sink where the mapping takes its root, a
    // pin's direction turned by the mapping's rotation there, a sink's radius times its scale.
    FlowSet transferred(const ScalpMapping& mapping) const;
    // Stored bound to the cage, as parts are (PartSet::onRefined): the pins and sinks carried onto
    // `refined` (the cage refined by `plan`) and back. A pin's direction is turned from the one
    // surface's frame there to the other's by the least rotation between their normals, so
    // carrying it back undoes it (to rounding). `cage` and `refined` in one pose: the session
    // gives their rest poses (the result the same on every frame), a tool the pose it shows.
    FlowSet onRefined(const SubdivisionPlan& plan, const ScalpMesh& cage, const ScalpMesh& refined) const;
    FlowSet onCage(const SubdivisionPlan& plan, const ScalpMesh& refined, const ScalpMesh& cage) const;

    // {"version": 1, "pins": [{"face": f, "u": u, "v": v, "dir": [x, y, z], "line": l, "seq": s}],
    //  "sinks": [{"face": f, "u": u, "v": v, "angle": a, "radius": r}]}
    // (dir frame-local; line and seq only for pins laid by a stroke; sinks only when there are some).
    // Empty text is no pins. fromJson throws std::invalid_argument.
    std::string toJson() const;
    static FlowSet fromJson(const std::string& text);
    std::uint64_t contentHash() const;
};

// A FlowSet on a scalp pose: world positions and directions, and the blend.
class FlowField {
public:
    FlowField(const FlowSet& set, const ScalpMesh& scalp);

    bool empty() const { return positions_.empty() && sinkPositions_.empty(); }
    // The unit direction hair lies in at a surface point with normal n: the
    // nearest pins' directions (inverse-distance weighted), each carried onto
    // the tangent plane there, turned by any sink it is within the radius of.
    // Zero with neither.
    Vec3f at(const Vec3f& position, const Vec3f& normal) const;
    // The same at a scalp point, from the field baked at the scalp's vertices
    // when this was built (a barycentric blend of the three corners, laid on
    // the tangent plane): O(1) per call instead of a neighbour search, for
    // tracing strands point by point. Falls back to at() where the blend
    // degenerates (the middle of a whorl). `scalp` must be the one given to
    // the constructor.
    Vec3f atRoot(const ScalpMesh& scalp, const StrandRoot& root, const Vec3f& normal) const;

    const std::vector<Vec3f>& positions() const { return positions_; }
    const std::vector<Vec3f>& directions() const { return directions_; }
    // Per position: the stroke it came from (-1: none) and its place along it.
    const std::vector<int>& lines() const { return lines_; }
    const std::vector<int>& seqs() const { return seqs_; }
    // Sinks on this pose: center, normal there, angle (radians) and radius.
    const std::vector<Vec3f>& sinkPositions() const { return sinkPositions_; }
    const std::vector<Vec3f>& sinkNormals() const { return sinkNormals_; }
    const std::vector<float>& sinkAngles() const { return sinkAngles_; }
    const std::vector<float>& sinkRadii() const { return sinkRadii_; }
    // The field baked at each vertex of the scalp it was built on (what atRoot blends; empty: none).
    const std::vector<Vec3f>& vertexDirections() const { return vertexDirections_; }
    // Sink s's heading at `position` (on the surface, normal n) and how much
    // it rules there (1 at the center, 0 from its radius on).
    Vec3f sinkDirection(std::size_t s, const Vec3f& position, const Vec3f& normal, float* weight) const;
    std::uint64_t contentHash() const { return hash_; }

private:
    Vec3f pinsAt(const Vec3f& position, const Vec3f& n) const;

    std::vector<Vec3f> positions_, directions_;
    std::vector<int> lines_, seqs_;
    std::vector<Vec3f> sinkPositions_, sinkNormals_;
    std::vector<float> sinkAngles_, sinkRadii_;
    PointIndex index_;
    std::vector<Vec3f> vertexDirections_;  // the field at each scalp vertex (empty: no pins)
    std::uint64_t hash_ = 0;
};

using FlowPtr = std::shared_ptr<const FlowField>;

// The world frame of a scalp point's triangle (x along the first edge, made
// perpendicular to the smooth normal; z the normal).
void surfaceFrame(const ScalpMesh& scalp, const StrandRoot& root, Vec3f& x, Vec3f& y, Vec3f& z);

}  // namespace groomlab
