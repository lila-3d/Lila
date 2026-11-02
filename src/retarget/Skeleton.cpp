#include "retarget/Skeleton.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace lila::retarget
{
int Skeleton::find(std::string_view name) const
{
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == name)
            return int(i);
    return -1;
}

std::vector<int> Skeleton::children(int joint) const
{
    std::vector<int> out;
    for (size_t i = 0; i < parent.size(); ++i)
        if (parent[i] == joint)
            out.push_back(int(i));
    return out;
}

std::vector<int> Skeleton::path(int start, int end) const
{
    std::vector<int> out;
    if (start < 0 || end < 0)
        return out;
    for (int j = end; j >= 0; j = parent[size_t(j)])
    {
        out.push_back(j);
        if (j == start)
        {
            std::reverse(out.begin(), out.end());
            return out;
        }
    }
    return {};
}

bool Skeleton::isBelow(int joint, int ancestor) const
{
    for (int j = joint; j >= 0; j = parent[size_t(j)])
        if (j == ancestor)
            return true;
    return false;
}

int Skeleton::add(const std::string& name, int parentJoint, const glm::dmat4& preM, const glm::dmat4& afterM)
{
    names.push_back(name);
    parent.push_back(parentJoint);
    pre.push_back(preM);
    after.push_back(afterM);
    parentWorld.push_back(glm::dmat4(1.0));
    return int(names.size()) - 1;
}

Pose identityPose(const Skeleton& s)
{
    return {std::vector<glm::dquat>(s.size(), glm::dquat(1.0, 0.0, 0.0, 0.0)), std::vector<glm::dvec3>(s.size(), glm::dvec3(0.0))};
}

glm::dmat4 localOf(const Skeleton& s, const Pose& p, int j)
{
    const size_t i = size_t(j);
    return glm::translate(glm::dmat4(1.0), p.translate[i]) * s.pre[i] * glm::dmat4(glm::mat3_cast(p.rotation[i])) * s.after[i];
}

std::vector<glm::dmat4> worldOf(const Skeleton& s, const Pose& p)
{
    std::vector<glm::dmat4> w(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        w[i] = parentWorldOf(s, w, int(i)) * localOf(s, p, int(i));
    return w;
}

glm::dmat4 parentWorldOf(const Skeleton& s, const std::vector<glm::dmat4>& world, int j)
{
    const int up = s.parent[size_t(j)];
    return up < 0 ? s.parentWorld[size_t(j)] : world[size_t(up)];
}

glm::dquat rotationOf(const glm::dmat4& m)
{
    glm::dvec3 x(m[0]), y(m[1]);
    x = glm::length(x) > 1e-12 ? glm::normalize(x) : glm::dvec3(1, 0, 0);
    y -= glm::dot(y, x) * x;
    y = glm::length(y) > 1e-12 ? glm::normalize(y) : glm::dvec3(0, 1, 0);
    glm::dvec3 z = glm::cross(x, y);
    // A mirrored frame keeps its handedness out of the rotation: flip x, the axis exporters flip too.
    if (glm::dot(z, glm::dvec3(m[2])) < 0.0)
        x = -x, z = glm::cross(x, y);
    return glm::normalize(glm::quat_cast(glm::dmat3(x, y, z)));
}

glm::dquat localRotationFor(const Skeleton& s, int j, const glm::dmat4& parentWorld, const glm::dquat& want)
{
    const glm::dquat above = rotationOf(parentWorld * s.pre[size_t(j)]);
    const glm::dquat below = rotationOf(s.after[size_t(j)]);
    return glm::normalize(glm::inverse(above) * want * glm::inverse(below));
}

glm::dvec3 translateFor(const glm::dmat4& parentWorld, const glm::dvec3& worldPos) { return glm::dvec3(glm::inverse(parentWorld) * glm::dvec4(worldPos, 1.0)); }

std::string bareName(std::string_view name)
{
    size_t cut = name.find_last_of(":|");
    return std::string(cut == std::string_view::npos ? name : name.substr(cut + 1));
}
} // namespace lila::retarget
