#include "deform/Ssdr.h"

#include "core/Profile.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#pragma warning(disable : 4068 4127 4244 4267 4456 4457 4458 4701 4702)
#endif
#include <DemBones/DemBones.h>
#include <Eigen/Geometry>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "deform/EigenGlm.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace lila::deform
{
namespace
{
using Solver = Dem::DemBones<double, double>;

void setWeights(Solver& s, int bones, const std::vector<uint32_t>& offsets, const std::vector<uint32_t>& influences, const std::vector<float>& weights)
{
    std::vector<Eigen::Triplet<double>> trip;
    for (int i = 0; i < s.nV; ++i)
    {
        double total = 0.0;
        for (uint32_t k = offsets[size_t(i)]; k < offsets[size_t(i) + 1]; ++k)
            if (int(influences[k]) < bones && weights[k] > 0.0f)
                total += weights[k];
        for (uint32_t k = offsets[size_t(i)]; k < offsets[size_t(i) + 1]; ++k)
            if (int(influences[k]) < bones && weights[k] > 0.0f)
                trip.emplace_back(int(influences[k]), i, double(weights[k]) / total);
    }
    s.w.resize(bones, s.nV);
    s.w.setFromTriplets(trip.begin(), trip.end());
}

/** Groups the worst-fitting points into helpers clusters (farthest seeds, then a few Lloyd passes), in rest space. */
std::vector<std::vector<int>> helperClusters(const SsdrProblem& p, const std::vector<double>& error, int helpers)
{
    const int n = int(p.rest.size());
    std::vector<int> order(static_cast<size_t>(n));
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return error[size_t(a)] > error[size_t(b)]; });
    const int count = std::min(n, std::max(helpers * 24, n / 20));
    std::vector<int> cand(order.begin(), order.begin() + count);
    if (cand.empty())
        return {};

    std::vector<glm::dvec3> seeds{p.rest[size_t(cand[0])]};
    std::vector<double> nearest(cand.size(), 1e300);
    while (int(seeds.size()) < helpers)
    {
        size_t best = 0;
        for (size_t c = 0; c < cand.size(); ++c)
        {
            nearest[c] = std::min(nearest[c], glm::length(p.rest[size_t(cand[c])] - seeds.back()));
            if (nearest[c] > nearest[best])
                best = c;
        }
        if (nearest[best] <= 0.0)
            break;
        seeds.push_back(p.rest[size_t(cand[best])]);
    }
    std::vector<std::vector<int>> clusters;
    for (int pass = 0; pass < 6; ++pass)
    {
        clusters.assign(seeds.size(), {});
        for (int v : cand)
        {
            size_t best = 0;
            for (size_t s = 1; s < seeds.size(); ++s)
                if (glm::length(p.rest[size_t(v)] - seeds[s]) < glm::length(p.rest[size_t(v)] - seeds[best]))
                    best = s;
            clusters[best].push_back(v);
        }
        for (size_t s = 0; s < seeds.size(); ++s)
            if (!clusters[s].empty())
            {
                glm::dvec3 c(0.0);
                for (int v : clusters[s])
                    c += p.rest[size_t(v)];
                seeds[s] = c / double(clusters[s].size());
            }
    }
    std::erase_if(clusters, [](const std::vector<int>& c) { return c.size() < 3; });
    return clusters;
}
} // namespace

