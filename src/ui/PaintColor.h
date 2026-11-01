#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <functional>

/**
 * A color picked for painting from outside the paint tools (a Reference window's eyedropper). It becomes the sculpt
 * polypaint color, and each paint tool that listens takes it as its primary.
 */
namespace lila::ui::paintColor
{
/** linear: linear RGB. */
LILA_UI_API void pick(const glm::vec3& linear);
/** Returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void(const glm::vec3& linear)> fn);
LILA_UI_API void removeListener(int id);
} // namespace lila::ui::paintColor
