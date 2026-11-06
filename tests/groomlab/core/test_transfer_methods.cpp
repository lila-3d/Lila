// SPDX-License-Identifier: Apache-2.0
// Transfer methods (transfer.h): bounds, markers (align and warp) and UVs, guides carried by the
// mapping's local rotation and scale, maps projected through each mapping, marker files.

#include <cmath>

#include <gtest/gtest.h>

#include "groomlab/flow.h"
#include "groomlab/groom_file.h"
#include "groomlab/guide_hierarchy.h"
#include "groomlab/parts.h"
#include "groomlab/scalp_paint.h"
#include "groomlab/transfer.h"
#include "test_helpers.h"

namespace groomlab {
namespace {

float curveLength(const std::vector<Vec3f>& pts) {
    float len = 0.0f;
    for (std::size_t i = 1; i < pts.size(); ++i) len += (pts[i] - pts[i - 1]).length();
    return len;
}

// The UV sphere with its lat-long UVs on every corner (u around, v down from the top).
std::shared_ptr<const ScalpMesh> uvSphere(int rings, int segments) {
    const auto sphere = test::makeSphere(rings, segments, 1.0f);
    std::vector<ScalpMesh::UV> uvs;
    for (const auto& t : sphere->triangles()) {
        for (const std::int32_t v : t) {
            const int r = v / (segments + 1), s = v % (segments + 1);
            uvs.push_back({static_cast<float>(s) / segments, 1.0f - static_cast<float>(r) / rings});
        }
    }
    auto out = std::make_shared<ScalpMesh>(sphere->points(), sphere->triangles());
    out->setCornerUVs(uvs);
    return out;
}

// A head: an ellipsoid (no symmetry a bounding box or closest point could lean on).
std::shared_ptr<const ScalpMesh> headA() {
    const auto sphere = uvSphere(32, 64);
    std::vector<Vec3f> pts;
    for (const Vec3f& p : sphere->points()) pts.push_back({p.x * 9.0f, p.y * 11.0f, p.z * 10.0f});
    return std::make_shared<const ScalpMesh>(sphere->withPoints(pts));
}

// A rotation about a unit axis, as a Mat3f.
Mat3f rotation(Vec3f axis, float angle) {
    axis = axis.normalized();
    const Vec3f x = rotateAxisAngle({1, 0, 0}, axis, angle), y = rotateAxisAngle({0, 1, 0}, axis, angle),
                z = rotateAxisAngle({0, 0, 1}, axis, angle);  // the columns
    Mat3f m;
    m.rows[0] = {x.x, y.x, z.x};
    m.rows[1] = {x.y, y.y, z.y};
    m.rows[2] = {x.z, y.z, z.z};
    return m;
}

// The new head's exact transform: turned, scaled and moved.
struct Moved {
    Mat3f turn = rotation({0.3f, 1.0f, 0.2f}, 1.1f);
    float scale = 1.5f;
    Vec3f offset{40.0f, -5.0f, 12.0f};
    Vec3f operator()(const Vec3f& p) const { return turn * p * scale + offset; }
};

std::shared_ptr<const ScalpMesh> transformed(const ScalpMesh& mesh, const std::function<Vec3f(const Vec3f&)>& f) {
    std::vector<Vec3f> pts;
    for (const Vec3f& p : mesh.points()) pts.push_back(f(p));
    return std::make_shared<const ScalpMesh>(mesh.withPoints(pts));
}

// Bent guides standing on the upper half of the head, with children.
GuideHierarchy groomOn(const ScalpMesh& head) {
    GuideHierarchy h;
    const GroupId g = h.addGroup("primary");
    for (int i = 0; i < 10; ++i) {
        const float a = 0.3f + 0.11f * i, b = 1.9f * i;
        const SurfacePoint sp = head.closestPoint(Vec3f(std::sin(a) * std::cos(b), std::cos(a), std::sin(a) * std::sin(b)) * 20.0f);
        const Vec3f n = sp.normal.normalized();
        const Vec3f side = n.cross(Vec3f(0, 0, 1)).normalized();
        std::vector<Vec3f> pts;
        for (int k = 0; k <= 6; ++k) {
            const float t = k / 6.0f;
            pts.push_back(sp.position + n * (4.0f * t) + side * (3.0f * t * t));
        }
        h.addGuide(g, pts);
    }
    h.evaluate(&head);
    ChildGenerationParams params;
    params.count = 2;
    h.generateChildren(g, "children", params);
    h.evaluate(&head);
    return h;
}

// Markers at vertices of the old head spread over it, and where `f` takes them.
std::vector<TransferMarker> markersFor(const ScalpMesh& from, const ScalpMesh& to, int count) {
    const Vec3f dirs[] = {{0, 1, 0}, {1, 0.2f, 0}, {-1, 0.2f, 0}, {0, 0.3f, 1}, {0, 0.1f, -1}, {0.6f, -0.6f, 0.5f},
                          {-0.5f, -0.5f, -0.6f}, {0.7f, 0.6f, -0.4f}};
    std::vector<TransferMarker> out;
    for (int i = 0; i < count; ++i) {
        TransferMarker m;
        m.oldRoot = from.closestPoint(dirs[i].normalized() * 30.0f).root;
        m.oldPoint = from.position(m.oldRoot);
        m.newRoot = m.oldRoot;  // the same topology: the same face and barycentrics
        m.newPoint = to.position(m.newRoot);
        out.push_back(m);
    }
    return out;
}

TEST(TransferMethods, MethodNames) {
    for (TransferMethod m : {TransferMethod::Closest, TransferMethod::Bounds, TransferMethod::MarkersAlign,
                             TransferMethod::MarkersWarp, TransferMethod::UV}) {
        EXPECT_EQ(transferMethodFromName(transferMethodName(m)), m);
    }
    EXPECT_THROW(transferMethodFromName("nearest"), std::invalid_argument);
}

TEST(TransferMethods, FitSimilarityRecoversTheTransform) {
    const Moved t;
    std::vector<Vec3f> a = {{1, 2, 3}, {-4, 0, 1}, {2, -3, 5}, {0, 0, -2}};
    std::vector<Vec3f> b;
    for (const Vec3f& p : a) b.push_back(t(p));
    const Similarity s = fitSimilarity(a, b);
    EXPECT_NEAR(s.scale, t.scale, 1e-4f);
    for (const Vec3f& p : a) EXPECT_LT((s.apply(p) - t(p)).length(), 1e-3f);
    const Similarity inv = s.inverse();
    for (const Vec3f& p : a) EXPECT_LT((inv.apply(s.apply(p)) - p).length(), 1e-3f);
}

TEST(TransferMethods, ClosestAndBoundsAreTheFitsTransferExactly) {
    const auto a = headA();
    const auto b = transformed(*a, [](const Vec3f& p) { return p * 1.3f + Vec3f(5, 0, 0); });
    for (const TransferMethod method : {TransferMethod::Closest, TransferMethod::Bounds}) {
        GuideHierarchy viaFit = groomOn(*a), viaMapping = viaFit;
        const ScalpFit fit = method == TransferMethod::Bounds ? ScalpFit::boundingBoxes(*a, *b) : ScalpFit::identity();
        viaFit.transfer(*a, *b, fit);
        TransferOptions options;
        options.method = method;
        viaMapping.transfer(ScalpMapping(*a, *b, options));
        EXPECT_EQ(viaFit.toJson(), viaMapping.toJson()) << transferMethodName(method);
    }
}

TEST(TransferMethods, AlignLandsGuidesOnAMovedTurnedScaledHeadWhereClosestAndBoundsFail) {
    const auto a = headA();
    const Moved t;
    const auto b = transformed(*a, t);
    const GuideHierarchy before = groomOn(*a);
    auto rootError = [&](const GuideHierarchy& h) {
        float worst = 0.0f;
        for (std::size_t i = 0; i < h.guides().size(); ++i) {
            if (h.guides()[i].isChild()) continue;
            worst = std::max(worst, (h.guides()[i].points.front() - t(before.guides()[i].points.front())).length());
        }
        return worst;
    };
    TransferOptions options;
    options.method = TransferMethod::MarkersAlign;
    options.markers = markersFor(*a, *b, 4);
    GuideHierarchy aligned = before;
    aligned.transfer(ScalpMapping(*a, *b, options));
    EXPECT_LT(rootError(aligned), 0.01f * 15.0f) << "align lands roots where they belong";
    for (std::size_t i = 0; i < aligned.guides().size(); ++i) {
        const auto& was = before.guides()[i].points;
        const auto& now = aligned.guides()[i].points;
        ASSERT_EQ(was.size(), now.size());
        for (std::size_t k = 0; k < was.size(); ++k) {
            EXPECT_LT((now[k] - t(was[k])).length(), 0.05f) << "guide " << i << " point " << k << " follows the transform";
        }
    }
    // The case: without markers the guides land elsewhere.
    for (const TransferMethod method : {TransferMethod::Closest, TransferMethod::Bounds}) {
        GuideHierarchy h = before;
        TransferOptions o;
        o.method = method;
        h.transfer(ScalpMapping(*a, *b, o));
        EXPECT_GT(rootError(h), 3.0f) << transferMethodName(method) << " misses on a turned head";
    }
}

TEST(TransferMethods, KeepLengthsTurnsWithoutScaling) {
    const auto a = headA();
    const auto b = transformed(*a, Moved());
    const GuideHierarchy before = groomOn(*a);
    TransferOptions options;
    options.method = TransferMethod::MarkersAlign;
    options.markers = markersFor(*a, *b, 5);
    options.keepLengths = true;
    GuideHierarchy h = before;
    h.transfer(ScalpMapping(*a, *b, options));
    for (std::size_t i = 0; i < h.guides().size(); ++i) {
        if (h.guides()[i].isChild()) continue;
        EXPECT_NEAR(curveLength(h.guides()[i].points), curveLength(before.guides()[i].points), 0.01f);
    }
    options.keepLengths = false;
    h = before;
    h.transfer(ScalpMapping(*a, *b, options));
    for (std::size_t i = 0; i < h.guides().size(); ++i) {
        if (h.guides()[i].isChild()) continue;
        EXPECT_NEAR(curveLength(h.guides()[i].points), 1.5f * curveLength(before.guides()[i].points), 0.02f);
    }
}

// The new head wider, more so toward the top (no affine map makes it), then turned and moved.
Vec3f widened(const Vec3f& p) {
    const Moved t;
    const float k = 1.3f + 0.015f * p.y;
    return t(Vec3f(p.x * k, p.y, p.z));
}

TEST(TransferMethods, WarpLandsGuidesOnAWiderHeadAndTurnsThemWithoutShear) {
    const auto a = headA();
    const auto b = transformed(*a, widened);
    const GuideHierarchy before = groomOn(*a);
    TransferOptions options;
    options.method = TransferMethod::MarkersWarp;
    options.markers = markersFor(*a, *b, 8);
    std::vector<std::string> warnings;
    const ScalpMapping warp(*a, *b, options, &warnings);
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(warp.method(), TransferMethod::MarkersWarp);
    for (const TransferMarker& m : options.markers) {
        EXPECT_LT((warp.apply(m.oldPoint) - m.newPoint).length(), 0.02f) << "each marker lands on its pair";
    }
    GuideHierarchy warped = before;
    warped.transfer(warp);
    options.method = TransferMethod::MarkersAlign;
    GuideHierarchy aligned = before;
    aligned.transfer(ScalpMapping(*a, *b, options));
    float warpError = 0.0f, alignError = 0.0f;
    for (std::size_t i = 0; i < before.guides().size(); ++i) {
        const HierarchyGuide& g = before.guides()[i];
        if (g.isChild()) continue;
        const Vec3f want = widened(g.points.front());
        warpError = std::max(warpError, (warped.guides()[i].points.front() - want).length());
        alignError = std::max(alignError, (aligned.guides()[i].points.front() - want).length());
        // No shear: the angle between the first and last segment, and the length ratios, kept.
        const auto& was = g.points;
        const auto& now = warped.guides()[i].points;
        const Vec3f a0 = (was[1] - was[0]).normalized(), a1 = (was.back() - was[was.size() - 2]).normalized();
        const Vec3f b0 = (now[1] - now[0]).normalized(), b1 = (now.back() - now[now.size() - 2]).normalized();
        EXPECT_NEAR(a0.dot(a1), b0.dot(b1), 1e-3f) << "guide " << i << " keeps its bend";
        const float s = curveLength(now) / curveLength(was);
        EXPECT_GT(s, 1.2f);
        EXPECT_LT(s, 1.9f);
        // Standing out of the new surface as it stood out of the old.
        const Vec3f n0 = a->closestPoint(was[0]).normal.normalized(), n1 = b->closestPoint(now[0]).normal.normalized();
        EXPECT_NEAR(a0.dot(n0), b0.dot(n1), 0.02f);
    }
    EXPECT_LT(warpError, 0.03f * 15.0f) << "warp lands near the corresponding spots";
    EXPECT_LT(warpError, alignError) << "and nearer than the best similarity";
}

TEST(TransferMethods, WarpInverseIsConsistent) {
    const auto a = headA();
    const auto b = transformed(*a, widened);
    TransferOptions options;
    options.method = TransferMethod::MarkersWarp;
    options.markers = markersFor(*a, *b, 8);
    const ScalpMapping warp(*a, *b, options);
    float worst = 0.0f;
    for (std::size_t v = 0; v < a->pointCount(); v += 37) {
        const Vec3f p = a->points()[v];
        worst = std::max(worst, (warp.unapply(warp.apply(p)) - p).length());
    }
    EXPECT_LT(worst, 0.02f * 20.0f) << "old -> new -> old within 2% of the head";
}

TEST(TransferMethods, UVTransferIsExactOnAMatchingLayout) {
    const auto a = headA();
    const Moved t;
    const auto b = transformed(*a, widened);  // same UVs, another shape and place
    const GuideHierarchy before = groomOn(*a);
    TransferOptions options;
    options.method = TransferMethod::UV;
    const ScalpMapping uv(*a, *b, options);
    GuideHierarchy h = before;
    h.transfer(uv);
    EXPECT_EQ(uv.uvMisses(), 0u);
    for (std::size_t i = 0; i < before.guides().size(); ++i) {
        const HierarchyGuide& g = before.guides()[i];
        if (g.isChild()) continue;
        const StrandRoot r = g.root;
        EXPECT_LT((h.guides()[i].points.front() - b->position(r)).length(), 1e-3f) << "the same face and barycentrics";
    }
    // A mesh without UVs is refused.
    const auto plain = std::make_shared<const ScalpMesh>(a->points(), a->triangles());
    EXPECT_THROW(ScalpMapping(*plain, *b, options), std::invalid_argument);
}

TEST(TransferMethods, UVMissesFallBackToClosest) {
    const auto a = headA();
    // The new head's UVs cover only the top half: the lower roots find no match.
    const auto sphere = uvSphere(32, 64);
    std::vector<ScalpMesh::UV> uvs = a->cornerUVs();
    for (auto& uv : uvs) uv[1] = 0.5f + uv[1] * 0.5f;
    auto b = std::make_shared<ScalpMesh>(a->points(), a->triangles());
    b->setCornerUVs(uvs);
    TransferOptions options;
    options.method = TransferMethod::UV;
    const ScalpMapping uv(*a, *b, options);
    const MappedPoint low = uv.mapPoint(Vec3f(0, -11, 0.5f));
    EXPECT_GE(uv.uvMisses(), 1u);
    EXPECT_GE(low.target.root.face, 0) << "the closest point instead";
}

TEST(TransferMethods, MapsProjectThroughEachMapping) {
    const auto a = headA();
    // A gray vertex map that varies over the old head: its height, 0 at the bottom, 1 at the top.
    std::vector<float> values;
    for (const Vec3f& p : a->points()) values.push_back((p.y + 11.0f) / 22.0f);
    struct Case {
        TransferMethod method;
        std::function<Vec3f(const Vec3f&)> f;
        float tolerance;
    };
    const Moved t;
    for (const Case& c : {Case{TransferMethod::MarkersAlign, t, 0.01f}, Case{TransferMethod::MarkersWarp, widened, 0.03f},
                          Case{TransferMethod::UV, widened, 0.01f}}) {
        const auto b = transformed(*a, c.f);
        TransferOptions options;
        options.method = c.method;
        options.markers = markersFor(*a, *b, 8);
        const std::vector<float> out = projectVertexMap(values, 1, ScalpMapping(*a, *b, options));
        ASSERT_EQ(out.size(), b->pointCount());
        float worst = 0.0f;
        for (std::size_t v = 0; v < out.size(); ++v) worst = std::max(worst, std::abs(out[v] - values[v]));  // same topology
        EXPECT_LT(worst, c.tolerance) << transferMethodName(c.method);
    }
    // Closest point on the turned head: the map lands wrong.
    const auto b = transformed(*a, t);
    const std::vector<float> out = projectVertexMap(values, 1, *a, *b);
    float worst = 0.0f;
    for (std::size_t v = 0; v < out.size(); ++v) worst = std::max(worst, std::abs(out[v] - values[v]));
    EXPECT_GT(worst, 0.2f);
}

TEST(TransferMethods, PartsAndFlowFollowTheMarkers) {
    const auto a = headA();
    const Moved t;
    const auto b = transformed(*a, t);
    TransferOptions options;
    options.method = TransferMethod::MarkersAlign;
    options.markers = markersFor(*a, *b, 4);
    const ScalpMapping mapping(*a, *b, options);

    PartSet parts;
    std::vector<Vec3f> line;
    for (int i = 0; i <= 10; ++i) line.push_back(Vec3f(0.0f, std::cos(0.1f * i - 0.5f), std::sin(0.1f * i - 0.5f)) * 12.0f);
    parts.parts.push_back(PartSet::fromPoints("crown", line, *a));
    const PartSet movedParts = parts.transferred(mapping);
    ASSERT_EQ(movedParts.parts.size(), 1u);
    for (std::size_t i = 0; i < movedParts.parts[0].points.size(); ++i) {
        EXPECT_LT((b->position(movedParts.parts[0].points[i]) - t(a->position(parts.parts[0].points[i]))).length(), 0.1f);
    }

    FlowSet flow;
    const SurfacePoint at = a->closestPoint(Vec3f(3, 10, 2));
    flow.pins.push_back(FlowSet::makePin(*a, at, Vec3f(0, 0, 1)));
    FlowSink sink;
    sink.root = a->closestPoint(Vec3f(0, 12, 0)).root;
    sink.radius = 2.0f;
    flow.sinks.push_back(sink);
    const FlowSet moved = flow.transferred(mapping);
    ASSERT_EQ(moved.pins.size(), 1u);
    ASSERT_EQ(moved.sinks.size(), 1u);
    EXPECT_NEAR(moved.sinks[0].radius, 3.0f, 0.01f);
    const FlowField was(flow, *a), now(moved, *b);
    EXPECT_GT((t.turn * was.directions()[0]).dot(now.directions()[0]), 0.99f) << "the arrow turned with the head";
}

TEST(TransferMethods, DegenerateMarkersSayWhy) {
    const auto a = headA();
    const auto b = transformed(*a, Moved());
    TransferOptions options;
    options.method = TransferMethod::MarkersAlign;
    options.markers = markersFor(*a, *b, 2);
    EXPECT_THROW(ScalpMapping(*a, *b, options), std::invalid_argument) << "fewer than 3";
    try {
        ScalpMapping(*a, *b, options);
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("at least 3"), std::string::npos) << e.what();
    }
    // A duplicate pair.
    options.markers = markersFor(*a, *b, 4);
    options.markers.push_back(options.markers[1]);
    try {
        ScalpMapping(*a, *b, options);
        ADD_FAILURE() << "duplicates accepted";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("markers 2 and 5"), std::string::npos) << e.what();
    }
    // Three on a line (points given by position alone).
    options.markers.clear();
    for (int i = 0; i < 3; ++i) {
        TransferMarker m;
        m.oldPoint = Vec3f(-5.0f + 5.0f * i, 0, 10);
        m.newPoint = Vec3f(-5.0f + 5.0f * i, 0, 10) * 1.5f;
        options.markers.push_back(m);
    }
    try {
        ScalpMapping(*a, *b, options);
        ADD_FAILURE() << "collinear markers accepted";
    } catch (const std::invalid_argument& e) {
        EXPECT_NE(std::string(e.what()).find("one line"), std::string::npos) << e.what();
    }
    // The warp with 3 pairs, or with pairs on one plane: aligned instead, with a warning.
    options.method = TransferMethod::MarkersWarp;
    options.markers = markersFor(*a, *b, 3);
    std::vector<std::string> warnings;
    EXPECT_EQ(ScalpMapping(*a, *b, options, &warnings).method(), TransferMethod::MarkersAlign);
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("aligned by the markers instead"), std::string::npos);
    options.markers.clear();
    for (const Vec3f& p : {Vec3f(9, 0, 0), Vec3f(-9, 0, 0), Vec3f(0, 0, 10), Vec3f(0, 0, -10), Vec3f(6, 0, 7)}) {
        TransferMarker m;
        m.oldPoint = a->closestPoint(p).position;
        m.newPoint = Moved()(m.oldPoint);
        options.markers.push_back(m);
    }
    warnings.clear();
    EXPECT_EQ(ScalpMapping(*a, *b, options, &warnings).method(), TransferMethod::MarkersAlign);
    EXPECT_EQ(warnings.size(), 1u);
}

