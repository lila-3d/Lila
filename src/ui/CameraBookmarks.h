#pragma once

#include "core/Export.h"

#include <functional>

namespace lila::ui
{
class ViewportWidget;
}

/**
 * Camera bookmarks, as Unreal's: slots 1 to 9 per camera, set with Ctrl+Shift+number and jumped to with Alt+Shift+number.
 * A camera node keeps its own in its "bookmarks" attribute; the built-in views (persp, top, front ...) keep theirs in the
 * scene file, shared by every panel showing that view.
 */
namespace lila::ui::cameraBookmarks
{
inline constexpr int kSlots = 9;

/**
 * Registers the cameraBookmark command, "viewport.bookmarkSet<n>", "viewport.bookmarkGo<n>" and "viewport.bookmarkClear"
 * on active(), and the scene section the built-in views' bookmarks save in.
 */
LILA_UI_API void install(std::function<ViewportWidget*()> active);

/** The view install() acts on, or null before it ran. Marking menus list its bookmarks. */
LILA_UI_API ViewportWidget* activeView();
} // namespace lila::ui::cameraBookmarks
