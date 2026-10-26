#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"

#include "render/FrameState.h"
#include "scene/Node.h"
#include "ui/Overlay.h"
#include "ui/ViewInfo.h"

#include <QRectF>

class QMimeData;

#include <functional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Viewport drawers and pickers: content the viewport doesn't draw itself (joints, constraints, measurements, image
 * planes...). Every viewport calls each drawer once a frame and each picker on every object-mode pick, in the order
 * they were added. To draw and pick nodes of one type, a draw override (ui/DrawOverrides.h) is usually simpler.
 */
namespace lila::geo
{
class Mesh;
}

namespace lila::ui::viewportHooks
{
/** Adds scene content to every viewport frame (joints, constraints, curves...). Called on the main thread. */
using Drawer = std::function<void(render::FrameState& s, OverlayPainter& p, const ViewInfo& view)>;

/**
 * Object picking for content the viewport doesn't know: for a click (box empty-ish) return the nearest hit as
 * {name, distance in px}; for a marquee return everything inside with distance 0.
 */
struct PickHit
{
    std::string name;
    double pixels = 0.0;
};
using Picker = std::function<std::vector<PickHit>(QPointF a, QPointF b, bool click, const ViewInfo& view)>;

/** Lila's own, unnamed (ids "lila.drawer.<n>" / "lila.picker.<n>"). */
LILA_UI_API void addDrawer(Drawer fn);
LILA_UI_API void addPicker(Picker fn);

/** Named; adding an id again replaces it in place. owner is an extension id, empty for Lila. */
LILA_UI_API void addDrawer(std::string id, Drawer fn, std::string owner = {});
LILA_UI_API void addPicker(std::string id, Picker fn, std::string owner = {});
/** For extensions: owned by ctx and removed when it unloads. 0, with a log message, when the id is taken. */
LILA_UI_API ext::Context::Handle addDrawer(ext::Context& ctx, std::string id, Drawer fn);
LILA_UI_API ext::Context::Handle addPicker(ext::Context& ctx, std::string id, Picker fn);
LILA_UI_API bool removeDrawer(std::string_view id);
LILA_UI_API bool removePicker(std::string_view id);
/** Everything owner added. Returns the count. */
LILA_UI_API size_t removeOwned(std::string_view owner);

/** In the order added. Copies, so a callback may add or remove hooks while the caller iterates. */
LILA_UI_API std::vector<Drawer> drawers();
LILA_UI_API std::vector<Picker> pickers();
LILA_UI_API std::vector<std::string> drawerIds();
LILA_UI_API std::vector<std::string> pickerIds();

/**
 * Replaces the materials a mesh node draws with (bake previews): fill one material per slot and return true to take the
 * node over; it then draws textured, with tangents, whatever the materials toggle says. Called once a frame per mesh.
 */
using MaterialHook = std::function<bool(scene::NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& materials)>;
/** Adding an id again replaces it. */
LILA_UI_API void setMaterialHook(std::string id, MaterialHook fn);
LILA_UI_API bool removeMaterialHook(std::string_view id);
/** The first hook (by id) that takes the node over, true then. */
LILA_UI_API bool hookedMaterials(scene::NodeId id, const geo::Mesh& mesh, std::vector<render::Material>& materials);

/** View-only hiding (a bake group's highs while its cage is painted): the node is neither drawn nor picked, and the scene is untouched. */
using HideHook = std::function<bool(scene::NodeId id)>;
/** Adding an id again replaces it. */
LILA_UI_API void setHideHook(std::string id, HideHook fn);
LILA_UI_API bool removeHideHook(std::string_view id);
LILA_UI_API bool hiddenInView(scene::NodeId id);

/** What a drag over a viewport is over: the object and, on a mesh, the face under the cursor and where it was hit. */
struct DropTarget
{
    scene::NodeId node = scene::kNoNode;
    uint32_t face = UINT32_MAX;
    /** World point on the surface under the cursor; valid when face is. */
    glm::dvec3 position{0.0};
    /** The view and cursor, so a handler can cast its own ray or place on a ground plane where nothing was hit. */
    ViewInfo view;
    QPointF px;
    /** The drag left the view without dropping: handlers drop any preview they drew. */
    bool left = false;
};
/** Return true to take the drag (commit false: hovering, commit true: dropped). */
using DropHandler = std::function<bool(const QMimeData& mime, const DropTarget& target, bool commit)>;
/** Adding an id again replaces it. */
LILA_UI_API void setDropHandler(std::string id, DropHandler fn);
LILA_UI_API bool removeDropHandler(std::string_view id);
/** The first handler (by id) that takes the drag; true then. */
LILA_UI_API bool handleDrop(const QMimeData& mime, const DropTarget& target, bool commit);
} // namespace lila::ui::viewportHooks
