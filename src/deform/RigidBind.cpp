#include "deform/RigidBind.h"

#include "core/Profile.h"
#include "deform/DeformUtil.h"
#include "deform/EigenGlm.h"

#include <Eigen/Eigenvalues>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>

namespace lila::deform
{
namespace
{
/** Thin plates still have some depth to collide with, as a fraction of their largest half extent. */
constexpr double kMinDepth = 0.1;

struct Gap
{
    double value = std::numeric_limits<double>::max();
    glm::dvec3 dir{0.0};  // pushes the first piece away from the second
    glm::dvec3 soft{0.0}; // dir blended across the near-best axes, so it turns smoothly as the pieces do
};

/** A piece's current placement: rest point x goes to rotation * (x - centroid) + position. */
struct Pose
{
    glm::dmat3 rotation{1.0};
    glm::dvec3 position{0.0};
};

/** The box around ids in axes: its corners' low and high corner in those axes. Returns its volume. */
double boxIn(std::span<const glm::dvec3> rest, std::span<const uint32_t> ids, const glm::dvec3& c, const glm::dmat3& axes, glm::dvec3& lo, glm::dvec3& hi)
{
    lo = glm::dvec3(std::numeric_limits<double>::max());
    hi = -lo;
    const glm::dmat3 t = glm::transpose(axes);
    for (uint32_t p : ids)
    {
        glm::dvec3 q = t * (rest[p] - c);
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    glm::dvec3 size = hi - lo;
    return size.x * size.y * size.z + 1e-9 * (size.x * size.y + size.y * size.z + size.z * size.x);
}

/**
 * A tight oriented box: principal axes and the rest axes as starting frames, each refined by turning about its own
 * axes in 3 degree steps. Principal axes alone are arbitrary where spreads are equal (a cube, a round scale).
 */
glm::dmat3 tightAxes(std::span<const glm::dvec3> rest, std::span<const uint32_t> ids, const glm::dvec3& c, const glm::dmat3& principal)
{
    glm::dmat3 best = principal;
    glm::dvec3 lo, hi;
    double bestVolume = std::numeric_limits<double>::max();
    for (glm::dmat3 frame : {principal, glm::dmat3(1.0)})
    {
        double volume = boxIn(rest, ids, c, frame, lo, hi);
        for (int pass = 0; pass < 2; ++pass)
            for (int axis = 0; axis < 3; ++axis)
            {
                glm::dmat3 start = frame;
                for (int step = 1; step < 30; ++step)
                {
                    const double a = glm::radians(3.0 * step);
                    const glm::dvec3 u = start[(axis + 1) % 3], v = start[(axis + 2) % 3];
                    glm::dmat3 turned = start;
                    turned[(axis + 1) % 3] = u * std::cos(a) + v * std::sin(a);
                    turned[(axis + 2) % 3] = v * std::cos(a) - u * std::sin(a);
                    if (double vol = boxIn(rest, ids, c, turned, lo, hi); vol < volume * (1.0 - 1e-9))
                        volume = vol, frame = turned;
                }
            }
        if (volume < bestVolume)
            bestVolume = volume, best = frame;
    }
    return best;
}

glm::dvec3 place(const RigidBindRig& rig, const Pose& pose, size_t piece, const glm::dvec3& rest)
{
    return pose.rotation * (rest - rig.centroid[piece]) + pose.position;
}

/** A capsule's segment half length and radius from its box half extents, along its longest axis k. */
int capsuleOf(const glm::dvec3& e, double& half, double& r)
{
    int k = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
    r = std::max(e[(k + 1) % 3], e[(k + 2) % 3]);
    half = std::max(e[k] - r, 0.0);
    return k;
}

/** Closest points of segments p0..p1 and q0..q1 (Ericson, Real-Time Collision Detection 5.1.9). */
void closestOnSegments(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& q0, const glm::dvec3& q1, glm::dvec3& cp, glm::dvec3& cq)
{
    glm::dvec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    double s = 0.0, t = 0.0;
    if (a <= 1e-24 && e <= 1e-24)
        s = t = 0.0;
    else if (a <= 1e-24)
        t = std::clamp(f / e, 0.0, 1.0);
    else
    {
        double c = glm::dot(d1, r);
        if (e <= 1e-24)
            s = std::clamp(-c / a, 0.0, 1.0);
        else
        {
            double b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom > 1e-24 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0)
                t = 0.0, s = std::clamp(-c / a, 0.0, 1.0);
            else if (t > 1.0)
                t = 1.0, s = std::clamp((b - c) / a, 0.0, 1.0);
        }
    }
    cp = p0 + d1 * s;
    cq = q0 + d2 * t;
}

/**
 * The gap between the collision shapes of pieces a and b (negative where they overlap), with the direction pushing a
 * away from b. Boxes by separating axes, whose gap is exact when overlapping and a lower bound apart.
 */
Gap separation(const RigidBindRig& rig, const std::vector<Pose>& pose, size_t a, size_t b)
{
    const glm::dmat3 fa = pose[a].rotation * rig.axes[a], fb = pose[b].rotation * rig.axes[b];
    const glm::dvec3 ca = place(rig, pose[a], a, rig.shapeCenter[a]), cb = place(rig, pose[b], b, rig.shapeCenter[b]);
    const glm::dvec3 &ea = rig.extent[a], &eb = rig.extent[b];
    const glm::dvec3 between = ca - cb;
    Gap g;
    auto fallback = [&] {
        double l = glm::length(between);
        return l > 1e-12 ? between / l : fa[2];
    };
    switch (rig.shape)
    {
    case RigidShape::Sphere:
    {
        g.value = glm::length(between) - rig.radius[a] - rig.radius[b];
        g.dir = g.soft = fallback();
        return g;
    }
    case RigidShape::Capsule:
    {
        double ha, ra, hb, rb;
        int ka = capsuleOf(ea, ha, ra), kb = capsuleOf(eb, hb, rb);
        glm::dvec3 pa, pb;
        closestOnSegments(ca - fa[ka] * ha, ca + fa[ka] * ha, cb - fb[kb] * hb, cb + fb[kb] * hb, pa, pb);
        glm::dvec3 d = pa - pb;
        double l = glm::length(d);
        g.value = l - ra - rb;
        g.dir = g.soft = l > 1e-12 ? d / l : fallback();
        return g;
    }
    default:
    {
        // Separating axes: the faces of both boxes. Edge pair axes are left out, as their direction swings wildly where
        // edges turn near parallel; without them the gap is a little conservative at edges but changes smoothly.
        std::array<std::pair<double, glm::dvec3>, 6> axes;
        g.value = -std::numeric_limits<double>::max();
        for (int k = 0; k < 6; ++k)
        {
            const glm::dvec3 l = k < 3 ? fa[k] : fb[k - 3];
            double radius = 0.0;
            for (int m = 0; m < 3; ++m)
                radius += ea[m] * std::abs(glm::dot(fa[m], l)) + eb[m] * std::abs(glm::dot(fb[m], l));
            const double along = glm::dot(between, l);
            axes[size_t(k)] = {std::abs(along) - radius, along < 0.0 ? -l : l};
            if (axes[size_t(k)].first > g.value)
                g.value = axes[size_t(k)].first, g.dir = axes[size_t(k)].second;
        }
        const double width = 0.1 * (ea.x + ea.y + ea.z + eb.x + eb.y + eb.z) / 6.0 + 1e-12;
        for (const auto& [gap, dir] : axes)
            g.soft += dir * std::exp((gap - g.value) / width);
        const double l = glm::length(g.soft);
        g.soft = l > 1e-12 ? g.soft / l : g.dir;
        return g;
    }
    }
}

/** The shortest rotation taking unit a onto unit b. */
glm::dquat arc(const glm::dvec3& a, const glm::dvec3& b)
{
    double c = glm::dot(a, b);
    if (c < -1.0 + 1e-12)
    {
        glm::dvec3 axis = glm::cross(a, std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0));
        return glm::angleAxis(glm::pi<double>(), glm::normalize(axis));
    }
    glm::dvec3 x = glm::cross(a, b);
    return glm::normalize(glm::dquat(1.0 + c, x.x, x.y, x.z));
}

/** Each piece's weighted surface normal from its points' driver triangles. */
std::vector<glm::dvec3> pieceNormals(const ProximityBinding& b, const RigidBindRig& rig, std::span<const std::span<const glm::dvec3>> drivers)
{
    std::vector<glm::dvec3> out(rig.pieces(), glm::dvec3(0.0));
    parallelPoints(rig.pieces(), [&](size_t i) {
        glm::dvec3 sum(0.0);
        for (uint32_t k = rig.offsets[i]; k < rig.offsets[i + 1]; ++k)
        {
            const uint32_t p = rig.points[k];
            if (p + 1 >= b.offsets.size())
                continue;
            for (uint32_t f = b.offsets[p]; f < b.offsets[p + 1]; ++f)
            {
                const ProximityBinding::Influence& inf = b.influences[f];
                if (inf.driver >= drivers.size())
                    continue;
                std::span<const glm::dvec3> d = drivers[inf.driver];
                if (inf.tri[0] >= d.size() || inf.tri[1] >= d.size() || inf.tri[2] >= d.size())
                    continue;
                sum += triangleFrame(d[inf.tri[0]], d[inf.tri[1]], d[inf.tri[2]])[2] * inf.weight;
            }
        }
        double l = glm::length(sum);
        out[i] = l > 1e-12 ? sum / l : rig.axes[i][2];
    });
    return out;
}

/** The mean of a piece's bound surface points: at rest (drivers empty) or on the drivers now. */
glm::dvec3 surfaceCentre(const ProximityBinding& b, const RigidBindRig& rig, size_t piece, std::span<const std::span<const glm::dvec3>> drivers)
{
    glm::dvec3 sum(0.0);
    double w = 0.0;
    for (uint32_t k = rig.offsets[piece]; k < rig.offsets[piece + 1]; ++k)
    {
        const uint32_t p = rig.points[k];
        if (p + 1 >= b.offsets.size())
            continue;
        for (uint32_t f = b.offsets[p]; f < b.offsets[p + 1]; ++f)
        {
            const ProximityBinding::Influence& inf = b.influences[f];
            if (drivers.empty())
            {
                sum += inf.anchor * inf.weight;
                w += inf.weight;
                continue;
            }
            if (inf.driver >= drivers.size())
                continue;
            std::span<const glm::dvec3> d = drivers[inf.driver];
            if (inf.tri[0] >= d.size() || inf.tri[1] >= d.size() || inf.tri[2] >= d.size())
                continue;
            sum += (d[inf.tri[0]] * inf.bary.x + d[inf.tri[1]] * inf.bary.y + d[inf.tri[2]] * inf.bary.z) * inf.weight;
            w += inf.weight;
        }
    }
    return w > 0.0 ? sum / w : rig.centroid[piece];
}

/**
 * The lowest height of a piece's outermost points above the plane of the surface under it. A plane rather than the
 * triangles themselves, so it changes smoothly where the driver folds or collapses (the inside of a bent elbow).
 */
double lowestHeight(const RigidBindRig& rig, std::span<const glm::dvec3> rest, const Pose& pose, size_t piece, const glm::dvec3& centre, const glm::dvec3& n)
{
    double low = std::numeric_limits<double>::max();
    for (uint32_t k = rig.sampleOffsets[piece]; k < rig.sampleOffsets[piece + 1]; ++k)
        low = std::min(low, glm::dot(place(rig, pose, piece, rest[rig.samples[k]]) - centre, n));
    return low;
}

/**
 * Holds a piece within reach (a fraction of its size), maxSlide along the surface and maxLift off it, measured from
 * its wrapped place.
 */
glm::dvec3 limit(const glm::dvec3& pos, const glm::dvec3& anchor, const glm::dvec3& n, double radius, const RigidBindParams& o)
{
    glm::dvec3 v = pos - anchor;
    if (o.reach > 0.0)
        if (double l = glm::length(v), most = o.reach * radius; l > most)
            v *= most / l;
    double lift = glm::dot(v, n);
    glm::dvec3 slide = v - n * lift;
    if (o.maxSlide > 0.0)
        if (double l = glm::length(slide); l > o.maxSlide)
            slide *= o.maxSlide / l;
    if (o.maxLift > 0.0)
        lift = std::clamp(lift, -o.maxLift, o.maxLift);
    return anchor + slide + n * lift;
}
} // namespace

