#include "sculpt/SubdLevels.h"

#include "core/Profile.h"
#include "geo/Symmetry.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>

namespace lila::sculpt
{
namespace
{
using geo::kInvalid;
using geo::PointStencil;

constexpr Index kGrain = 4096;

template <class T> std::vector<T> applyStencil(const PointStencil& s, std::span<const T> in)
{
    std::vector<T> out(s.rows());
    tbb::parallel_for(tbb::blocked_range<Index>(0, s.rows(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index i = r.begin(); i < r.end(); ++i)
        {
            T sum(0);
            for (Index k = s.offsets[i]; k < s.offsets[i + 1]; ++k)
                sum += in[s.cols[k]] * T(s.weights[k]);
            out[i] = sum;
        }
    });
    return out;
}

PointStencil transpose(const PointStencil& s)
{
    PointStencil t;
    t.inputs = s.rows();
    t.offsets.assign(size_t(s.inputs) + 1, 0);
    for (Index c : s.cols)
        ++t.offsets[c + 1];
    for (size_t i = 1; i < t.offsets.size(); ++i)
        t.offsets[i] += t.offsets[i - 1];
    t.cols.resize(s.cols.size());
    t.weights.resize(s.weights.size());
    std::vector<Index> fill(t.offsets.begin(), t.offsets.end() - 1);
    for (Index r = 0; r < s.rows(); ++r)
        for (Index k = s.offsets[r]; k < s.offsets[r + 1]; ++k)
        {
            const Index at = fill[s.cols[k]]++;
            t.cols[at] = r;
            t.weights[at] = s.weights[k];
        }
    return t;
}

std::shared_ptr<const SubdLevel> levelAbove(const geo::Mesh& below, const geo::SubdivideOptions& opts, PointStencil& stencil)
{
    LILA_ZONE();

    geo::SubdivideOptions once = opts;
    once.levels = 1;
    auto lvl = std::make_shared<SubdLevel>();
    stencil = std::move(geo::subdivideStencils(below, once).front());
    geo::SubdivideMap map;
    lvl->mesh = geo::subdivide(below, once, &map);
    geo::clearSymmetryKeys(lvl->mesh, 0);
    lvl->parentPoint = std::move(map.points);
    lvl->down = transpose(stencil);
    const geo::Mesh& m = lvl->mesh;
    lvl->framePeer.assign(m.pointCapacity(), kInvalid);
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c) && lvl->framePeer[m.cornerPoint(c)] == kInvalid)
            lvl->framePeer[m.cornerPoint(c)] = m.cornerNextPoint(c);
    return lvl;
}

struct Frame
{
    glm::dvec3 t, b, n;
};

/** Each point's detail frame on level `level` at positions: the surface normal and a tangent toward its frame peer. */
std::vector<Frame> frames(const SubdLevel& lvl, std::span<const glm::dvec3> pos)
{
    LILA_ZONE();

    const geo::Mesh& m = lvl.mesh;
    std::vector<glm::dvec3> faceN(m.faceCapacity(), glm::dvec3(0.0));
    tbb::parallel_for(tbb::blocked_range<Index>(0, m.faceCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            glm::dvec3 n(0.0);
            const Index first = m.faceCorner(f);
            Index c = first;
            do
            {
                const glm::dvec3& a = pos[m.cornerPoint(c)];
                const glm::dvec3& b = pos[m.cornerNextPoint(c)];
                n += glm::cross(a, b);
                c = m.cornerNext(c);
            } while (c != first);
            faceN[f] = n;
        }
    });
    std::vector<Frame> out(m.pointCapacity());
    tbb::parallel_for(tbb::blocked_range<Index>(0, m.pointCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            glm::dvec3 n(0.0);
            m.forEachPointCorner(p, [&](Index c) { n += faceN[m.cornerFace(c)]; });
            n = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0.0, 1.0, 0.0);
            glm::dvec3 t = lvl.framePeer[p] != kInvalid ? pos[lvl.framePeer[p]] - pos[p] : glm::dvec3(0.0);
            t -= n * glm::dot(t, n);
            if (glm::length(t) <= 1e-300)
            {
                t = std::abs(n.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
                t -= n * glm::dot(t, n);
            }
            t = glm::normalize(t);
            out[p] = {t, glm::cross(n, t), n};
        }
    });
    return out;
}

