#pragma once

#include "core/Export.h"

#include "cmds/CommandOptions.h"

#include <functional>

class QWidget;

namespace lila::ui
{
/** One command flag's editor: an optional label (null when the field carries its own text) and the field. */
struct FlagFieldWidgets
{
    QWidget* label = nullptr;
    QWidget* field = nullptr;
    /** Shows a value without reporting it as a change. */
    std::function<void(const json&)> set;
};

/**
 * The field for a flag, shared by the adjust-last-op editor and the option box dialogs: check box, spin box with a
 * scrubbable label, combo box, text line or three spin boxes. `changed` gets each edit, already in the flag's type.
 * A middle click resets it to the field's default.
 */
LILA_UI_API FlagFieldWidgets makeFlagField(const cmds::options::Field& f, const json& value, std::function<void(const json&)> changed, QWidget* parent);
} // namespace lila::ui