std::vector<double> skinningError(const SsdrProblem& p, const std::vector<uint32_t>& offsets, const std::vector<uint32_t>& influences,
                                  const std::vector<float>& weights, const std::vector<glm::dmat4>& transforms, int bones)
{
    const size_t n = p.rest.size();
    std::vector<double> err(n, 0.0);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 256), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i != r.end(); ++i)
        {
            double sum = 0.0;
            for (int f = 0; f < p.frames; ++f)
            {
                glm::dmat4 m(0.0);
                double total = 0.0;
                for (uint32_t k = offsets[i]; k < offsets[i + 1]; ++k)
                    if (int(influences[k]) < bones)
                    {
                        m += transforms[size_t(f) * size_t(bones) + influences[k]] * double(weights[k]);
                        total += weights[k];
                    }
                if (total > 0.0)
                    m *= 1.0 / total;
                else
                    m = glm::dmat4(1.0);
                const glm::dvec3 d = glm::dvec3(m * glm::dvec4(p.rest[i], 1.0)) - p.targets[size_t(f) * n + i];
                sum += glm::dot(d, d);
            }
            err[i] = std::sqrt(sum / std::max(1, p.frames));
        }
    });
    return err;
}

SsdrResult ssdrFit(const SsdrProblem& p)
{
    LILA_ZONE();

    const int n = int(p.rest.size());
    if (n == 0 || p.frames < 1 || p.bones < 1)
        throw std::invalid_argument("ssdr: needs points, frames and bones");
    if (p.targets.size() != size_t(p.frames) * size_t(n) || p.transforms.size() != size_t(p.frames) * size_t(p.bones) ||
        p.offsets.size() != size_t(n) + 1)
        throw std::invalid_argument("ssdr: inconsistent sizes");

    Solver s;
    s.nV = n;
    s.nB = p.bones;
    s.nS = 1;
    s.nF = p.frames;
    s.fStart.resize(2);
    s.fStart << 0, p.frames;
    s.subjectID = Eigen::VectorXi::Zero(p.frames);
    s.u.resize(3, n);
    s.v.resize(3 * p.frames, n);
    for (int i = 0; i < n; ++i)
    {
        s.u.col(i) << p.rest[size_t(i)].x, p.rest[size_t(i)].y, p.rest[size_t(i)].z;
        for (int f = 0; f < p.frames; ++f)
        {
            const glm::dvec3& t = p.targets[size_t(f) * size_t(n) + size_t(i)];
            s.v.col(i).segment<3>(f * 3) << t.x, t.y, t.z;
        }
    }
    s.fv = p.polygons;
    s.m.resize(4 * p.frames, 4 * p.bones);
    for (int f = 0; f < p.frames; ++f)
        for (int j = 0; j < p.bones; ++j)
            s.m.block<4, 4>(f * 4, j * 4) = toEigen(p.transforms[size_t(f) * size_t(p.bones) + size_t(j)]);
    s.lockM = Eigen::VectorXi::Ones(p.bones);
    s.lockW = Eigen::VectorXd::Zero(n);
    s.nnz = std::max(1, p.maxInfluences);
    s.weightsSmooth = p.smoothness;
    s.nWeightsIters = std::max(1, p.iterations);
    setWeights(s, p.bones, p.offsets, p.influences, p.weights);
    s.computeWeights();

    auto extract = [&](SsdrResult& r) {
        r.offsets.assign(1, 0);
        r.influences.clear();
        r.weights.clear();
        for (int i = 0; i < n; ++i)
        {
            std::vector<std::pair<double, int>> e;
            for (Eigen::SparseMatrix<double>::InnerIterator it(s.w, i); it; ++it)
                if (it.value() > 1e-4)
                    e.push_back({it.value(), int(it.row())});
            std::sort(e.begin(), e.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
            if (e.empty())
                for (Eigen::SparseMatrix<double>::InnerIterator it(s.w, i); it; ++it)
                    if (it.value() > 0.0 && (e.empty() || it.value() > e[0].first))
                        e.assign(1, {it.value(), int(it.row())});
            if (int(e.size()) > p.maxInfluences)
                e.resize(size_t(p.maxInfluences));
            double total = 0.0;
            for (auto& x : e)
                total += x.first;
            std::sort(e.begin(), e.end(), [](auto& a, auto& b) { return a.second < b.second; });
            for (auto& x : e)
            {
                r.influences.push_back(uint32_t(x.second));
                r.weights.push_back(float(x.first / total));
            }
            r.offsets.push_back(uint32_t(r.influences.size()));
        }
    };

    SsdrResult r;
    extract(r);
    std::vector<glm::dmat4> all = p.transforms;
    int bones = p.bones;

    if (p.helpers > 0)
    {
        std::vector<std::vector<int>> clusters = helperClusters(p, skinningError(p, r.offsets, r.influences, r.weights, all, bones), p.helpers);
        const int h = int(clusters.size());
        if (h > 0)
        {
            const int nb = p.bones + h;
            Eigen::MatrixXd m(4 * p.frames, 4 * nb);
            m.leftCols(4 * p.bones) = s.m;
            for (int c = 0; c < h; ++c)
            {
                Eigen::Matrix3Xd src(3, clusters[size_t(c)].size()), dst(3, clusters[size_t(c)].size());
                glm::dvec3 pivot(0.0);
                for (size_t k = 0; k < clusters[size_t(c)].size(); ++k)
                {
                    src.col(Eigen::Index(k)) = s.u.col(clusters[size_t(c)][k]);
                    pivot += p.rest[size_t(clusters[size_t(c)][k])];
                }
                r.helperPivots.push_back(pivot / double(clusters[size_t(c)].size()));
                for (int f = 0; f < p.frames; ++f)
                {
                    for (size_t k = 0; k < clusters[size_t(c)].size(); ++k)
                        dst.col(Eigen::Index(k)) = s.v.col(clusters[size_t(c)][k]).segment<3>(f * 3);
                    m.block<4, 4>(f * 4, (p.bones + c) * 4) = Eigen::umeyama(src, dst, false);
                }
            }
            // Each clustered point starts half on its helper.
            std::vector<Eigen::Triplet<double>> trip;
            std::vector<int> helperOf(size_t(n), -1);
            for (int c = 0; c < h; ++c)
                for (int v : clusters[size_t(c)])
                    helperOf[size_t(v)] = c;
            for (int i = 0; i < n; ++i)
            {
                const double keep = helperOf[size_t(i)] >= 0 ? 0.5 : 1.0;
                for (Eigen::SparseMatrix<double>::InnerIterator it(s.w, i); it; ++it)
                    trip.emplace_back(int(it.row()), i, it.value() * keep);
                if (helperOf[size_t(i)] >= 0)
                    trip.emplace_back(p.bones + helperOf[size_t(i)], i, 0.5);
            }
            s.nB = nb;
            s.m = m;
            s.lockM = Eigen::VectorXi::Zero(nb);
            s.lockM.head(p.bones).setOnes();
            s.w.resize(nb, n);
            s.w.setFromTriplets(trip.begin(), trip.end());
            s.nIters = std::max(1, p.iterations);
            s.nWeightsIters = 2;
            s.compute();

            extract(r);
            bones = nb;
            all.assign(size_t(p.frames) * size_t(nb), glm::dmat4(1.0));
            r.helperTransforms.resize(size_t(p.frames) * size_t(h));
            for (int f = 0; f < p.frames; ++f)
                for (int j = 0; j < nb; ++j)
                {
                    const glm::dmat4 t = toGlm(s.m.block<4, 4>(f * 4, j * 4));
                    all[size_t(f) * size_t(nb) + size_t(j)] = j < p.bones ? p.transforms[size_t(f) * size_t(p.bones) + size_t(j)] : t;
                    if (j >= p.bones)
                        r.helperTransforms[size_t(f) * size_t(h) + size_t(j - p.bones)] = t;
                }
        }
    }

    r.pointError = skinningError(p, r.offsets, r.influences, r.weights, all, bones);
    double sum = 0.0;
    for (double e : r.pointError)
    {
        sum += e * e;
        r.maxError = std::max(r.maxError, e);
    }
    r.rmse = std::sqrt(sum / double(n));
    return r;
}
} // namespace lila::deform