/** min |S x - y|^2 + lambda |x - x0|^2 by Jacobi-preconditioned conjugate gradients on the normal equations. */
std::vector<glm::dvec3> leastSquaresDown(const PointStencil& S, const PointStencil& ST, std::span<const glm::dvec3> y, std::vector<glm::dvec3> x)
{
    LILA_ZONE();

    const Index n = ST.rows();
    std::vector<double> diag(n, 0.0);
    double meanDiag = 0.0;
    for (Index j = 0; j < n; ++j)
    {
        for (Index k = ST.offsets[j]; k < ST.offsets[j + 1]; ++k)
            diag[j] += ST.weights[k] * ST.weights[k];
        meanDiag += diag[j];
    }
    meanDiag = n ? meanDiag / double(n) : 1.0;
    const double lambda = 1e-6 * std::max(meanDiag, 1e-12);
    const std::vector<glm::dvec3> x0 = x;

    auto normalOp = [&](std::span<const glm::dvec3> v) {
        std::vector<glm::dvec3> sv = applyStencil<glm::dvec3>(S, v);
        std::vector<glm::dvec3> out = applyStencil<glm::dvec3>(ST, sv);
        tbb::parallel_for(Index(0), n, [&](Index j) { out[j] += lambda * v[j]; });
        return out;
    };
    auto dot3 = [&](const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b) {
        return tbb::parallel_reduce(
            tbb::blocked_range<Index>(0, n, kGrain), glm::dvec3(0.0),
            [&](const tbb::blocked_range<Index>& r, glm::dvec3 acc) {
                for (Index j = r.begin(); j < r.end(); ++j)
                    acc += a[j] * b[j];
                return acc;
            },
            [](glm::dvec3 a, glm::dvec3 b) { return a + b; });
    };

    std::vector<glm::dvec3> rhs = applyStencil<glm::dvec3>(ST, y);
    for (Index j = 0; j < n; ++j)
        rhs[j] += lambda * x0[j];
    std::vector<glm::dvec3> r = normalOp(x);
    for (Index j = 0; j < n; ++j)
        r[j] = rhs[j] - r[j];
    std::vector<glm::dvec3> z(n), p(n);
    for (Index j = 0; j < n; ++j)
        z[j] = r[j] / (diag[j] + lambda);
    p = z;
    glm::dvec3 rz = dot3(r, z);
    const glm::dvec3 rhsNorm = dot3(rhs, rhs);
    const double tol2 = 1e-22;
    for (int it = 0; it < 400; ++it)
    {
        const glm::dvec3 rr = dot3(r, r);
        if (rr.x <= tol2 * rhsNorm.x && rr.y <= tol2 * rhsNorm.y && rr.z <= tol2 * rhsNorm.z)
            break;
        std::vector<glm::dvec3> ap = normalOp(p);
        const glm::dvec3 pap = dot3(p, ap);
        glm::dvec3 alpha(0.0);
        for (int c = 0; c < 3; ++c)
            alpha[c] = pap[c] > 0.0 ? rz[c] / pap[c] : 0.0;
        tbb::parallel_for(Index(0), n, [&](Index j) {
            x[j] += alpha * p[j];
            r[j] -= alpha * ap[j];
            z[j] = r[j] / (diag[j] + lambda);
        });
        const glm::dvec3 rzNext = dot3(r, z);
        glm::dvec3 beta(0.0);
        for (int c = 0; c < 3; ++c)
            beta[c] = rz[c] > 0.0 ? rzNext[c] / rz[c] : 0.0;
        rz = rzNext;
        tbb::parallel_for(Index(0), n, [&](Index j) { p[j] = z[j] + beta * p[j]; });
    }
    return x;
}

template <class T> std::vector<T> layerUp(const PointStencil& s, const std::vector<T>& below, const std::vector<T>* diff, T fallback, Index belowCount)
{
    if (below.empty() && (!diff || diff->empty()))
        return {};
    std::vector<T> in = below.empty() ? std::vector<T>(belowCount, fallback) : below;
    std::vector<T> out = applyStencil<T>(s, in);
    if (diff && !diff->empty())
        for (size_t i = 0; i < out.size(); ++i)
            out[i] += (*diff)[i];
    for (T& v : out)
        v = glm::clamp(v, T(0.0f), T(1.0f));
    return out;
}

