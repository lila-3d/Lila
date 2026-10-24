#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Shape conform actions: Straighten, Circularize, Spherize, Flatten, Conform to Shape and Conform to Object. Conform to
 * Object runs at once when another mesh is selected alongside the edited one; otherwise it waits for a click on the mesh
 * to conform to (Esc cancels).
 */
LILA_UI_API void registerConformActions();
} // namespace lila::ui
