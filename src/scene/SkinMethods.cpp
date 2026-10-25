#include "scene/SkinMethods.h"

#include "core/Profile.h"
#include "deform/Skinning.h"
#include "geo/Mesh.h"
#include "ops/Command.h"

#include <tbb/parallel_for.h>

namespace lila::scene
{
struct SkinMethodCache
{
    DataPtr weights;
    int64_t method = -1;
    size_t points = 0;
    geo::Index edges = 0;
    int iterations = -1;
    double step = -1.0, sigma = -1.0;
    deform::CorBinding cor;
    deform::DdmBinding ddm;
};

namespace skin
{
MeshPtr skinWithMethod(const SkinClusterNode& node, const geo::Mesh& in, const SkinWeights& w, const std::vector<glm::dmat4>& palette, double envelope)
{
    LILA_ZONE();

    const int64_t method = node.as<int64_t>(SkinClusterNode::kSkinningMethod);
    const DataPtr& weightsData = node.as<DataPtr>(SkinClusterNode::kWeights);
    const int iterations = int(std::clamp<int64_t>(node.as<int64_t>(SkinClusterNode::kDdmIterations), 0, 1000));
    const double step = std::clamp(node.as<double>(SkinClusterNode::kDdmSmoothStep), 0.0, 1.0);
    const double sigma = std::max(node.as<double>(SkinClusterNode::kCorSigma), 1e-4);
    const std::span<const glm::dvec3> rest = in.positions();
    const deform::WeightsView view{w.offsets, w.influences, w.weights};

    // Binding is the costly half (centres search, moment smoothing), so it stays until the weights, topology or settings change.
    auto& cache = node.methodCache;
    if (!cache || cache->weights != weightsData || cache->method != method || cache->points != rest.size() || cache->edges != in.numEdges() ||
        (method == SkinClusterNode::kDirectDeltaMush && (cache->iterations != iterations || cache->step != step)) ||
        (method == SkinClusterNode::kCentresOfRotation && cache->sigma != sigma))
    {
        auto c = std::make_shared<SkinMethodCache>();
        c->weights = weightsData;
        c->method = method;
        c->points = rest.size();
        c->edges = in.numEdges();
        c->iterations = iterations;
        c->step = step;
        c->sigma = sigma;
        if (method == SkinClusterNode::kCentresOfRotation)
            c->cor = deform::corBind(rest, deform::trianglePoints(in), view, sigma);
        else
            c->ddm = deform::ddmBind(rest, deform::pointAdjacency(in), view, iterations, step, true);
        cache = std::move(c);
    }

    std::vector<glm::dvec3> moved;
    std::vector<glm::dmat3> rot;
    if (method == SkinClusterNode::kCentresOfRotation)
        deform::corSkin(rest, cache->cor, view, palette, moved, &rot);
    else
        deform::ddmSkin(rest, cache->ddm, view, palette, moved, &rot);

    auto out = std::make_shared<geo::Mesh>(in);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, rest.size(), 512), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t p = r.begin(); p != r.end(); ++p)
        {
            if (!in.pointValid(geo::Index(p)))
                continue;
            out->setPosition(geo::Index(p), glm::mix(rest[p], moved[p], envelope));
            rot[p] = glm::dmat3(1.0) + (rot[p] - glm::dmat3(1.0)) * envelope;
        }
    });
    auto normals = out->cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    if (!normals.empty())
        tbb::parallel_for(tbb::blocked_range<size_t>(0, normals.size(), 1024), [&](const tbb::blocked_range<size_t>& r) {
            for (size_t c = r.begin(); c != r.end(); ++c)
            {
                if (!out->cornerValid(geo::Index(c)))
                    continue;
                geo::Index p = out->cornerPoint(geo::Index(c));
                if (p >= rot.size())
                    continue;
                glm::dvec3 nn = glm::transpose(glm::inverse(rot[p])) * glm::dvec3(normals[c]);
                if (glm::length(nn) > 1e-12)
                    normals[c] = glm::vec3(glm::normalize(nn));
            }
        });
    return out;
}

int64_t methodOf(const std::string& name, const char* cmd)
{
    if (name == "linear")
        return SkinClusterNode::kLinear;
    if (name == "dualQuaternion")
        return SkinClusterNode::kDualQuaternion;
    if (name == "centresOfRotation")
        return SkinClusterNode::kCentresOfRotation;
    if (name == "directDeltaMush")
        return SkinClusterNode::kDirectDeltaMush;
    throw CommandError(std::string(cmd) + ": skinningMethod must be linear, dualQuaternion, centresOfRotation or directDeltaMush");
}
} // namespace skin
} // namespace lila::scene