template <class T> void layerDown(const PointStencil& s, const SubdLevel& lvl, std::vector<T>& layer, std::vector<T>& diff)
{
    if (layer.empty())
        return;
    std::vector<T> coarse(lvl.parentPoint.size(), T(0.0f));
    for (size_t j = 0; j < coarse.size(); ++j)
        if (lvl.parentPoint[j] != kInvalid)
            coarse[j] = layer[lvl.parentPoint[j]];
    std::vector<T> back = applyStencil<T>(s, coarse);
    diff.resize(layer.size());
    bool any = false;
    for (size_t i = 0; i < layer.size(); ++i)
    {
        diff[i] = layer[i] - back[i];
        any = any || diff[i] != T(0.0f);
    }
    if (!any)
        diff.clear();
    layer = std::move(coarse);
}
} // namespace

std::shared_ptr<const SubdStack> buildSubdStack(const geo::Mesh& cage, const geo::SubdivideOptions& opts)
{
    LILA_ZONE();

    auto s = std::make_shared<SubdStack>();
    s->opts = opts;
    auto base = std::make_shared<SubdLevel>();
    base->mesh = cage;
    s->levels.push_back(base);
    for (int l = 0; l < opts.levels; ++l)
        s->levels.push_back(levelAbove(s->levels.back()->mesh, opts, s->stencils.emplace_back()));
    return s;
}

std::shared_ptr<const SubdStack> addSubdLevel(const SubdStack& s)
{
    auto out = std::make_shared<SubdStack>(s);
    out->levels.push_back(levelAbove(s.levels.back()->mesh, s.opts, out->stencils.emplace_back()));
    out->opts.levels = out->top();
    return out;
}

std::shared_ptr<const SubdStack> truncateSubdStack(const SubdStack& s, int top)
{
    auto out = std::make_shared<SubdStack>();
    out->opts = s.opts;
    out->opts.levels = top;
    out->levels.assign(s.levels.begin(), s.levels.begin() + top + 1);
    out->stencils.assign(s.stencils.begin(), s.stencils.begin() + top);
    return out;
}

void stepUp(const SubdStack& s, int to, const DetailPtr& detail, std::vector<glm::dvec3>& positions, LevelLayers& layers)
{
    LILA_ZONE();

    const PointStencil& S = s.stencils[size_t(to - 1)];
    const SubdLevel& lvl = *s.levels[size_t(to)];
    const Index below = Index(positions.size());
    std::vector<glm::dvec3> q = S.apply(positions);
    if (detail && !detail->offset.empty())
    {
        const std::vector<Frame> f = frames(lvl, q);
        tbb::parallel_for(Index(0), Index(q.size()), [&](Index i) {
            const glm::dvec3& o = detail->offset[i];
            q[i] += f[i].t * o.x + f[i].b * o.y + f[i].n * o.z;
        });
    }
    positions = std::move(q);
    const LevelLayers* d = detail ? &detail->layers : nullptr;
    layers.color = layerUp<glm::vec4>(S, layers.color, d ? &d->color : nullptr, glm::vec4(1.0f), below);
    layers.mask = layerUp<float>(S, layers.mask, d ? &d->mask : nullptr, 0.0f, below);
    layers.freeze = layerUp<float>(S, layers.freeze, d ? &d->freeze : nullptr, 0.0f, below);
}

DetailPtr stepDown(const SubdStack& s, int from, std::vector<glm::dvec3>& positions, LevelLayers& layers)
{
    LILA_ZONE();

    const PointStencil& S = s.stencils[size_t(from - 1)];
    const SubdLevel& lvl = *s.levels[size_t(from)];
    std::vector<glm::dvec3> start(lvl.parentPoint.size(), glm::dvec3(0.0));
    for (size_t j = 0; j < start.size(); ++j)
        if (lvl.parentPoint[j] != kInvalid)
            start[j] = positions[lvl.parentPoint[j]];
    std::vector<glm::dvec3> coarse = leastSquaresDown(S, lvl.down, positions, std::move(start));
    const std::vector<glm::dvec3> q = S.apply(coarse);
    const std::vector<Frame> f = frames(lvl, q);

    auto d = std::make_shared<LevelDetail>();
    d->offset.resize(positions.size());
    tbb::parallel_for(Index(0), Index(positions.size()), [&](Index i) {
        const glm::dvec3 v = positions[i] - q[i];
        d->offset[i] = {glm::dot(v, f[i].t), glm::dot(v, f[i].b), glm::dot(v, f[i].n)};
    });
    positions = std::move(coarse);
    layerDown<glm::vec4>(S, lvl, layers.color, d->layers.color);
    layerDown<float>(S, lvl, layers.mask, d->layers.mask);
    layerDown<float>(S, lvl, layers.freeze, d->layers.freeze);
    return d;
}
} // namespace lila::sculpt
