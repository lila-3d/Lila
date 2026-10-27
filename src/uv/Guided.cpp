#include "uv/Guided.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
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

/** Least squares T with T * moving ~ fixed over the pairs. */
glm::dmat4 fitPairs(const std::vector<Pair>& pairs, AlignFit fit)
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
        // Affine, then the rotation and symmetric stretch of its polar decomposition, so it never shears or mirrors.
        A = cross * self.inverse();
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(A, Eigen::ComputeFullU | Eigen::ComputeFullV);
        Eigen::Matrix3d U = svd.matrixU(), V = svd.matrixV();
        Eigen::Vector3d s = svd.singularValues();
        if ((U * V.transpose()).determinant() < 0.0)
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
        if ((svd.matrixU() * svd.matrixV().transpose()).determinant() < 0.0)
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
        for (int it = 0; it < iterations && p.size() >= 4; ++it)
        {
            glm::dmat4 next = fitPairs(p, fit);
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

/** The 24 rotations that permute and negate the axes. */
std::vector<glm::dmat3> axisRotations()
{
    std::vector<glm::dmat3> out;
    const std::array<std::array<int, 3>, 6> perms = {{{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}};
    for (const auto& p : perms)
        for (int signs = 0; signs < 8; ++signs)
        {
            glm::dmat3 m(0.0);
            for (int i = 0; i < 3; ++i)
                m[i][p[i]] = (signs >> i) & 1 ? -1.0 : 1.0;
            if (glm::determinant(m) > 0.0)
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

/** Each face's label replaced by the most common among itself and its edge neighbours; ties keep its own. */
void smoothLabels(const geo::Mesh& mesh, std::vector<Index>& label, int passes)
{
    for (int pass = 0; pass < passes; ++pass)
    {
        std::vector<Index> next = label;
        tbb::parallel_for(Index(0), mesh.faceCapacity(), [&](Index f) {
            if (!mesh.faceValid(f))
                return;
            std::map<Index, int> votes;
            if (label[f] != kInvalid)
                votes[label[f]] += 1;
            mesh.forEachFaceCorner(f, [&](Index c) {
                Index o = mesh.oppositeFace(mesh.cornerEdge(c), f);
                if (o != kInvalid && label[o] != kInvalid)
                    votes[label[o]] += 1;
            });
            int best = label[f] != kInvalid ? votes[label[f]] : 0;
            for (const auto& [l, v] : votes)
                if (v > best)
                {
                    best = v;
                    next[f] = l;
                }
        });
        label.swap(next);
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
    for (const glm::dmat3& P : axisRotations())
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

    // Label each target face with the source island under it, on the side facing the same way.
    const glm::dmat3 normalToSource = glm::transpose(glm::inverse(glm::dmat3(toSource)));
    std::vector<Index> label(target.faceCapacity(), kInvalid);
    tbb::parallel_for(Index(0), target.faceCapacity(), [&](Index f) {
        if (!target.faceValid(f))
            return;
        const glm::dvec3 p(toSource * glm::dvec4(target.faceCentroid(f), 1.0));
        const glm::dvec3 n = glm::normalize(normalToSource * target.faceNormal(f));
        geo::BvhClosest c;
        if (!whole.closestPoint(p, c))
            return;
        Index sf = c.face;
        if (glm::dot(source.faceNormal(sf), n) < 0.0)
        {
            double bestT = std::numeric_limits<double>::max();
            for (double dir : {1.0, -1.0})
            {
                geo::BvhHit h;
                if (whole.raycast(p, n * dir, h) && h.t < bestT && glm::dot(source.faceNormal(h.face), n) > 0.0)
                {
                    bestT = h.t;
                    sf = h.face;
                }
            }
        }
        label[f] = islandOf[sf];
    });
    smoothLabels(target, label, std::max(opts.smoothPasses, 1));

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
        seam[e] = n > 2 || (n == 2 && label[faces[0]] != label[faces[1]]);
    }
    cutToDisks(target);
    seam = target.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    for (bool s : seam)
        r.seams += s ? 1 : 0;

    ChartOptions chartOpts;
    chartOpts.uvSet = opts.targetSet;
    std::vector<Chart> charts = buildCharts(target, {}, chartOpts);
    std::vector<uint8_t> relaxed(charts.size(), 0), unfolded(charts.size(), 0);
    tbb::parallel_for(size_t(0), charts.size(), [&](size_t ci) {
        Chart& ch = charts[ci];
        std::map<Index, int> votes;
        for (Index f : ch.faces)
            if (label[f] != kInvalid)
                ++votes[label[f]];
        if (votes.empty())
        {
            unwrapChart(ch);
            unfolded[ci] = 1;
            return;
        }
        Index island = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        std::vector<glm::dvec2> guide(ch.vertexCount());
        for (Index v = 0; v < ch.vertexCount(); ++v)
        {
            geo::BvhClosest c;
            if (islandBvh[island].closestPoint(glm::dvec3(toSource * glm::dvec4(ch.pos[v], 1.0)), c))
                guide[v] = glm::dvec2(islandBvh[island].interpolate<glm::vec2>(srcUv, c.triangle, c.bary));
        }
        // Mirrored source islands are solved unmirrored and flipped back after.
        const bool mirrored = signedUvArea(ch, guide) < 0.0;
        if (mirrored)
            for (glm::dvec2& g : guide)
                g.x = -g.x;
        ch.uv.assign(guide.begin(), guide.end());
        std::fill(ch.pinned.begin(), ch.pinned.end(), uint8_t(0));
        bool solved = false;
        if (opts.relax && foldedTriangles(ch) == 0 && std::abs(ch.areaUv()) > 0.0)
        {
            matchSurfaceArea(ch);
            solved = slim(ch, opts.relaxIterations, false) >= 0 && foldedTriangles(ch) == 0;
            relaxed[ci] = solved;
        }
        else if (!opts.relax && foldedTriangles(ch) == 0)
            solved = true;
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
    writeCharts(target, charts, opts.targetSet);
    r.islands = charts.size();
    for (size_t i = 0; i < charts.size(); ++i)
    {
        r.relaxed += relaxed[i];
        r.unfolded += unfolded[i];
    }
    return r;
}
} // namespace lila::uv
