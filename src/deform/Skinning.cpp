#include "deform/Skinning.h"

#include "core/Profile.h"

#include <Eigen/Dense>
#include <glm/gtc/quaternion.hpp>
#include <tbb/enumerable_thread_specific.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::deform
{
namespace
{
template <class F> void parallelPoints(size_t n, F&& fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 256), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
            fn(i);
    });
}

using Sparse = std::vector<std::pair<uint32_t, double>>;

/** A point's weights, normalized and sorted by influence id. */
Sparse pointWeights(const WeightsView& w, size_t p)
{
    Sparse out;
    if (p >= w.points())
        return out;
    double total = 0.0;
    for (uint32_t k = w.offsets[p]; k < w.offsets[p + 1]; ++k)
        if (w.weights[k] > 0.0f)
        {
            out.push_back({w.influences[k], double(w.weights[k])});
            total += w.weights[k];
        }
    std::sort(out.begin(), out.end());
    if (total > 0.0)
        for (auto& e : out)
            e.second /= total;
    return out;
}

glm::dmat3 rotationOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
    {
        double len = glm::length(r[i]);
        r[i] = len > 1e-300 ? r[i] / len : glm::dvec3(i == 0, i == 1, i == 2);
    }
    // Gram-Schmidt keeps a sheared matrix from producing a non-unit quaternion.
    r[1] = glm::normalize(r[1] - r[0] * glm::dot(r[0], r[1]));
    r[2] = glm::cross(r[0], r[1]);
    return r;
}

uint64_t pairKey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

/** Le and Hodgins' weight similarity, over the influence pairs both share. */
double similarity(const Sparse& a, const Sparse& b, double sigma2)
{
    // Weights of a's influences in b, aligned by index.
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        auto bi = std::lower_bound(b.begin(), b.end(), std::pair<uint32_t, double>{a[i].first, -1.0});
        if (bi == b.end() || bi->first != a[i].first)
            continue;
        for (size_t j = i + 1; j < a.size(); ++j)
        {
            auto bj = std::lower_bound(b.begin(), b.end(), std::pair<uint32_t, double>{a[j].first, -1.0});
            if (bj == b.end() || bj->first != a[j].first)
                continue;
            const double d = a[i].second * bj->second - a[j].second * bi->second;
            s += a[i].second * a[j].second * bi->second * bj->second * std::exp(-(d * d) / sigma2);
        }
    }
    return s;
}
} // namespace

glm::dmat4 blendMatrix(const WeightsView& w, size_t p, std::span<const glm::dmat4> palette)
{
    glm::dmat4 m(0.0);
    double total = 0.0;
    if (p < w.points())
        for (uint32_t k = w.offsets[p]; k < w.offsets[p + 1]; ++k)
        {
            const uint32_t i = w.influences[k];
            const double wt = w.weights[k];
            if (wt <= 0.0)
                continue;
            m += (i < palette.size() ? palette[i] : glm::dmat4(1.0)) * wt;
            total += wt;
        }
    return total > 0.0 ? m * (1.0 / total) : glm::dmat4(1.0);
}

// ------------------------------------------------------------------------------------------------ centres of rotation

CorBinding corBind(std::span<const glm::dvec3> rest, std::span<const uint32_t> triangles, const WeightsView& w, double sigma)
{
    LILA_ZONE();

    const size_t n = rest.size();
    const size_t nt = triangles.size() / 3;
    std::vector<Sparse> pw(n);
    parallelPoints(n, [&](size_t p) { pw[p] = pointWeights(w, p); });

    std::vector<Sparse> tw(nt);
    std::vector<glm::dvec3> centroid(nt);
    std::vector<double> area(nt);
    parallelPoints(nt, [&](size_t t) {
        const uint32_t* v = &triangles[t * 3];
        if (v[0] >= n || v[1] >= n || v[2] >= n)
            return;
        std::unordered_map<uint32_t, double> acc;
        for (int k = 0; k < 3; ++k)
            for (auto& [i, wt] : pw[v[k]])
                acc[i] += wt / 3.0;
        tw[t].assign(acc.begin(), acc.end());
        std::sort(tw[t].begin(), tw[t].end());
        centroid[t] = (rest[v[0]] + rest[v[1]] + rest[v[2]]) / 3.0;
        area[t] = 0.5 * glm::length(glm::cross(rest[v[1]] - rest[v[0]], rest[v[2]] - rest[v[0]]));
    });

    // Similarity is zero unless both share an influence pair, so triangles are looked up by pair.
    std::unordered_map<uint64_t, std::vector<uint32_t>> byPair;
    for (size_t t = 0; t < nt; ++t)
        for (size_t i = 0; i < tw[t].size(); ++i)
            for (size_t j = i + 1; j < tw[t].size(); ++j)
                byPair[pairKey(tw[t][i].first, tw[t][j].first)].push_back(uint32_t(t));

    CorBinding b;
    b.centers.assign(rest.begin(), rest.end());
    b.valid.assign(n, 0);
    const double sigma2 = std::max(sigma * sigma, 1e-12);
    tbb::enumerable_thread_specific<std::vector<uint32_t>> seenTls([nt] { return std::vector<uint32_t>(nt, UINT32_MAX); });
    parallelPoints(n, [&](size_t p) {
        const Sparse& a = pw[p];
        if (a.size() < 2)
            return;
        std::vector<uint32_t>& seen = seenTls.local();
        glm::dvec3 num(0.0);
        double den = 0.0;
        for (size_t i = 0; i < a.size(); ++i)
            for (size_t j = i + 1; j < a.size(); ++j)
            {
                auto it = byPair.find(pairKey(a[i].first, a[j].first));
                if (it == byPair.end())
                    continue;
                for (uint32_t t : it->second)
                {
                    if (seen[t] == uint32_t(p))
                        continue;
                    seen[t] = uint32_t(p);
                    const double s = similarity(a, tw[t], sigma2) * area[t];
                    num += centroid[t] * s;
                    den += s;
                }
            }
        if (den > 1e-300)
        {
            b.centers[p] = num / den;
            b.valid[p] = 1;
        }
    });
    return b;
}

