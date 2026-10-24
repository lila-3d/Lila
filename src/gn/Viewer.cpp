#include "gn/Viewer.h"

#include <algorithm>
#include <format>
#include <map>
#include <mutex>

namespace lila::gn
{
namespace
{
struct Store
{
    std::mutex mutex;
    std::map<scene::NodeId, ViewerPtr> data;
};

Store& store()
{
    // Never destroyed: the scene graph's static nodes clear their entries during exit, after this would be gone.
    static Store* s = new Store;
    return *s;
}
} // namespace

void setViewerData(scene::NodeId modifier, ViewerPtr data)
{
    std::lock_guard lock(store().mutex);
    store().data[modifier] = std::move(data);
}

void clearViewerData(scene::NodeId modifier)
{
    std::lock_guard lock(store().mutex);
    store().data.erase(modifier);
}

ViewerPtr viewerData(scene::NodeId modifier)
{
    std::lock_guard lock(store().mutex);
    auto it = store().data.find(modifier);
    return it == store().data.end() ? nullptr : it->second;
}

std::vector<std::pair<scene::NodeId, ViewerPtr>> viewerSnapshots()
{
    std::lock_guard lock(store().mutex);
    return {store().data.begin(), store().data.end()};
}

glm::vec3 viewerColor(const ViewerData& d, const glm::dvec4& v)
{
    switch (d.type)
    {
    case SocketType::Color: return glm::vec3(glm::clamp(glm::dvec3(v), 0.0, 1.0));
    case SocketType::Vector:
    case SocketType::Rotation:
    {
        glm::dvec3 x(v);
        double l = glm::length(x);
        return l > 1e-12 ? glm::vec3(0.5 + 0.5 * x / l) : glm::vec3(0.5f);
    }
    case SocketType::Bool: return v.x != 0.0 ? glm::vec3(1.0f) : glm::vec3(0.02f);
    default:
    {
        double range = d.maxValue - d.minValue;
        float t = float(range > 1e-12 ? std::clamp((v.x - d.minValue) / range, 0.0, 1.0) : 0.5);
        // Blue, cyan, green, yellow, red.
        static const glm::vec3 ramp[5] = {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}};
        float s = t * 4.0f;
        int i = std::min(3, int(s));
        return glm::mix(ramp[i], ramp[i + 1], s - float(i));
    }
    }
}

std::string viewerText(const ViewerData& d, const glm::dvec4& v)
{
    switch (d.type)
    {
    case SocketType::Color: return std::format("({:.3g}, {:.3g}, {:.3g}, {:.3g})", v.x, v.y, v.z, v.w);
    case SocketType::Vector:
    case SocketType::Rotation: return std::format("({:.3g}, {:.3g}, {:.3g})", v.x, v.y, v.z);
    case SocketType::Bool: return v.x != 0.0 ? "true" : "false";
    case SocketType::Int: return std::format("{}", int64_t(v.x));
    default: return std::format("{:.4g}", v.x);
    }
}
} // namespace lila::gn
