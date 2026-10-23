#pragma once

// IK and reverse foot controllers as the viewport shows them: the shape drawn, picked, and right-clicked to lock.

#include "Common.h"

#include <lila/Viewport.h>

#include <optional>
#include <vector>

namespace puppeteer
{
/** The controller's outline in world space, a closed loop: the sole for a reverse foot, a ring around an IK goal. */
std::vector<glm::dvec3> controllerOutline(Graph& g, NodeId chain);

/** The IK chain whose controller outline passes within tolerance pixels of px, or kNoNode. */
NodeId controllerAt(Graph& g, const lila::ui::ViewInfo& view, QPointF px, double tolerance = 6.0);
} // namespace puppeteer