void corSkin(std::span<const glm::dvec3> rest, const CorBinding& bind, const WeightsView& w, std::span<const glm::dmat4> palette,
             std::vector<glm::dvec3>& out, std::vector<glm::dmat3>* rotations)
{
    LILA_ZONE();

    const size_t n = rest.size();
    std::vector<glm::dquat> quats(palette.size());
    for (size_t i = 0; i < palette.size(); ++i)
        quats[i] = glm::normalize(glm::quat_cast(rotationOf(palette[i])));
    out.resize(n);
    if (rotations)
        rotations->assign(n, glm::dmat3(1.0));
    parallelPoints(n, [&](size_t p) {
        const glm::dmat4 m = blendMatrix(w, p, palette);
        if (p >= bind.valid.size() || !bind.valid[p])
        {
            out[p] = glm::dvec3(m * glm::dvec4(rest[p], 1.0));
            if (rotations)
                (*rotations)[p] = glm::dmat3(m);
            return;
        }
        glm::dquat sum(0, 0, 0, 0), pivot(1, 0, 0, 0);
        bool first = true;
        for (uint32_t k = w.offsets[p]; k < w.offsets[p + 1]; ++k)
        {
            const uint32_t i = w.influences[k];
            const double wt = w.weights[k];
            if (wt <= 0.0)
                continue;
            const glm::dquat q = i < quats.size() ? quats[i] : glm::dquat(1, 0, 0, 0);
            if (first)
                pivot = q;
            first = false;
            sum += q * (glm::dot(pivot, q) < 0.0 ? -wt : wt);
        }
        const double len = glm::length(sum);
        const glm::dmat3 r = len > 1e-12 ? glm::mat3_cast(sum / len) : glm::dmat3(m);
        const glm::dvec3 c = bind.centers[p];
        const glm::dvec3 t = glm::dvec3(m * glm::dvec4(c, 1.0)) - r * c;
        out[p] = r * rest[p] + t;
        if (rotations)
            (*rotations)[p] = r;
    });
}

// ------------------------------------------------------------------------------------------------ direct delta mush

