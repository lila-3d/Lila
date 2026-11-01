#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <QPoint>
#include <QString>

#include <functional>

/**
 * A small colour wheel that pops up at a point (a swatch clicked in the viewport): hue round the wheel, saturation out
 * from its middle, brightness on the bar beside it, and optionally a colour temperature. Edits report live; a click
 * outside keeps them, Esc or the old swatch puts the colour back.
 */
namespace lila::ui::colorPopup
{
struct Value
{
    glm::dvec3 color{1.0}; // linear
    bool useTemperature = false;
    double temperature = 6500.0; // kelvin

    bool operator==(const Value&) const = default;
};

struct Options
{
    Value value;
    /** Shows the temperature row (a light's kelvin). */
    bool temperature = false;
    QString title;
    /** Every edit, live. */
    std::function<void(const Value&)> changed;
    /** Once, when it closes: accepted is false for Esc, and value is then the one it opened with. */
    std::function<void(const Value& value, bool accepted)> closed;
};

/** Opens the popup with its top left near globalPos, kept on screen. Closes any other open one first (accepting it). */
LILA_UI_API void show(QPoint globalPos, Options options);
} // namespace lila::ui::colorPopup
