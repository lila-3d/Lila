#pragma once

namespace lila::ext
{
class Context;
}

namespace lights_ext
{
/**
 * Viewport handles on selected lights: range, cone angles, area size and source radius on the glyph, plus a colour
 * swatch (click for the colour wheel) and an intensity readout (drag sideways, a stop per notch) beside each light.
 */
void addHandles(lila::ext::Context& ctx);
} // namespace lights_ext
