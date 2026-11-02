#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <functional>

namespace lila::scene::viewState
{
/** The camera of the viewport last clicked or focused, in world space. */
struct ActiveCamera
{
    glm::dmat4 world{1.0};
    /** Full angles in radians after the film fit; 0 when orthographic. */
    double horizontalFov = 0.0;
    double verticalFov = 0.0;
    /** Visible width and height in scene units when orthographic; 0 otherwise. */
    glm::dvec2 orthoSize{0.0};
    bool orthographic = false;

    bool operator==(const ActiveCamera&) const = default;
};

/** False while an export, a send to Unreal or a render evaluates the scene; true otherwise. */
LILA_SCENE_API bool isViewport();
/** True while an export or a send to Unreal evaluates the scene: what leaves Lila. Renders are final but not exports. */
LILA_SCENE_API bool isExport();

/** While alive the scene evaluates for an export or a render, so isViewport() reads false. Nests. Main thread only. */
class LILA_SCENE_API FinalScope
{
public:
    /** exporting false: a render, which shows the scene as the viewport does apart from view-only aids. */
    explicit FinalScope(bool exporting = true);
    ~FinalScope();
    FinalScope(const FinalScope&) = delete;
    FinalScope& operator=(const FinalScope&) = delete;

private:
    bool mExporting = true;
};

LILA_SCENE_API ActiveCamera activeCamera();
/** The active viewport calls this as it draws; a change re-evaluates whatever reads the camera. Main thread only. */
LILA_SCENE_API void setActiveCamera(const ActiveCamera& camera);

/** Called on the main thread whenever isViewport() or the active camera changes. */
LILA_SCENE_API void onChange(std::function<void()> fn);

/** Wraps the export, Unreal send and render commands in a FinalScope. Called once at startup. */
LILA_SCENE_API void installCommandScopes();
} // namespace lila::scene::viewState
