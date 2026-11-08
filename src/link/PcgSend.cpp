#include "link/PcgSend.h"

#include "core/Profile.h"
#include "math/Matrix.h"
#include "math/Transform.h"
#include "math/Unreal.h"

#include <algorithm>
#include <stdexcept>

namespace lila::link
{
namespace
{
json pcgVec(const math::Vector& v)
{
    return json::array({v.x, v.y, v.z});
}

json pcgUnrealPoint(const glm::dvec3& p)
{
    return pcgVec(math::toUnreal(math::Vector(p.x, p.y, p.z)));
}

/** Vector attributes become Unreal-space directions; everything else passes through. */
json pcgUnrealAttributes(const json& attrs)
{
    json out = json::object();
    for (const auto& [k, v] : attrs.items())
        out[k] = v.is_array() && v.size() == 3 && v[0].is_number() ? pcgUnrealPoint(glm::dvec3(v[0].get<double>(), v[1].get<double>(), v[2].get<double>())) : v;
    return out;
}

const char* pcgKindName(PcgSet::Kind k)
{
    switch (k)
    {
    case PcgSet::Kind::Spline:
        return "spline";
    case PcgSet::Kind::Polygon:
        return "polygon";
    default:
        return "points";
    }
}
} // namespace

json pcgDataParams(const std::string& asset, const std::string& label, const std::vector<PcgSet>& sets)
{
    LILA_ZONE();
    json out = {{"asset", asset}, {"label", label}, {"sets", json::array()}};
    for (const PcgSet& s : sets)
    {
        json j = {{"kind", pcgKindName(s.kind)}, {"pin", s.pin}, {"tag", s.tag}, {"attributes", s.attributes}};
        if (s.kind == PcgSet::Kind::Points)
        {
            json pts = json::array();
            for (const PcgPoint& p : s.points)
            {
                const math::Transform t = math::toUnreal(math::Transform(math::Matrix(p.world)));
                const math::Box b = math::toUnreal(math::Box(math::Vector(p.boundsMin.x, p.boundsMin.y, p.boundsMin.z),
                                                             math::Vector(p.boundsMax.x, p.boundsMax.y, p.boundsMax.z)));
                pts.push_back({{"location", pcgVec(t.translation)},
                               {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
                               {"scale", pcgVec(t.scale3d)},
                               {"boundsMin", pcgVec(b.min)},
                               {"boundsMax", pcgVec(b.max)},
                               {"density", p.density},
                               {"attributes", pcgUnrealAttributes(p.attributes)}});
            }
            j["points"] = std::move(pts);
        }
        else
        {
            json path = json::array();
            for (const glm::dvec3& p : s.path)
                path.push_back(pcgUnrealPoint(p));
            j["path"] = std::move(path);
            j["closed"] = s.closed;
        }
        out["sets"].push_back(std::move(j));
    }
    return out;
}

json sendPcgData(RpcClient& client, const json& helloFeatures, const std::string& asset, const std::string& label,
                 const std::vector<PcgSet>& sets)
{
    if (!helloFeatures.is_array() || std::find(helloFeatures.begin(), helloFeatures.end(), json("pcgData")) == helloFeatures.end())
        throw std::runtime_error("enable the LilaLinkPCG plugin in the Unreal project (copy Lila's ue/LilaLinkPCG next to LilaLink); it brings PCG");
    const RpcResult r = client.call("setPcgData", pcgDataParams(asset, label, sets), 60000);
    if (!r.ok)
        throw std::runtime_error("setPcgData: " + r.error);
    return r.result;
}
} // namespace lila::link
