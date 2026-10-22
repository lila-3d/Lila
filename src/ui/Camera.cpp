#include "ui/Camera.h"

#include "scene/Camera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
glm::dvec3 Camera::forward() const
{
    if (free)
        return -rotation[2];
    if (ortho)
        return glm::normalize(orthoForward);
    double y = glm::radians(yaw), p = glm::radians(pitch);
    // Y-up: yaw around Y, starting along -Z. Z-up swaps the vertical axis.
    glm::dvec3 f(-std::sin(y) * std::cos(p), std::sin(p), -std::cos(y) * std::cos(p));
    if (zUp)
        f = {f.x, -f.z, f.y};
    return glm::normalize(f);
}

glm::dvec3 Camera::right() const
{
    return glm::normalize(glm::cross(forward(), viewUp()));
}

glm::dmat4 Camera::view() const
{
    return glm::lookAtRH(eye(), target, viewUp());
}

glm::dvec2 Camera::filmExtents() const
{
    double halfX = ortho ? orthoWidth * 0.5 : (horizontalAperture * 0.5) / focalLength;
    return {halfX, halfX * verticalAperture / std::max(horizontalAperture, 1e-9)};
}

glm::dvec2 Camera::viewExtents(double aspect) const
{
    return scene::fitView(filmExtents(), aspect, filmFit, overscan, gateAspect);
}

std::optional<glm::dvec2> Camera::gateExtents() const
{
    if (!gateAspect)
        return std::nullopt;
    return scene::fitGate(filmExtents(), *gateAspect, filmFit);
}

glm::dmat4 Camera::proj(double aspect) const
{
    double farD = farClip.value_or(ortho ? distance * 2.0 + 1e5 : 0.0);
    return scene::camera::projection(viewExtents(aspect), ortho, nearClip, farD);
}

void Camera::tumble(double dxDeg, double dyDeg)
{
    // Like Maya, orthographic views don't tumble; Alt+LMB pans them instead (handled by the caller).
    if (ortho)
        return;
    if (free)
    {
        // Yaw around world up, pitch around the view's right; the orbit is around the target.
        glm::dmat3 yawRot(glm::rotate(glm::dmat4(1.0), glm::radians(-dxDeg), up()));
        glm::dmat3 pitchRot(glm::rotate(glm::dmat4(1.0), glm::radians(-dyDeg), right()));
        rotation = yawRot * pitchRot * rotation;
        return;
    }
    yaw = std::fmod(yaw - dxDeg, 360.0);
    pitch = std::clamp(pitch - dyDeg, -89.9, 89.9);
}

void Camera::track(double dxFrac, double dyFrac, double aspect)
{
    glm::dvec2 ext = viewExtents(aspect) * (ortho ? 2.0 : 2.0 * distance);
    double width = ext.x, height = ext.y;
    glm::dvec3 camUp = glm::cross(right(), forward());
    target += -right() * (dxFrac * width) + camUp * (dyFrac * height);
}

void Camera::dolly(double factor)
{
    if (ortho)
    {
        orthoWidth = std::clamp(orthoWidth * factor, 0.01, 1e7);
        return;
    }
    distance = std::clamp(distance * factor, 0.01, 1e7);
}

void Camera::frame(const glm::dvec3& center, double radius, double aspect)
{
    glm::dvec2 ext = viewExtents(aspect);
    double tanHalf = std::min(ext.x, ext.y);
    target = center;
    if (ortho)
    {
        if (free)
            distance = std::max(distance, std::max(radius, 0.5) * 2.0);
        // View extents scale linearly with orthoWidth.
        orthoWidth = std::clamp(orthoWidth * std::max(radius, 0.5) * 1.1 / std::max(tanHalf, 1e-12), 0.01, 1e7);
        return;
    }
    distance = std::max(radius, 0.5) / tanHalf * 1.1;
}

void Camera::lookThrough(const scene::Node& cam, const glm::dmat4& world)
{
    using scene::Camera;
    glm::dmat4 view = scene::camera::viewMatrix(world);
    glm::dmat3 r = glm::transpose(glm::dmat3(view));
    free = true;
    rotation = r;
    ortho = cam.as<bool>(Camera::kOrthographic);
    target = glm::dvec3(world[3]) - r[2] * distance;
    focalLength = std::max(cam.as<double>(Camera::kFocalLength), 1e-6);
    horizontalAperture = std::max(cam.as<double>(Camera::kHorizontalFilmAperture), 1e-6) * 25.4;
    verticalAperture = std::max(cam.as<double>(Camera::kVerticalFilmAperture), 1e-6) * 25.4;
    nearClip = std::max(cam.as<double>(Camera::kNearClipPlane), 1e-6);
    farClip = cam.as<double>(Camera::kFarClipPlane);
    orthoWidth = std::max(cam.as<double>(Camera::kOrthographicWidth), 1e-3);
    filmFit = scene::camera::filmFit(cam);
}

void Camera::leaveLookThrough()
{
    const Camera lens;
    free = false;
    focalLength = lens.focalLength;
    horizontalAperture = lens.horizontalAperture;
    verticalAperture = lens.verticalAperture;
    nearClip = lens.nearClip;
    farClip.reset();
}

void Camera::setView(const std::string& name)
{
    if (name == "persp")
    {
        ortho = false;
        return;
    }
    glm::dvec3 u = up();
    // Lila's front looks down -Z at +Z-facing faces (Y-up); with Z-up, front looks along +Y.
    glm::dvec3 frontDir = zUp ? glm::dvec3(0, 1, 0) : glm::dvec3(0, 0, -1);
    glm::dvec3 rightDir = glm::cross(frontDir, u);
    ortho = true;
    if (name == "top" || name == "bottom")
    {
        orthoForward = name == "top" ? -u : u;
        orthoUp = name == "top" ? frontDir : -frontDir;
    }
    else
    {
        orthoForward = name == "front" ? frontDir : name == "back" ? -frontDir : name == "left" ? rightDir : -rightDir;
        orthoUp = u;
    }
    distance = 10000.0;
}
} // namespace lila::ui
