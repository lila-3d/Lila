#pragma once

#include <glm/glm.hpp>

namespace lila::ui
{
// Overlay colours are sRGB; the *Linear ones feed the 3D streams.

/** A hovered or picked handle, point or edge. */
inline const glm::vec4 kHotColor(1.0f, 1.0f, 1.0f, 1.0f);
/** The dark outline under points and handles, so they read on any background. */
inline const glm::vec4 kHandleRim(0.08f, 0.08f, 0.08f, 1.0f);
/** An idle manipulator handle. */
inline const glm::vec4 kHandleGray(0.70f, 0.70f, 0.70f, 1.0f);

/** Retopology vertices, brush ring and wire, shared by Retopo Draw and the topology tools. */
inline const glm::vec4 kVertexFill(0.15f, 0.15f, 0.15f, 1.0f);
inline const glm::vec4 kVertexRim(0.8f, 0.8f, 0.8f, 1.0f);
inline const glm::vec4 kTopoBrush(0.85f, 0.85f, 0.85f, 0.8f);
inline const glm::vec4 kWireLinear(0.35f, 0.35f, 0.35f, 1.0f);
inline const glm::vec4 kHotLinear(1.0f, 1.0f, 1.0f, 1.0f);
} // namespace lila::ui
