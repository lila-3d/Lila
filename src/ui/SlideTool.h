#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * Slide: middle-drag anywhere, or drag the selection, to slide the selected edges along the edges beside them (a face
 * selection slides its border), or in vertex mode each vertex along the edge most along the drag. The surface keeps its
 * shape. Shift slides every point the same distance. The drag previews live and lands as one polySlide call.
 */
LILA_UI_API void registerSlideActions();
} // namespace lila::ui
