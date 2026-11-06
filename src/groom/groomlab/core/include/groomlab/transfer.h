// SPDX-License-Identifier: Apache-2.0
#pragma once

// Moving a groom to another scalp (GH-8): a new sculpt, a retopologized head, another
// character. Guides, parts and flow arrows are re-rooted on the new scalp; attribute layers ride
// with the guides. Painted scalp maps are projected (scalp_paint.h: projectVertexMap,
// projectTexture).
//
// Where a point of the old scalp lands is a ScalpMapping, built by one of five methods
// (TransferMethod):
//   closest       the closest point of the new scalp to the old point, as it is.
//   bounds        the old scalp's bounding box moved and scaled (uniformly) onto the new one's
//                 first (ScalpFit::boundingBoxes), then the closest point.
//   markersAlign  marker pairs (a point of the old scalp and the matching point of the new one,
//                 3 or more, not on one line) give the similarity (rotation, uniform scale,
//                 translation) that best takes the old markers onto the new ones by least squares
//                 (Horn's closed form with Umeyama's scale), then the closest point.
//   markersWarp   the marker pairs (4 or more, not on one plane) drive a smooth warp of space: a
//                 polyharmonic spline (kernel r^3, the 3D thin-plate-like spline that is smooth at
//                 its centers) plus an affine part, fitted so each old marker lands on its new one
//                 (up to a small smoothing term, kWarpSmoothing), then the closest point. Fewer or
//                 coplanar markers fall back to markersAlign with a warning.
//   uv            both scalps laid out alike in UV: each old point's UV found on the new scalp
//                 (no UV match: the closest point, counted in the warnings).
//
// For every method but closest a guide's shape (its points about its root) is carried by the
// mapping's local linear part at the root (MappedPoint::rotation and scale): the similarity's for
// align, the warp's Jacobian orthonormalized into a rotation times a uniform scale (its polar
// decomposition: guides turn and scale, they never shear) for warp, the change between the two
// scalps' UV tangent frames for uv (the scale from the ratio of their UV densities). closest and
// bounds turn shapes by the least rotation between the normals, as they always did.

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "groomlab/math.h"
#include "groomlab/scalp_mesh.h"

namespace groomlab {

// Where a point of the old scalp lands before the closest-point search. The
// identity keeps positions; a fit moves the old scalp's bounding-box center
// onto the new one's and scales uniformly by the ratio of their diagonals
// (for a scalp in another place or of another size). Guides scale with it.
// The `bounds` method of a ScalpMapping (and what ScalpMapping(from, to, fit) wraps).
struct ScalpFit {
    Vec3f fromCenter{0, 0, 0};
    Vec3f toCenter{0, 0, 0};
    float scale = 1.0f;

