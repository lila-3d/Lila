#pragma once

#include <functional>
#include <string>

class QMenu;
class QWidget;

namespace lila::ui
{
class ViewportWidget;
}

namespace lila::ui::imagePlanes
{
/**
 * Image planes in the viewports: draws and picks them, and registers create.imagePlane, viewport.imagePlaneImport and
 * viewport.toggleImagePlanes. activeView is the view the viewport actions act on. Call once.
 */
void install(std::function<ViewportWidget*()> activeView);

/** The per-view Image Planes toggle (general pref /viewport/imagePlanes/hidden lists the views that hide them). */
bool shownIn(const std::string& view);
void setShownIn(const std::string& view, bool on);

/** Picks an image file and creates a plane attached to camera (a view name), or a free plane when camera is empty. */
void importImage(QWidget* parent, const std::string& camera);

/** Import Image... plus a Select entry per plane attached to camera, Maya's camera Image Plane menu. */
void fillCameraMenu(QMenu* menu, const std::string& camera);

/** Image files dropped onto the view each make a plane attached to its camera. */
void acceptDrops(ViewportWidget* view);

/** paths::resolvePath of a stored image path, as UTF-8. */
std::string resolve(const std::string& stored);
} // namespace lila::ui::imagePlanes
