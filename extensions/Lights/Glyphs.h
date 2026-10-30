#pragma once

#include <lila/Lights.h>
#include <lila/Viewport.h>

#include <utility>
#include <vector>

namespace lights_ext
{
using Segment = std::pair<glm::dvec3, glm::dvec3>;

/**
 * The wire glyph of a light in world space: its kind's shape, sized by its settings and localScale. detail adds its range
 * (for a selected or hovered light).
 */
std::vector<Segment> glyph(const lila::scene::Node& light, const glm::dmat4& world, bool detail = false);

void draw(lila::ui::drawOverrides::DrawContext& c);
std::optional<double> pick(const lila::ui::drawOverrides::PickContext& c);
} // namespace lights_ext