    Vec3f apply(const Vec3f& p) const { return toCenter + (p - fromCenter) * scale; }
    // A point of the new scalp taken back into the old one's space (maps are projected this
    // way: each point of the new scalp looks up the old scalp where it came from).
    Vec3f unapply(const Vec3f& p) const { return fromCenter + (p - toCenter) / (scale != 0.0f ? scale : 1.0f); }
    static ScalpFit identity() { return {}; }
    static ScalpFit boundingBoxes(const ScalpMesh& from, const ScalpMesh& to);
};

enum class TransferMethod { Closest, Bounds, MarkersAlign, MarkersWarp, UV };

// "closest", "bounds", "markersAlign", "markersWarp", "uv" (what hosts and files say).
const char* transferMethodName(TransferMethod method);
// The method a name says; throws std::invalid_argument naming the five for another.
TransferMethod transferMethodFromName(const std::string& name);
bool usesMarkers(TransferMethod method);

// One marker pair: a point of the old scalp and the matching point of the new one. Each side is
// kept as a root (face and barycentrics: it follows the mesh when the mesh moves or deforms) and
// as a world position (what it was when picked). A side whose root is not on the mesh it is
// read on (face -1, or past its triangles) is read at its position.
struct TransferMarker {
    StrandRoot oldRoot;
    Vec3f oldPoint;
    StrandRoot newRoot;
    Vec3f newPoint;
};

// Where the marker sides are on these meshes (their roots, else their positions).
Vec3f markerOldPosition(const TransferMarker& marker, const ScalpMesh& from);
Vec3f markerNewPosition(const TransferMarker& marker, const ScalpMesh& to);
// Markers with both sides on the meshes: a root from the position (the closest point) where
// there is none, the position from the root where there is one. What hosts store after picking.
std::vector<TransferMarker> snapTransferMarkers(const std::vector<TransferMarker>& markers, const ScalpMesh& from,
                                                const ScalpMesh& to);

// The markers as JSON, the one format every host stores (Maya's transferMarkers attribute, the
// groom file's "transferMarkers", the app's markers files):
//   [{"old": {"face": 12, "u": 0.2, "v": 0.3, "point": [x, y, z]}, "new": {...}}, ...]
// A side may leave out face/u/v (read at its point) or point (read at its root; then the root
// must be on the mesh). Blank text is no markers; throws std::invalid_argument for other text
// that is not such a list.
std::string transferMarkersToJson(const std::vector<TransferMarker>& markers);
std::vector<TransferMarker> transferMarkersFromJson(const std::string& text);

struct TransferOptions {
    TransferMethod method = TransferMethod::Closest;
    std::vector<TransferMarker> markers;  // for markersAlign and markersWarp
    // Guides keep their lengths and tube radii (they turn, they do not scale). Off (the default):
    // they scale with the head (the mapping's scale at their root). Methods but closest.
    bool keepLengths = false;
    // Fit the groom to the new scalp's size (the hosts' transfers; the mapping does not read it):
    // the new scalp taken as the old one resized, the groom's scale reference kept, so its scale
    // becomes k * sqrt(new area / old area) (lengths, widths and clumps follow, densities keep the
    // strand count, guide shapes scale about their roots) and the guides turn without scaling
    // (as keepLengths). Off (the default): the groom keeps the size shown.
    bool fitSize = false;
};

// A 3x3 matrix, rows. What a mapping does to a vector standing at a point.
struct Mat3f {
    std::array<Vec3f, 3> rows{Vec3f{1, 0, 0}, Vec3f{0, 1, 0}, Vec3f{0, 0, 1}};
    Vec3f operator*(const Vec3f& v) const { return {rows[0].dot(v), rows[1].dot(v), rows[2].dot(v)}; }
    static Mat3f identity() { return {}; }
};

// A point of the old scalp on the new one.
struct MappedPoint {
    SurfacePoint target;   // on the new scalp (face -1 when nothing was found)
    Vec3f mapped;          // where the mapping took the old point before the projection
    Mat3f rotation;        // the mapping's local rotation there (guide shapes, flow directions)
    float scale = 1.0f;    // and its uniform scale (1 with keepLengths for shapes: see shapeScale)
};

// The mapping from one scalp to another, by a TransferMethod. It reads the two meshes it was
// built on (kept by reference: they outlive it); onMeshes() rebuilds it on other poses of them
// (the rest poses maps are projected in).
class ScalpMapping {
public:
    // Throws std::invalid_argument when the markers cannot make the mapping (fewer than 3, on
    // one line, two at one point of a scalp); `warnings` gets the fallbacks (warp to align) and,
    // after use, nothing else: uvMisses() counts the uv method's points that fell back.
    ScalpMapping(const ScalpMesh& from, const ScalpMesh& to, const TransferOptions& options,
                 std::vector<std::string>* warnings = nullptr);
    // The bounds method with this fit (identity: closest), as transfers did with a ScalpFit.
    ScalpMapping(const ScalpMesh& from, const ScalpMesh& to, const ScalpFit& fit);
    ~ScalpMapping();
    ScalpMapping(const ScalpMapping&);
    ScalpMapping& operator=(const ScalpMapping&) = delete;

    // The same method and markers on other meshes (other poses of the same two scalps).
    ScalpMapping onMeshes(const ScalpMesh& from, const ScalpMesh& to) const;

