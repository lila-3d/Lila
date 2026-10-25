#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QPointF>

#include <optional>
#include <string>

namespace lila::scene::isolation
{
struct Set;
}

namespace lila::ui
{
/** What a viewport hands gizmos, tools, drawers and pickers each frame. Pixels are logical (widget) pixels. */
struct LILA_UI_API ViewInfo
{
    glm::dmat4 view{1.0};
    glm::dmat4 proj{1.0};
    glm::dvec3 eye{0.0};
    double width = 1.0;
    double height = 1.0;
    std::string camera; // view name: persp, top, front, ...
    bool ortho = false;
    glm::dvec2 film{0.0}; // film back half extents: tangents for perspective, cm for orthographic
    /** The view's isolate select set, for drawers to hide what its faces hide; null outside a viewport. */
    const scene::isolation::Set* isolation = nullptr;

    glm::dmat4 viewProj() const { return proj * view; }
    std::optional<QPointF> project(const glm::dvec3& world) const;
    /** World-space ray through a pixel. */
    void ray(QPointF px, glm::dvec3& origin, glm::dvec3& dir) const;
    /** World units per pixel at a point's depth. */
    double unitsPerPixel(const glm::dvec3& at) const;
    glm::dvec3 viewDir(const glm::dvec3& at) const { return glm::normalize(at - eye); }
};
} // namespace lila::ui
