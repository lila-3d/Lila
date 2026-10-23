#pragma once

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

/** Small helpers shared by the rig, skin and constraint command files. */
namespace lila::scene::rigutil
{
inline Graph& graph() { return Graph::current(); }

inline Node& nodeNamed(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

/** Re-throws graph errors as user-facing command errors. */
template <class F> auto guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}

inline glm::dvec3 vec3Of(const json& j)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError("expected [x, y, z], got " + j.dump());
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

inline json toJson3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

inline glm::dmat4 worldOf(const Node& n) { return n.as<glm::dmat4>(Transform::kWorldMatrix); }
inline glm::dvec3 positionOf(const Node& n) { return glm::dvec3(worldOf(n)[3]); }

/** Rotation part with scale and shear removed (Gram-Schmidt, handedness kept). */
inline glm::dmat3 orthonormal(const glm::dmat3& m)
{
    glm::dvec3 x = glm::length(m[0]) > 1e-12 ? glm::normalize(m[0]) : glm::dvec3(1, 0, 0);
    glm::dvec3 y = m[1] - glm::dot(m[1], x) * x;
    y = glm::length(y) > 1e-12 ? glm::normalize(y) : glm::dvec3(0, 1, 0);
    glm::dvec3 z = glm::cross(x, y);
    if (glm::dot(z, m[2]) < 0.0)
        z = -z;
    return {x, y, z};
}

/** Zeroes float noise so channel boxes read 0 instead of 1e-15. */
inline glm::dvec3 clean(glm::dvec3 v)
{
    for (int i = 0; i < 3; ++i)
        if (std::abs(v[i]) < 1e-9)
            v[i] = 0.0;
    return v;
}
} // namespace lila::scene::rigutil
