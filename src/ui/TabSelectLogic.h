#pragma once

#include <optional>

/** The Qt-free half of Tab drag selection: which way a stroke goes. */
namespace lila::ui::tabSelect
{
enum class Mode
{
    Select,
    Deselect,
};

/**
 * Decided once at the press and kept until the release. underSelected says whether the component or object under the
 * cursor is selected, nullopt when nothing is there. Ctrl always deselects and Ctrl+Shift always selects, as with clicks;
 * otherwise a stroke starting on something selected deselects and anything else selects.
 */
inline Mode modeAtPress(std::optional<bool> underSelected, bool shift, bool ctrl)
{
    if (ctrl)
        return shift ? Mode::Select : Mode::Deselect;
    return underSelected.value_or(false) ? Mode::Deselect : Mode::Select;
}
} // namespace lila::ui::tabSelect