    const ScalpMesh& from() const { return *from_; }
    const ScalpMesh& to() const { return *to_; }
    const TransferOptions& options() const { return options_; }
    // The method used: the one asked for, or markersAlign when warp fell back.
    TransferMethod method() const { return method_; }
    // closest and bounds: shapes turned by the normals, scaled by fit().scale (the old transfer).
    bool byNormals() const { return method_ == TransferMethod::Closest || method_ == TransferMethod::Bounds; }
    const ScalpFit& fit() const { return fit_; }
    // The scale a guide's shape takes at a mapped point: 1 with keepLengths, else its scale.
    float shapeScale(const MappedPoint& m) const { return options_.keepLengths ? 1.0f : m.scale; }

    // A point of the old scalp (a root on `from`) on the new one.
    MappedPoint map(const StrandRoot& oldRoot) const;
    // Any point near the old scalp: its closest point there first.
    MappedPoint mapPoint(const Vec3f& oldPosition) const;
    // A point of the new scalp (its position, and its root on `to` when known: face -1 to look it
    // up) back on the old one: where a map's value comes from.
    SurfacePoint back(const Vec3f& newPosition, const StrandRoot& newRoot = {}) const;

    // The space parts alone (no projection): old space -> new and back. uv has none (identity).
    Vec3f apply(const Vec3f& p) const;
    Vec3f unapply(const Vec3f& p) const;

    // uv: how many map() and back() calls found no UV match and fell back to the closest point.
    std::size_t uvMisses() const;

    struct Impl;

private:
    ScalpMapping(const ScalpMesh& from, const ScalpMesh& to);
    void build(std::vector<std::string>* warnings);

    const ScalpMesh* from_;
    const ScalpMesh* to_;
    TransferOptions options_;
    TransferMethod method_ = TransferMethod::Closest;
    ScalpFit fit_;
    std::unique_ptr<Impl> impl_;
};

// ---- The math, exposed for tests and hosts ---------------------------------------------

// A similarity: p -> scale * rotation * p + translation.
struct Similarity {
    Mat3f rotation;
    float scale = 1.0f;
    Vec3f translation;
    Vec3f apply(const Vec3f& p) const { return rotation * p * scale + translation; }
    Similarity inverse() const;
};
// The similarity that best takes `from[i]` onto `to[i]` (least squares; Horn 1987 with
// Umeyama's scale). Throws std::invalid_argument for fewer than 3 points or points on a line.
Similarity fitSimilarity(const std::vector<Vec3f>& from, const std::vector<Vec3f>& to);

// A smooth warp of space through point pairs: f(x) = A x + b + sum_i w_i |x - c_i|^3, fitted in
// coordinates normalized by the centers' size, with `smoothing` (relative) added to the kernel's
// diagonal: 0 interpolates exactly, a little keeps close centers from blowing up.
constexpr double kWarpSmoothing = 1e-4;
class SplineWarp {
public:
    SplineWarp() = default;
    // Throws std::invalid_argument for fewer than 4 centers or centers on one plane.
    SplineWarp(const std::vector<Vec3f>& from, const std::vector<Vec3f>& to, double smoothing = kWarpSmoothing);
    Vec3f apply(const Vec3f& p) const;
    // The warp's Jacobian at p (rows: d f_k / d x).
    Mat3f jacobian(const Vec3f& p) const;
    bool empty() const { return centers_.empty(); }

private:
    std::vector<std::array<double, 3>> centers_;   // normalized
    std::vector<std::array<double, 3>> weights_;   // per center, per output coordinate
    std::array<std::array<double, 4>, 3> affine_{};  // per output: constant, x, y, z
    std::array<double, 3> origin_{};
    double size_ = 1.0;
};

// The rotation and uniform scale closest to `m` (its polar decomposition m = R S, scale the
// cube root of |det m|). False when m is singular or turns space inside out (det <= 0).
bool rotationAndScale(const Mat3f& m, Mat3f* rotation, float* scale);

}  // namespace groomlab
