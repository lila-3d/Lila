#pragma once

#include "core/Export.h"

namespace lila::ui
{
/**
 * The "createAt.*" actions behind Tab+RMB: a primitive created on the selected components (centred on them, up axis on
 * their average normal), or with nothing selected in component mode or in object mode, carried under the cursor along
 * the surfaces until a click drops it. Call once after the built-in actions are registered.
 */
LILA_UI_API void registerPlacePrimitiveUi();
} // namespace lila::ui