RigidBindRig rigidBindRig(std::span<const glm::dvec3> rest, std::span<const int32_t> piece, RigidShape shape, double scale)
{
    LILA_ZONE();

    RigidBindRig rig;
    rig.shape = shape;
    rig.scale = std::max(scale, 1e-6);
    std::map<int32_t, uint32_t> dense;
    const size_t n = std::min(rest.size(), piece.size());
    for (size_t p = 0; p < n; ++p)
        if (piece[p] >= 0)
            dense.emplace(piece[p], 0);
    uint32_t next = 0;
    for (auto& [id, d] : dense)
        d = next++;
    std::vector<uint32_t> count(next, 0);
    for (size_t p = 0; p < n; ++p)
        if (piece[p] >= 0)
            ++count[dense[piece[p]]];
    rig.offsets.assign(next + 1, 0);
    for (uint32_t i = 0; i < next; ++i)
        rig.offsets[i + 1] = rig.offsets[i] + count[i];
    rig.points.resize(rig.offsets.back());
    std::vector<uint32_t> fill(rig.offsets.begin(), rig.offsets.end() - 1);
    for (size_t p = 0; p < n; ++p)
        if (piece[p] >= 0)
            rig.points[fill[dense[piece[p]]]++] = uint32_t(p);

    rig.centroid.resize(next);
    rig.shapeCenter.resize(next);
    rig.axes.resize(next);
    rig.extent.resize(next);
    rig.radius.resize(next);
    std::vector<std::vector<uint32_t>> samples(next);
    parallelPoints(next, [&](size_t i) {
        std::span<const uint32_t> ids(rig.points.data() + rig.offsets[i], rig.offsets[i + 1] - rig.offsets[i]);
        glm::dvec3 c(0.0);
        for (uint32_t p : ids)
            c += rest[p];
        c /= double(ids.size());
        Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
        for (uint32_t p : ids)
        {
            Eigen::Vector3d d(rest[p].x - c.x, rest[p].y - c.y, rest[p].z - c.z);
            cov += d * d.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(cov);
        glm::dmat3 axes = toGlm(eig.eigenvectors());
        if (glm::determinant(axes) < 0.0)
            axes[2] = -axes[2];
        axes = tightAxes(rest, ids, c, axes);
        // Longest side first and thinnest last, so a plate's thin axis is z.
        glm::dvec3 lo, hi;
        boxIn(rest, ids, c, axes, lo, hi);
        std::array<int, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](int x, int y) { return hi[x] - lo[x] > hi[y] - lo[y]; });
        axes = glm::dmat3(axes[order[0]], axes[order[1]], axes[order[2]]);
        if (glm::determinant(axes) < 0.0)
            axes[2] = -axes[2];
        boxIn(rest, ids, c, axes, lo, hi);
        glm::dvec3 half = (hi - lo) * 0.5;
        const double big = std::max({half.x, half.y, half.z});
        half = glm::max(half, glm::dvec3(big * kMinDepth));
        rig.centroid[i] = c;
        rig.shapeCenter[i] = c + axes * ((hi + lo) * 0.5);
        rig.axes[i] = axes;
        rig.extent[i] = half * rig.scale;
        double r = 0.0;
        for (uint32_t p : ids)
            r = std::max(r, glm::length(rest[p] - rig.shapeCenter[i]));
        rig.radius[i] = std::max(r, big * kMinDepth) * rig.scale;

        // The extreme point along each of 13 directions and their opposites stands in for the piece's hull.
        std::vector<uint32_t>& s = samples[i];
        for (int a = -1; a <= 1; ++a)
            for (int b = -1; b <= 1; ++b)
                for (int d = -1; d <= 1; ++d)
                {
                    if (a == 0 && b == 0 && d == 0)
                        continue;
                    glm::dvec3 dir = axes * glm::dvec3(a, b, d);
                    uint32_t best = ids.front();
                    double bestDot = -std::numeric_limits<double>::max();
                    for (uint32_t p : ids)
                        if (double v = glm::dot(rest[p] - c, dir); v > bestDot)
                            bestDot = v, best = p;
                    s.push_back(best);
                }
        std::sort(s.begin(), s.end());
        s.erase(std::unique(s.begin(), s.end()), s.end());
    });
    for (uint32_t i = 0; i < next; ++i)
    {
        rig.samples.insert(rig.samples.end(), samples[i].begin(), samples[i].end());
        rig.sampleOffsets.push_back(uint32_t(rig.samples.size()));
    }

    if (next == 0)
        return rig;
    double maxRadius = *std::max_element(rig.radius.begin(), rig.radius.end());
    PointGrid grid(rig.shapeCenter);
    std::vector<std::vector<uint32_t>> nbs(next);
    std::vector<std::vector<double>> gaps(next);
    std::vector<Pose> restPose(next);
    for (uint32_t i = 0; i < next; ++i)
        restPose[i].position = rig.centroid[i];
    constexpr size_t kMaxNeighbours = 24;
    parallelPoints(next, [&](size_t i) {
        std::vector<uint32_t> near;
        grid.within(rig.shapeCenter[i], (rig.radius[i] + maxRadius) * 1.05, near);
        std::vector<std::pair<double, uint32_t>> touching;
        for (uint32_t j : near)
        {
            if (j == i)
                continue;
            double d = glm::length(rig.shapeCenter[j] - rig.shapeCenter[i]);
            if (d < (rig.radius[i] + rig.radius[j]) * 1.05)
                touching.push_back({d, j});
        }
        std::sort(touching.begin(), touching.end());
        if (touching.size() > kMaxNeighbours)
            touching.resize(kMaxNeighbours);
        for (const auto& [d, j] : touching)
        {
            nbs[i].push_back(j);
            gaps[i].push_back(shape == RigidShape::Off ? 0.0 : separation(rig, restPose, i, j).value);
        }
    });
    for (uint32_t i = 0; i < next; ++i)
    {
        rig.nb.insert(rig.nb.end(), nbs[i].begin(), nbs[i].end());
        rig.restGap.insert(rig.restGap.end(), gaps[i].begin(), gaps[i].end());
        rig.nbOffsets.push_back(uint32_t(rig.nb.size()));
    }
    return rig;
}

