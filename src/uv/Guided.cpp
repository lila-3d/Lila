#include "uv/Guided.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"
#include "uv/Chart.h"
#include "uv/Seams.h"
#include "uv/Unwrap.h"

#include <Eigen/Dense>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <span>

namespace lila::uv
{
namespace
{
using Pair = std::pair<glm::dvec3, glm::dvec3>; // (moving point, fixed point)

std::vector<glm::dvec3> samplePoints(const geo::Mesh& m, size_t maxCount)
{
    std::vector<glm::dvec3> all;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            all.push_back(m.position(p));
    if (all.size() <= maxCount)
        return all;
    std::vector<glm::dvec3> out;
    const double step = double(all.size()) / double(maxCount);
    for (size_t i = 0; i < maxCount; ++i)
        out.push_back(all[size_t(double(i) * step)]);
    return out;
}

struct Frame
{
    glm::dvec3 mean{0.0};
    glm::dmat3 axes{1.0}; // columns, largest spread first
    glm::dvec3 spread{1.0};
};

Frame principalFrame(const std::vector<glm::dvec3>& pts)
{
    Frame f;
    if (pts.empty())
        return f;
    for (const glm::dvec3& p : pts)
        f.mean += p;
    f.mean /= double(pts.size());
    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    for (const glm::dvec3& p : pts)
    {
        Eigen::Vector3d d(p.x - f.mean.x, p.y - f.mean.y, p.z - f.mean.z);
        cov += d * d.transpose();
    }
    cov /= double(pts.size());
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    for (int i = 0; i < 3; ++i)
    {
        Eigen::Vector3d v = es.eigenvectors().col(2 - i);
        f.axes[i] = glm::dvec3(v.x(), v.y(), v.z());
        f.spread[i] = std::sqrt(std::max(es.eigenvalues()(2 - i), 1e-18));
    }
    if (glm::determinant(f.axes) < 0.0)
        f.axes[2] = -f.axes[2];
    return f;
}

Eigen::Matrix3d toEigen(const glm::dmat3& m)
{
    Eigen::Matrix3d e;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            e(r, c) = m[c][r];
    return e;
}

glm::dmat3 toGlm(const Eigen::Matrix3d& e)
{
    glm::dmat3 m;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            m[c][r] = e(r, c);
    return m;
}

glm::dmat4 compose(const glm::dmat3& a, const glm::dvec3& t)
{
    glm::dmat4 m(a);
    m[3] = glm::dvec4(t, 1.0);
    return m;
}

/** Least squares T with T * moving ~ fixed over the pairs, mirroring when asked. */
glm::dmat4 fitPairs(const std::vector<Pair>& pairs, AlignFit fit, bool mirrored)
{
    const double n = double(pairs.size());
    glm::dvec3 ma(0.0), mb(0.0);
    for (const auto& [a, b] : pairs)
    {
        ma += a;
        mb += b;
    }
    ma /= n;
    mb /= n;
    Eigen::Matrix3d cross = Eigen::Matrix3d::Zero(), self = Eigen::Matrix3d::Zero();
    double var = 0.0;
    for (const auto& [a, b] : pairs)
    {
        Eigen::Vector3d da(a.x - ma.x, a.y - ma.y, a.z - ma.z), db(b.x - mb.x, b.y - mb.y, b.z - mb.z);
        cross += db * da.transpose();
        self += da * da.transpose();
        var += da.squaredNorm();
    }
    Eigen::Matrix3d A;
    if (fit == AlignFit::Stretch && std::abs(self.determinant()) > 1e-12 * std::pow(var, 3.0))
    {
        // Affine, then the rotation and symmetric stretch of its polar decomposition, so it never shears.
        A = cross * self.inverse();
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
        Eigen::Matrix3d U = svd.matrixU(), V = svd.matrixV();
        Eigen::Vector3d s = svd.singularValues();
        if (((U * V.transpose()).determinant() < 0.0) != mirrored)
        {
            U.col(2) = -U.col(2);
            s(2) = -s(2);
        }
        s = s.cwiseMax(1e-6 * std::max(s(0), 1e-12));
        A = (U * V.transpose()) * (V * s.asDiagonal() * V.transpose());
    }
    else
    {
        // Umeyama.
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(cross, Eigen::ComputeFullU | Eigen::ComputeFullV);
        Eigen::Matrix3d S = Eigen::Matrix3d::Identity();
        if (((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0.0) != mirrored)
            S(2, 2) = -1.0;
        Eigen::Matrix3d R = svd.matrixU() * S * svd.matrixV().transpose();
        double scale = 1.0;
        if (fit != AlignFit::Rigid && var > 0.0)
            scale = (svd.singularValues().asDiagonal() * S).trace() / var;
        A = R * scale;
    }
    glm::dmat3 a = toGlm(A);
    return compose(a, mb - a * ma);
}

struct Icp
{
    const geo::MeshBvh& movingBvh;
    const geo::MeshBvh& fixedBvh;
    const std::vector<glm::dvec3>& movingPts;
    const std::vector<glm::dvec3>& fixedPts;
    double diagonal = 1.0;

    /** Closest pairs both ways under T, worst tenth of each direction dropped; returns the rms distance. */
    double pairs(const glm::dmat4& T, size_t stride, std::vector<Pair>& out) const
    {
        const glm::dmat4 inv = glm::inverse(T);
        const size_t nm = (movingPts.size() + stride - 1) / stride, nf = (fixedPts.size() + stride - 1) / stride;
        std::vector<Pair> fwd(nm), back(nf);
        std::vector<double> dFwd(nm, -1.0), dBack(nf, -1.0);
        tbb::parallel_for(size_t(0), nm, [&](size_t i) {
            const glm::dvec3& m = movingPts[i * stride];
            geo::BvhClosest c;
            if (fixedBvh.closestPoint(glm::dvec3(T * glm::dvec4(m, 1.0)), c))
            {
                fwd[i] = {m, c.point};
                dFwd[i] = c.distance;
            }
        });
        tbb::parallel_for(size_t(0), nf, [&](size_t i) {
            const glm::dvec3& f = fixedPts[i * stride];
            geo::BvhClosest c;
            if (movingBvh.closestPoint(glm::dvec3(inv * glm::dvec4(f, 1.0)), c))
            {
                back[i] = {c.point, f};
                dBack[i] = glm::length(glm::dvec3(T * glm::dvec4(c.point, 1.0)) - f);
            }
        });
        out.clear();
        double sum = 0.0;
        size_t count = 0;
        auto keep = [&](const std::vector<Pair>& p, const std::vector<double>& d) {
            std::vector<double> sorted;
            for (double x : d)
                if (x >= 0.0)
                    sorted.push_back(x);
            if (sorted.empty())
                return;
            std::sort(sorted.begin(), sorted.end());
            const double cut = sorted[std::min(sorted.size() - 1, size_t(double(sorted.size()) * 0.9))];
            for (size_t i = 0; i < p.size(); ++i)
                if (d[i] >= 0.0 && d[i] <= cut)
                {
                    out.push_back(p[i]);
                    sum += d[i] * d[i];
                    ++count;
                }
        };
        keep(fwd, dFwd);
        keep(back, dBack);
        return count ? std::sqrt(sum / double(count)) / diagonal : std::numeric_limits<double>::max();
    }

    std::pair<glm::dmat4, double> run(glm::dmat4 T, AlignFit fit, int iterations, size_t stride) const
    {
        std::vector<Pair> p;
        double err = pairs(T, stride, p);
        const bool mirrored = glm::determinant(glm::dmat3(T)) < 0.0;
        for (int it = 0; it < iterations && p.size() >= 4; ++it)
        {
            glm::dmat4 next = fitPairs(p, fit, mirrored);
            double e = pairs(next, stride, p);
            if (e > err)
                break;
            const bool settled = err - e < 1e-6;
            T = next;
            err = e;
            if (settled)
                break;
        }
        return {T, err};
    }
};

/** The 24 rotations that permute and negate the axes, and with mirrored the 24 reflections too. */
std::vector<glm::dmat3> axisFrames(bool mirrored)
{
    std::vector<glm::dmat3> out;
    const std::array<std::array<int, 3>, 6> perms = {{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
    for (const auto& p : perms)
        for (int signs = 0; signs < 8; ++signs)
        {
            glm::dmat3 m(0.0);
            for (int i = 0; i < 3; ++i)
                m[i][p[i]] = (signs >> i) & 1 ? -1.0 : 1.0;
            if (mirrored || glm::determinant(m) > 0.0)
                out.push_back(m);
        }
    return out;
}

/** Best rotation, uniform scale and offset taking from onto to (2D, no mirroring). */
void fitSimilarity2d(std::vector<glm::dvec2>& from, const std::vector<glm::dvec2>& to)
{
    const double n = double(from.size());
    glm::dvec2 ma(0.0), mb(0.0);
    for (size_t i = 0; i < from.size(); ++i)
    {
        ma += from[i];
        mb += to[i];
    }
    ma /= n;
    mb /= n;
    // As complex numbers: z = sum(conj(a) * b) / sum(|a|^2) is the rotation and scale.
    double re = 0.0, im = 0.0, norm = 0.0;
    for (size_t i = 0; i < from.size(); ++i)
    {
        glm::dvec2 a = from[i] - ma, b = to[i] - mb;
        re += a.x * b.x + a.y * b.y;
        im += a.x * b.y - a.y * b.x;
        norm += a.x * a.x + a.y * a.y;
    }
    if (norm <= 0.0)
        return;
    re /= norm;
    im /= norm;
    for (glm::dvec2& p : from)
    {
        glm::dvec2 a = p - ma;
        p = mb + glm::dvec2(a.x * re - a.y * im, a.x * im + a.y * re);
    }
}

double signedUvArea(const Chart& ch, const std::vector<glm::dvec2>& uv)
{
    double a = 0.0;
    for (const auto& t : ch.tris)
    {
        glm::dvec2 e1 = uv[t[1]] - uv[t[0]], e2 = uv[t[2]] - uv[t[0]];
        a += 0.5 * (e1.x * e2.y - e1.y * e2.x);
    }
    return a;
}

struct SurfaceHit
{
    Index triangle = kInvalid;
    glm::dvec3 bary{0.0};
    glm::dvec3 point{0.0};
    bool facing = false;
};

/**
 * The nearest source point facing the same way as n: the closest point when it faces n, else the nearest hit along the
 * normal either way that does, else the closest point anyway.
 */
std::optional<SurfaceHit> closestFacing(const geo::MeshBvh& bvh, const geo::Mesh& source, const glm::dvec3& p, const glm::dvec3& n)
{
    auto facing = [&](Index face) { return glm::dot(source.faceNormal(face), n) > 0.3; };
    geo::BvhClosest closest;
    if (!bvh.closestPoint(p, closest))
        return std::nullopt;
    SurfaceHit out{closest.triangle, closest.bary, closest.point, facing(closest.face)};
    if (out.facing)
        return out;
    double bestT = std::numeric_limits<double>::max();
    for (double dir : {-1.0, 1.0})
    {
        geo::BvhHit h;
        if (bvh.raycast(p, n * dir, h, closest.distance * 4.0 + 1e-6) && h.t < bestT && facing(h.face))
        {
            bestT = h.t;
            out = {h.triangle, h.bary, h.point, true};
        }
    }
    return out;
}

/**
 * The target in the source's space pulled snugly onto the source: each point moves toward the nearest source surface
 * facing its way, with the moves smoothed over its neighbours so the shape follows the source without crumpling. outward
 * is -1 when toSource mirrors, as the faces then wind inward.
 */
geo::Mesh wrapOnto(const geo::Mesh& target, const glm::dmat4& toSource, double outward, const geo::Mesh& source, const geo::MeshBvh& bvh,
                   int iterations)
{
    LILA_ZONE();

    geo::Mesh shape = target;
    for (Index p = 0; p < shape.pointCapacity(); ++p)
        if (shape.pointValid(p))
            shape.setPosition(p, glm::dvec3(toSource * glm::dvec4(target.position(p), 1.0)));
    std::vector<glm::dvec3> move(shape.pointCapacity());
    for (int it = 0; it < iterations; ++it)
    {
        const std::vector<glm::vec3> normals = geo::pointNormals(shape);
        tbb::parallel_for(Index(0), shape.pointCapacity(), [&](Index p) {
            move[p] = glm::dvec3(0.0);
            if (!shape.pointValid(p))
                return;
            if (auto hit = closestFacing(bvh, source, shape.position(p), glm::dvec3(normals[p]) * outward))
                move[p] = hit->point - shape.position(p);
        });
        for (int pass = 0; pass < 3; ++pass)
        {
            std::vector<glm::dvec3> next(move.size());
            tbb::parallel_for(Index(0), shape.pointCapacity(), [&](Index p) {
                if (!shape.pointValid(p))
                    return;
                glm::dvec3 sum(0.0);
                int n = 0;
                shape.forEachPointEdge(p, [&](Index e) {
                    sum += move[shape.edgeOtherPoint(e, p)];
                    ++n;
                });
                next[p] = n ? (move[p] + sum / double(n)) * 0.5 : move[p];
            });
            move.swap(next);
        }
        for (Index p = 0; p < shape.pointCapacity(); ++p)
            if (shape.pointValid(p))
                shape.setPosition(p, shape.position(p) + move[p]);
    }
    return shape;
}

/**
 * Lays the chart out like the guide without folds: vertices whose triangles all keep the guide's orientation and roughly
 * its scale are pinned to it and a conformal solve fills in the rest; vertices around any fold left are unpinned and it
 * solves again. False when it still folds.
 */
bool followGuide(Chart& ch, const std::vector<glm::dvec2>& guide)
{
    const size_t T = ch.tris.size();
    std::vector<double> ratio(T, 0.0);
    std::vector<double> sorted;
    for (size_t t = 0; t < T; ++t)
    {
        const auto& tri = ch.tris[t];
        const glm::dvec2 e1 = guide[tri[1]] - guide[tri[0]], e2 = guide[tri[2]] - guide[tri[0]];
        const double a3 = 0.5 * glm::length(glm::cross(ch.pos[tri[1]] - ch.pos[tri[0]], ch.pos[tri[2]] - ch.pos[tri[0]]));
        ratio[t] = a3 > 0.0 ? 0.5 * (e1.x * e2.y - e1.y * e2.x) / a3 : 0.0;
        if (ratio[t] > 0.0)
            sorted.push_back(ratio[t]);
    }
    if (sorted.empty())
        return false;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double typical = sorted[sorted.size() / 2];
    std::vector<uint8_t> pin(ch.vertexCount(), 1);
    for (size_t t = 0; t < T; ++t)
        if (ratio[t] < typical * 0.2 || ratio[t] > typical * 5.0)
            for (Index v : ch.tris[t])
                pin[v] = 0;
    for (int round = 0; round < 8; ++round)
    {
        size_t pins = 0;
        for (uint8_t p : pin)
            pins += p;
        if (pins < 2)
            return false;
        ch.uv.assign(guide.begin(), guide.end());
        ch.pinned = pin;
        if (!lscm(ch, true))
            return false;
        bool folded = false;
        std::vector<uint8_t> next = pin;
        for (const auto& tri : ch.tris)
        {
            const glm::dvec2 e1 = ch.uv[tri[1]] - ch.uv[tri[0]], e2 = ch.uv[tri[2]] - ch.uv[tri[0]];
            if (e1.x * e2.y - e1.y * e2.x <= 0.0)
            {
                folded = true;
                for (Index v : tri)
                    next[v] = 0;
            }
        }
        if (!folded)
            break;
        // Widen the unpinned area by a ring each round.
        pin = next;
        for (const auto& tri : ch.tris)
            if (!next[tri[0]] || !next[tri[1]] || !next[tri[2]])
                for (Index v : tri)
                    pin[v] = 0;
    }
    std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
    return foldedTriangles(ch) == 0;
}

/**
 * Islands with no source counterpart, unwrapped on their own: scaled to the texel density of the rest and stacked beside
 * them, so they never land on top of the guided layout.
 */
void placeUnguided(std::span<Chart> charts, std::span<const uint8_t> unguided)
{
    double uvArea = 0.0, area = 0.0;
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (size_t i = 0; i < charts.size(); ++i)
        if (!unguided[i])
        {
            uvArea += std::abs(charts[i].areaUv());
            area += charts[i].area3d();
            lo = glm::min(lo, charts[i].uvMin());
            hi = glm::max(hi, charts[i].uvMax());
        }
    if (area <= 0.0)
        return;
    const double scale = std::sqrt(uvArea / area);
    const double gap = 0.02 * std::max(hi.x - lo.x, hi.y - lo.y);
    double y = lo.y;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (!unguided[i])
            continue;
        Chart& ch = charts[i];
        const glm::dvec2 from = ch.uvMin();
        for (glm::dvec2& p : ch.uv)
            p = (p - from) * scale + glm::dvec2(hi.x + gap, y);
        y = ch.uvMax().y + gap;
    }
}

/**
 * Stray patches: a connected run of faces whose label already has a bigger run elsewhere, and is under a quarter of it
 * by area, takes the label it shares the longest border with. Thin islands one face wide are left alone, and so are
 * with keepUnlabelled, unlabelled runs of any real size, which become islands of their own.
 */
void cleanLabels(const geo::Mesh& mesh, std::vector<Index>& label, bool keepUnlabelled)
{
    double total = 0.0;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            total += mesh.faceArea(f);
    for (int pass = 0; pass < 4; ++pass)
    {
        std::vector<Index> part(mesh.faceCapacity(), kInvalid);
        std::vector<std::vector<Index>> parts;
        std::vector<double> area;
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
        {
            if (!mesh.faceValid(f) || part[f] != kInvalid)
                continue;
            const Index id = Index(parts.size());
            parts.emplace_back();
            area.push_back(0.0);
            std::vector<Index> stack = {f};
            part[f] = id;
            while (!stack.empty())
            {
                Index g = stack.back();
                stack.pop_back();
                parts[id].push_back(g);
                area[id] += mesh.faceArea(g);
                mesh.forEachFaceCorner(g, [&](Index c) {
                    Index o = mesh.oppositeFace(mesh.cornerEdge(c), g);
                    if (o != kInvalid && part[o] == kInvalid && label[o] == label[g])
                    {
                        part[o] = id;
                        stack.push_back(o);
                    }
                });
            }
        }
        std::map<Index, double> largest;
        for (size_t i = 0; i < parts.size(); ++i)
            largest[label[parts[i][0]]] = std::max(largest[label[parts[i][0]]], area[i]);
        bool changed = false;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const Index own = label[parts[i][0]];
            if (own != kInvalid ? area[i] >= largest[own] * 0.25 : keepUnlabelled && area[i] >= total * 0.02)
                continue;
            std::map<Index, double> border;
            for (Index g : parts[i])
                mesh.forEachFaceCorner(g, [&](Index c) {
                    Index o = mesh.oppositeFace(mesh.cornerEdge(c), g);
                    if (o != kInvalid && label[o] != own && label[o] != kInvalid)
                        border[label[o]] += mesh.edgeLength(mesh.cornerEdge(c));
                });
            if (border.empty())
                continue;
            const Index to = std::max_element(border.begin(), border.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
            for (Index g : parts[i])
                label[g] = to;
            changed = true;
        }
        if (!changed)
            break;
    }
}
} // namespace

AlignResult alignShapes(const geo::Mesh& moving, const geo::Mesh& fixed, const glm::dmat4& initial, const AlignOptions& opts)
{
    LILA_ZONE();

    AlignResult r;
    r.transform = initial;
    const std::vector<glm::dvec3> mPts = samplePoints(moving, 2000), fPts = samplePoints(fixed, 2000);
    if (mPts.size() < 4 || fPts.size() < 4)
        return r;
    geo::MeshBvh mBvh(moving), fBvh(fixed);
    geo::Bounds b = fBvh.bounds();
    Icp icp{mBvh, fBvh, mPts, fPts, std::max(glm::length(b.max - b.min), 1e-9)};

    auto [placed, placedErr] = icp.run(initial, opts.fit, opts.iterations, 1);
    r.transform = placed;
    r.residual = placedErr;
    if (!opts.search || placedErr < 1e-3)
        return r;

    const Frame fm = principalFrame(mPts), ff = principalFrame(fPts);
    glm::dmat4 best(1.0);
    double bestErr = std::numeric_limits<double>::max();
    const size_t coarse = std::max<size_t>(1, mPts.size() / 300);
    for (const glm::dmat3& P : axisFrames(opts.mirror))
    {
        glm::dvec3 s(1.0);
        for (int i = 0; i < 3; ++i)
        {
            int j = 0;
            for (int k = 0; k < 3; ++k)
                if (P[i][k] != 0.0)
                    j = k;
            s[i] = ff.spread[j] / fm.spread[i];
        }
        if (opts.fit == AlignFit::Similarity)
            s = glm::dvec3(std::cbrt(s.x * s.y * s.z));
        else if (opts.fit == AlignFit::Rigid)
            s = glm::dvec3(1.0);
        glm::dmat3 A = ff.axes * P * glm::dmat3(s.x, 0.0, 0.0, 0.0, s.y, 0.0, 0.0, 0.0, s.z) * glm::transpose(fm.axes);
        auto [T, err] = icp.run(compose(A, ff.mean - A * fm.mean), opts.fit, 6, coarse);
        if (err < bestErr)
        {
            bestErr = err;
            best = T;
        }
    }
    auto [refined, refinedErr] = icp.run(best, opts.fit, opts.iterations, 1);
    // The placement given wins ties: symmetric shapes fit about as well upside down.
    if (refinedErr < placedErr * 0.7)
    {
        r.transform = refined;
        r.residual = refinedErr;
        r.searched = true;
        r.mirrored = glm::determinant(glm::dmat3(refined)) < 0.0;
    }
    return r;
}

GuidedUvResult guidedUvs(const geo::Mesh& source, geo::Mesh& target, const glm::dmat4& toSource, const GuidedUvOptions& opts)
{
    LILA_ZONE();

    GuidedUvResult r;
    auto srcUv = source.cornerAttrs().get<glm::vec2>(opts.sourceSet);
    if (srcUv.empty())
        return r;

    // One lookup per source island, so a target island only ever samples the island it came from.
    ChartOptions islandOpts;
    islandOpts.source = IslandSource::Uvs;
    islandOpts.uvSet = opts.sourceSet;
    const auto islands = islandFaces(source, {}, islandOpts);
    std::vector<Index> islandOf(source.faceCapacity(), kInvalid);
    for (size_t i = 0; i < islands.size(); ++i)
        for (Index f : islands[i])
            islandOf[f] = Index(i);
    const geo::Triangulation all = geo::triangulate(source);
    std::vector<geo::Triangulation> parts(islands.size());
    for (auto& p : parts)
        p.faceFirst.assign(source.faceCapacity(), kInvalid);
    for (Index t = 0; t < all.triangleCount(); ++t)
    {
        Index f = all.faces[t], i = islandOf[f];
        if (i == kInvalid)
            continue;
        geo::Triangulation& p = parts[i];
        if (p.faceFirst[f] == kInvalid)
            p.faceFirst[f] = p.triangleCount();
        p.faces.push_back(f);
        p.corners.insert(p.corners.end(), all.corners.begin() + size_t(t) * 3, all.corners.begin() + size_t(t) * 3 + 3);
    }
    std::vector<geo::MeshBvh> islandBvh(islands.size());
    tbb::parallel_for(size_t(0), islands.size(), [&](size_t i) { islandBvh[i].build(source, std::move(parts[i])); });
    geo::MeshBvh whole;
    whole.build(source, all);

    // Every lookup below runs on the target wrapped onto the source, with the same ids as the target.
    const double outward = glm::determinant(glm::dmat3(toSource)) < 0.0 ? -1.0 : 1.0;
    const geo::Mesh shape = wrapOnto(target, toSource, outward, source, whole, opts.wrapIterations);

    // Label each target face with the source island under it, on the side facing the same way.
    std::vector<Index> label(target.faceCapacity(), kInvalid);
    tbb::parallel_for(Index(0), shape.faceCapacity(), [&](Index f) {
        if (!shape.faceValid(f))
            return;
        // Faces with nothing facing them on the source (a cap where the source is open) stay unlabelled.
        auto hit = closestFacing(whole, source, shape.faceCentroid(f), shape.faceNormal(f) * outward);
        if (hit && hit->facing)
            label[f] = islandOf[whole.triangles().faces[hit->triangle]];
    });
    cleanLabels(target, label, opts.separateUnmatched);

    // Each corner samples its face's island facing its face's way, so thin parts never map onto their far side.
    std::vector<glm::dvec2> cornerUv(target.cornerCapacity(), glm::dvec2(0.0));
    std::vector<uint8_t> cornerHit(target.cornerCapacity(), 0);
    tbb::parallel_for(Index(0), shape.faceCapacity(), [&](Index f) {
        if (!shape.faceValid(f) || label[f] == kInvalid)
            return;
        const geo::MeshBvh& bvh = islandBvh[label[f]];
        const glm::dvec3 n = shape.faceNormal(f) * outward;
        const glm::dvec3 centre = shape.faceCentroid(f);
        shape.forEachFaceCorner(f, [&](Index c) {
            // On a sharp edge the nearest source point can sit on the neighbouring side; step into the face until it doesn't.
            std::optional<SurfaceHit> hit;
            for (double inset : {1e-3, 0.05, 0.15, 0.3})
            {
                hit = closestFacing(bvh, source, glm::mix(shape.position(shape.cornerPoint(c)), centre, inset), n);
                if (!hit || hit->facing)
                    break;
            }
            if (hit)
            {
                cornerUv[c] = glm::dvec2(bvh.interpolate<glm::vec2>(srcUv, hit->triangle, hit->bary));
                cornerHit[c] = 1;
            }
        });
    });
    // Cuts inside a source island (a slit that stops short of splitting it) show as uvs that jump across an edge.
    std::vector<double> islandSize(islands.size(), 0.0);
    for (size_t i = 0; i < islands.size(); ++i)
    {
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (Index f : islands[i])
            source.forEachFaceCorner(f, [&](Index c) {
                lo = glm::min(lo, glm::dvec2(srcUv[c]));
                hi = glm::max(hi, glm::dvec2(srcUv[c]));
            });
        islandSize[i] = glm::length(hi - lo);
    }
    auto jumps = [&](Index e, Index fa, Index fb) {
        if (label[fa] == kInvalid)
            return false;
        const double tol = 0.02 * islandSize[label[fa]];
        for (int end = 0; end < 2; ++end)
        {
            const Index p = target.edgePoint(e, end);
            const Index ca = target.findCorner(fa, p), cb = target.findCorner(fb, p);
            if (ca != kInvalid && cb != kInvalid && cornerHit[ca] && cornerHit[cb] && glm::length(cornerUv[ca] - cornerUv[cb]) > tol)
                return true;
        }
        return false;
    };

    auto seam = target.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (Index e = 0; e < target.edgeCapacity(); ++e)
    {
        if (!target.edgeValid(e))
            continue;
        Index faces[2] = {kInvalid, kInvalid};
        int n = 0;
        target.forEachEdgeCorner(e, [&](Index c) {
            if (n < 2)
                faces[n] = target.cornerFace(c);
            ++n;
        });
        seam[e] = n > 2 || (n == 2 && (label[faces[0]] != label[faces[1]] || jumps(e, faces[0], faces[1])));
    }
    cutToDisks(target);
    seam = target.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (bool s : seam)
        r.seams += s ? 1 : 0;

    ChartOptions chartOpts;
    chartOpts.uvSet = opts.targetSet;
    std::vector<Chart> charts = buildCharts(target, {}, chartOpts);
    std::vector<uint8_t> relaxed(charts.size(), 0), unfolded(charts.size(), 0), repaired(charts.size(), 0), unguided(charts.size(), 0);
    tbb::parallel_for(size_t(0), charts.size(), [&](size_t ci) {
        Chart& ch = charts[ci];
        std::map<Index, int> votes;
        for (Index f : ch.faces)
            if (label[f] != kInvalid)
                ++votes[label[f]];
        if (votes.empty())
        {
            unwrapChart(ch);
            unguided[ci] = 1;
            return;
        }
        std::vector<glm::dvec2> guide(ch.vertexCount(), glm::dvec2(0.0));
        std::vector<int> samples(ch.vertexCount(), 0);
        for (size_t k = 0; k < ch.corners.size(); ++k)
            if (cornerHit[ch.corners[k]])
            {
                guide[ch.cornerVertex[k]] += cornerUv[ch.corners[k]];
                ++samples[ch.cornerVertex[k]];
            }
        for (Index v = 0; v < ch.vertexCount(); ++v)
            if (samples[v])
                guide[v] /= double(samples[v]);
        // Mirrored source islands are solved unmirrored and flipped back after.
        const bool mirrored = signedUvArea(ch, guide) < 0.0;
        if (mirrored)
            for (glm::dvec2& g : guide)
                g.x = -g.x;
        ch.uv.assign(guide.begin(), guide.end());
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        bool solved = foldedTriangles(ch) == 0 && std::abs(ch.areaUv()) > 0.0;
        if (!solved)
        {
            solved = followGuide(ch, guide);
            repaired[ci] = solved;
        }
        if (solved && opts.relax)
        {
            const std::vector<glm::dvec2> kept = ch.uv;
            matchSurfaceArea(ch);
            if (slim(ch, opts.relaxIterations, false) >= 0 && foldedTriangles(ch) == 0)
                relaxed[ci] = 1;
            else
                ch.uv = kept;
        }
        if (!solved)
        {
            UnwrapOptions u;
            u.usePins = false;
            unwrapChart(ch, u);
            unfolded[ci] = 1;
        }
        fitSimilarity2d(ch.uv, guide);
        if (mirrored)
            for (glm::dvec2& p : ch.uv)
                p.x = -p.x;
    });
    placeUnguided(charts, unguided);
    writeCharts(target, charts, opts.targetSet);
    r.islands = charts.size();
    for (size_t i = 0; i < charts.size(); ++i)
    {
        r.relaxed += relaxed[i];
        r.unfolded += unfolded[i];
        r.repaired += repaired[i];
        r.unmatched += unguided[i];
    }
    return r;
}
} // namespace lila::uv
