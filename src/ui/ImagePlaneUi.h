#pragma once

#include "core/Export.h"

#include <functional>
#include <string>

class QMenu;
class QWidget;

namespace lila::render
{
struct FrameState;
}

namespace lila::ui
{
class ViewportWidget;
struct ViewInfo;
}

namespace lila::ui::imagePlanes
{
/**
 * Image planes in the viewports: draws and picks them, and registers create.imagePlane, viewport.imagePlaneImport and
 * viewport.toggleImagePlanes. activeView is the view the viewport actions act on. Call once.
 */
LILA_UI_API void install(std::function<ViewportWidget*()> activeView);

/** The per-view Image Planes toggle (general pref /viewport/imagePlanes/hidden lists the views that hide them). */
LILA_UI_API bool shownIn(const std::string& view);
LILA_UI_API void setShownIn(const std::string& view, bool on);

/** Picks an image file and creates a plane attached to camera (a view name), or a free plane when camera is empty. */
LILA_UI_API void importImage(QWidget* parent, const std::string& camera);

/** Import Image... plus a Select entry per plane attached to camera, Maya's camera Image Plane menu. */
LILA_UI_API void fillCameraMenu(QMenu* menu, const std::string& camera);

/** Image files dropped onto the view each make a plane attached to its camera. */
LILA_UI_API void acceptDrops(ViewportWidget* view);

/** Adds the image planes view shows, as the viewports draw them, for offscreen captures. */
LILA_UI_API void drawInto(render::FrameState& s, const ViewInfo& view);

/** paths::resolvePath of a stored image path, as UTF-8. */
LILA_UI_API std::string resolve(const std::string& stored);
} // namespace lila::ui::imagePlanes