TEST(TransferMethods, MarkersRoundTripThroughJsonAndGroomFiles) {
    const auto a = headA();
    const auto b = transformed(*a, Moved());
    std::vector<TransferMarker> markers = markersFor(*a, *b, 4);
    TransferMarker byPoint;  // a side by position alone
    byPoint.oldPoint = Vec3f(1, 2, 3);
    byPoint.newPoint = Vec3f(4, 5, 6);
    markers.push_back(byPoint);
    const std::vector<TransferMarker> back = transferMarkersFromJson(transferMarkersToJson(markers));
    ASSERT_EQ(back.size(), markers.size());
    for (std::size_t i = 0; i < back.size(); ++i) {
        EXPECT_EQ(back[i].oldRoot.face, markers[i].oldRoot.face);
        EXPECT_EQ(back[i].newRoot.u, markers[i].newRoot.u);
        EXPECT_EQ(back[i].newPoint, markers[i].newPoint);
    }
    EXPECT_TRUE(transferMarkersFromJson("  ").empty());
    EXPECT_THROW(transferMarkersFromJson("{\"old\": 1}"), std::invalid_argument);

    // Snapping fills the roots from the points.
    const std::vector<TransferMarker> snapped = snapTransferMarkers({byPoint}, *a, *b);
    EXPECT_GE(snapped[0].oldRoot.face, 0);
    EXPECT_LT((snapped[0].newPoint - b->closestPoint(byPoint.newPoint).position).length(), 1e-4f);

    GroomFile file;
    file.data.scalp = a;
    EXPECT_EQ(groomFileToJson(file).find("transferMarkers"), std::string::npos) << "absent when none";
    file.transferMarkers = markers;
    const GroomFile read = groomFileFromJson(groomFileToJson(file));
    ASSERT_EQ(read.transferMarkers.size(), markers.size());
    EXPECT_EQ(read.transferMarkers[2].oldPoint, markers[2].oldPoint);
    EXPECT_EQ(read.transferMarkers[2].oldRoot.face, markers[2].oldRoot.face);
}

TEST(TransferMethods, MarkersFollowTheirMeshesByRoot) {
    // Stored by face and barycentrics, a marker side follows its mesh when the mesh moves.
    const auto a = headA();
    const auto b = transformed(*a, Moved());
    std::vector<TransferMarker> markers = markersFor(*a, *b, 4);
    const auto b2 = transformed(*b, [](const Vec3f& p) { return p + Vec3f(100, 0, 0); });
    EXPECT_LT((markerNewPosition(markers[0], *b2) - (markers[0].newPoint + Vec3f(100, 0, 0))).length(), 1e-3f);
}

}  // namespace
}  // namespace groomlab