DdmBinding ddmBind(std::span<const glm::dvec3> rest, const Adjacency& adj, const WeightsView& w, int iterations, double step, bool pinBoundary)
{
    LILA_ZONE();

    using Entry = std::pair<uint32_t, std::array<double, 10>>;
    const size_t n = rest.size();
    std::vector<std::vector<Entry>> field(n), next(n);
    parallelPoints(n, [&](size_t p) {
        const glm::dvec3& u = rest[p];
        for (auto& [i, wt] : pointWeights(w, p))
            field[p].push_back({i, {wt * u.x * u.x, wt * u.x * u.y, wt * u.x * u.z, wt * u.y * u.y, wt * u.y * u.z, wt * u.z * u.z, wt * u.x, wt * u.y,
                                    wt * u.z, wt}});
    });

    const size_t na = std::min(n, adj.offsets.size() - 1);
    for (int it = 0; it < iterations; ++it)
    {
        parallelPoints(n, [&](size_t p) {
            if (p >= na || adj.offsets[p] == adj.offsets[p + 1] || (pinBoundary && adj.boundary[p]))
            {
                next[p] = field[p];
                return;
            }
            const double share = step / double(adj.offsets[p + 1] - adj.offsets[p]);
            std::vector<Entry> merged;
            auto add = [&](const std::vector<Entry>& src, double s) {
                std::vector<Entry> out;
                out.reserve(merged.size() + src.size());
                size_t a = 0, b = 0;
                while (a < merged.size() || b < src.size())
                {
                    if (b == src.size() || (a < merged.size() && merged[a].first < src[b].first))
                        out.push_back(merged[a++]);
                    else if (a == merged.size() || src[b].first < merged[a].first)
                    {
                        Entry e = src[b++];
                        for (double& v : e.second)
                            v *= s;
                        out.push_back(e);
                    }
                    else
                    {
                        Entry e = merged[a++];
                        for (int k = 0; k < 10; ++k)
                            e.second[k] += src[b].second[k] * s;
                        ++b;
                        out.push_back(e);
                    }
                }
                merged.swap(out);
            };
            add(field[p], 1.0 - step);
            for (uint32_t k = adj.offsets[p]; k < adj.offsets[p + 1]; ++k)
                add(field[adj.neighbors[k]], share);
            // Influences that only reach a point as a trace are dropped, so the support stops growing without bound.
            std::erase_if(merged, [](const Entry& e) { return e.second[9] < 1e-6; });
            next[p] = std::move(merged);
        });
        field.swap(next);
    }

    DdmBinding b;
    b.offsets.reserve(n + 1);
    for (size_t p = 0; p < n; ++p)
    {
        for (const Entry& e : field[p])
        {
            b.influences.push_back(e.first);
            b.omega.push_back(e.second);
        }
        b.offsets.push_back(uint32_t(b.influences.size()));
    }
    return b;
}

void ddmSkin(std::span<const glm::dvec3> rest, const DdmBinding& bind, const WeightsView& w, std::span<const glm::dmat4> palette,
             std::vector<glm::dvec3>& out, std::vector<glm::dmat3>* rotations)
{
    LILA_ZONE();

    const size_t n = rest.size();
    out.resize(n);
    if (rotations)
        rotations->assign(n, glm::dmat3(1.0));
    parallelPoints(n, [&](size_t p) {
        auto linear = [&] {
            const glm::dmat4 m = blendMatrix(w, p, palette);
            out[p] = glm::dvec3(m * glm::dvec4(rest[p], 1.0));
            if (rotations)
                (*rotations)[p] = glm::dmat3(m);
        };
        if (p + 1 >= bind.offsets.size() || bind.offsets[p] == bind.offsets[p + 1])
            return linear();
        Eigen::Matrix3d q = Eigen::Matrix3d::Zero();
        Eigen::Vector3d qv = Eigen::Vector3d::Zero(), pv = Eigen::Vector3d::Zero();
        double total = 0.0;
        for (uint32_t k = bind.offsets[p]; k < bind.offsets[p + 1]; ++k)
        {
            const std::array<double, 10>& o = bind.omega[k];
            const uint32_t i = bind.influences[k];
            const glm::dmat4 mg = i < palette.size() ? palette[i] : glm::dmat4(1.0);
            Eigen::Matrix3d a;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    a(r, c) = mg[c][r];
            const Eigen::Vector3d t(mg[3][0], mg[3][1], mg[3][2]);
            Eigen::Matrix3d s;
            s << o[0], o[1], o[2], o[1], o[3], o[4], o[2], o[4], o[5];
            const Eigen::Vector3d sv(o[6], o[7], o[8]);
            q += a * s + t * sv.transpose();
            qv += a * sv + t * o[9];
            pv += sv;
            total += o[9];
        }
        if (total <= 1e-12)
            return linear();
        q /= total;
        qv /= total;
        pv /= total;
        const Eigen::Matrix3d cov = q - qv * pv.transpose();
        Eigen::JacobiSVD<Eigen::Matrix3d> svd(cov, Eigen::ComputeFullU | Eigen::ComputeFullV);
        const Eigen::Vector3d sing = svd.singularValues();
        if (!(sing(0) > 1e-20) || sing(1) < sing(0) * 1e-10)
            return linear();
        Eigen::Matrix3d d = Eigen::Matrix3d::Identity();
        d(2, 2) = (svd.matrixU() * svd.matrixV().transpose()).determinant() < 0.0 ? -1.0 : 1.0;
        const Eigen::Matrix3d r = svd.matrixU() * d * svd.matrixV().transpose();
        const Eigen::Vector3d tr = qv - r * pv;
        glm::dmat3 rg;
        for (int row = 0; row < 3; ++row)
            for (int c = 0; c < 3; ++c)
                rg[c][row] = r(row, c);
        out[p] = rg * rest[p] + glm::dvec3(tr.x(), tr.y(), tr.z());
        if (rotations)
            (*rotations)[p] = rg;
    });
}
} // namespace lila::deform