RigidBindBinding rigidBindSetup(std::span<const geo::Mesh* const> drivers, std::span<const glm::dvec3> rest, const RigidBindRig& rig, const ProximityParams& o)
{
    LILA_ZONE();

    RigidBindBinding b;
    b.wrap = proximityBind(drivers, rest, o);
    std::vector<std::span<const glm::dvec3>> pts;
    for (const geo::Mesh* d : drivers)
        pts.push_back(d->positions());
    b.restNormal = pieceNormals(b.wrap, rig, pts);
    b.restHeight.assign(rig.pieces(), 0.0);
    parallelPoints(rig.pieces(), [&](size_t i) {
        b.restHeight[i] = lowestHeight(rig, rest, Pose{glm::dmat3(1.0), rig.centroid[i]}, i, surfaceCentre(b.wrap, rig, i, {}), b.restNormal[i]);
    });
    return b;
}

std::vector<glm::dvec3> rigidBindApply(const RigidBindBinding& b, const RigidBindRig& rig, std::span<const std::span<const glm::dvec3>> drivers, std::span<const glm::dvec3> rest, const RigidBindParams& o,
                                       std::vector<RigidFit>* transforms)
{
    LILA_ZONE();

    std::vector<glm::dvec3> out = proximityApply(b.wrap, drivers, rest, WrapMode::Offset, 0.0);
    const size_t n = rig.pieces();
    if (b.restNormal.size() != n || b.restHeight.size() != n)
        throw std::runtime_error("the rigid bind binding doesn't match its pieces: run rebindDeformer");

    // Each piece's surface normal: its rest normal turned with it, which changes smoothly even where the triangles
    // under it fold over.
    std::vector<glm::dvec3> normal(n);
    std::vector<Pose> pose(n);
    std::vector<glm::dquat> turn(n);
    std::vector<glm::dvec3> anchor(n);
    // Each piece's bound surface points at rest and now, for the turn of the surface around it.
    std::vector<std::vector<glm::dvec3>> surfRest, surfNow;
    const double smoothing = std::clamp(o.rotationSmoothing, 0.0, 1.0);
    if (smoothing > 0.0)
    {
        surfRest.resize(n);
        surfNow.resize(n);
        parallelPoints(n, [&](size_t i) {
            for (uint32_t k = rig.offsets[i]; k < rig.offsets[i + 1]; ++k)
            {
                const uint32_t p = rig.points[k];
                if (p + 1 >= b.wrap.offsets.size())
                    continue;
                for (uint32_t f = b.wrap.offsets[p]; f < b.wrap.offsets[p + 1]; ++f)
                {
                    const ProximityBinding::Influence& inf = b.wrap.influences[f];
                    if (inf.restArea <= 0.0 || inf.driver >= drivers.size())
                        continue;
                    std::span<const glm::dvec3> d = drivers[inf.driver];
                    if (inf.tri[0] >= d.size() || inf.tri[1] >= d.size() || inf.tri[2] >= d.size())
                        continue;
                    surfRest[i].push_back(inf.anchor);
                    surfNow[i].push_back(d[inf.tri[0]] * inf.bary.x + d[inf.tri[1]] * inf.bary.y + d[inf.tri[2]] * inf.bary.z);
                }
            }
        });
    }
    parallelPoints(n, [&](size_t i) {
        std::span<const uint32_t> ids(rig.points.data() + rig.offsets[i], rig.offsets[i + 1] - rig.offsets[i]);
        RigidFit fit = rigidFit(rest, out, ids);
        glm::dquat q = glm::quat_cast(fit.rotation);
        if (smoothing > 0.0)
        {
            // The turn of the surface under this piece and its neighbours: where a small patch folds or collapses
            // (the inside of a bent elbow), the wider surface still turns sensibly.
            std::vector<glm::dvec3> from(surfRest[i]), to(surfNow[i]);
            for (uint32_t k = rig.nbOffsets[i]; k < rig.nbOffsets[i + 1]; ++k)
            {
                from.insert(from.end(), surfRest[rig.nb[k]].begin(), surfRest[rig.nb[k]].end());
                to.insert(to.end(), surfNow[rig.nb[k]].begin(), surfNow[rig.nb[k]].end());
            }
            if (from.size() >= 3)
            {
                glm::dquat region = glm::quat_cast(rigidFit(from, to).rotation);
                q = glm::slerp(q, glm::dot(q, region) < 0.0 ? -region : region, smoothing);
            }
        }
        normal[i] = glm::normalize(q * b.restNormal[i]);
        if (o.tangentBlend > 0.0)
            q = glm::slerp(q, arc(b.restNormal[i], normal[i]), std::clamp(o.tangentBlend, 0.0, 1.0));
        q = glm::slerp(glm::dquat(1, 0, 0, 0), q, std::clamp(o.rotationStrength, 0.0, 1.0));
        turn[i] = glm::normalize(q);
        anchor[i] = fit.to + normal[i] * o.normalOffset;
        pose[i].position = anchor[i];
    });

    for (size_t i = 0; i < n; ++i)
        pose[i].rotation = glm::mat3_cast(turn[i]);

    const double attach = std::clamp(o.attachment, 0.0, 1.0);
    std::vector<glm::dvec3> next(n);
    if (o.relaxation > 0.0)
        for (int it = 0; it < o.relaxIterations; ++it)
        {
            parallelPoints(n, [&](size_t i) {
                glm::dvec3 target(0.0);
                const uint32_t count = rig.nbOffsets[i + 1] - rig.nbOffsets[i];
                for (uint32_t k = rig.nbOffsets[i]; k < rig.nbOffsets[i + 1]; ++k)
                {
                    const uint32_t j = rig.nb[k];
                    target += pose[j].position + pose[j].rotation * (rig.centroid[i] - rig.centroid[j]);
                }
                glm::dvec3 p = pose[i].position;
                if (count > 0)
                {
                    glm::dvec3 d = (target / double(count) - p) * std::clamp(o.relaxation, 0.0, 1.0);
                    p += d - normal[i] * glm::dot(d, normal[i]);
                }
                p += (anchor[i] - p) * attach;
                next[i] = limit(p, anchor[i], normal[i], rig.radius[i] / rig.scale, o);
            });
            for (size_t i = 0; i < n; ++i)
                pose[i].position = next[i];
        }

    std::vector<glm::dvec3> centre;
    if (o.collideDriver)
    {
        centre.resize(n);
        parallelPoints(n, [&](size_t i) { centre[i] = surfaceCentre(b.wrap, rig, i, drivers); });
    }
    // Lifts a piece placed at p out of the drivers until it sits no deeper than at rest.
    auto ground = [&](size_t i, const glm::dvec3& p) {
        if (!o.collideDriver)
            return p;
        const double h = lowestHeight(rig, rest, Pose{pose[i].rotation, p}, i, centre[i], normal[i]);
        return h < b.restHeight[i] ? p + normal[i] * (b.restHeight[i] - h) : p;
    };

    if (rig.shape != RigidShape::Off && o.collisionStrength > 0.0)
    {
        const double lift = std::clamp(o.collisionLift, 0.0, 1.0);
        // How far each piece sat above each neighbour along their normals at rest.
        std::vector<double> restAbove(rig.nb.size(), 0.0);
        for (size_t i = 0; i < n; ++i)
            for (uint32_t k = rig.nbOffsets[i]; k < rig.nbOffsets[i + 1]; ++k)
            {
                const uint32_t j = rig.nb[k];
                glm::dvec3 nn = b.restNormal[i] + b.restNormal[j];
                double l = glm::length(nn);
                restAbove[k] = l > 1e-12 ? glm::dot(rig.shapeCenter[i] - rig.shapeCenter[j], nn / l) : 0.0;
            }
        const double tiny = 1e-9 * (1.0 + (n ? rig.radius[0] : 0.0));
        for (int it = 0; it < o.collisionIterations; ++it)
        {
            bool moved = false;
            parallelPoints(n, [&](size_t i) {
                glm::dvec3 push(0.0);
                double weight = 0.0;
                for (uint32_t k = rig.nbOffsets[i]; k < rig.nbOffsets[i + 1]; ++k)
                {
                    const uint32_t j = rig.nb[k];
                    const double allowed = std::min(rig.restGap[k], o.thickness);
                    Gap g = separation(rig, pose, i, j);
                    if (g.value >= allowed || glm::length(g.dir) == 0.0)
                        continue;
                    double depth = allowed - g.value;
                    if (const double soft = o.collisionSoftness * rig.radius[i] / rig.scale; soft > 0.0)
                        depth *= depth / (depth + soft);
                    depth *= std::clamp(o.collisionStrength, 0.0, 1.0);
                    const glm::dvec3& nn = normal[i];
                    // Slide apart along the surface, away from the neighbour: the contact normal often points off the
                    // surface where corners meet, and moving that way doesn't separate pieces lying side by side.
                    glm::dvec3 away = pose[i].position - pose[j].position;
                    away -= nn * glm::dot(away, nn);
                    if (glm::length(away) < 1e-12)
                        away = g.dir - nn * glm::dot(g.dir, nn);
                    const double la = glm::length(away);
                    away = la > 1e-12 ? away / la : g.dir;
                    // Sliding at an angle to the contact normal has to go farther to clear the same depth.
                    const double slide = depth / std::max(glm::dot(away, g.soft), 0.25);
                    const double above = restAbove[k];
                    // The upper piece climbs over the lower one; the lower one only slides. Weighted by depth, so a
                    // contact that is just starting changes nothing and pieces never jump between frames.
                    glm::dvec3 move;
                    if (above > tiny)
                        move = away * (0.5 * slide * (1.0 - lift)) + nn * (depth * lift);
                    else if (above < -tiny)
                        move = away * (0.5 * slide * (1.0 - lift));
                    else
                        move = away * (0.5 * slide);
                    push += move * depth;
                    weight += depth;
                }
                glm::dvec3 p = pose[i].position;
                if (weight > 0.0)
                    p += push / weight;
                p += (anchor[i] - p) * attach;
                next[i] = limit(ground(i, p), anchor[i], normal[i], rig.radius[i] / rig.scale, o);
            });
            for (size_t i = 0; i < n; ++i)
            {
                moved |= next[i] != pose[i].position;
                pose[i].position = next[i];
            }
            if (!moved)
                break;
        }
    }

    if (o.collideDriver)
    {
        parallelPoints(n, [&](size_t i) { next[i] = limit(ground(i, pose[i].position), anchor[i], normal[i], rig.radius[i] / rig.scale, o); });
        for (size_t i = 0; i < n; ++i)
            pose[i].position = next[i];
    }

    parallelPoints(n, [&](size_t i) {
        for (uint32_t k = rig.offsets[i]; k < rig.offsets[i + 1]; ++k)
        {
            const uint32_t p = rig.points[k];
            out[p] = place(rig, pose[i], i, rest[p]);
        }
    });
    if (transforms)
    {
        transforms->resize(n);
        for (size_t i = 0; i < n; ++i)
            (*transforms)[i] = {pose[i].rotation, rig.centroid[i], pose[i].position};
    }
    return out;
}
} // namespace lila::deform
