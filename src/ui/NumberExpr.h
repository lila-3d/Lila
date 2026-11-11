#pragma once

#include "core/Export.h"

#include <QString>

#include <optional>

class QObject;

namespace lila::ui
{
/**
 * A typed number: arithmetic ("2048/4", "sqrt(2)*10", "1.5e3"), or a relative edit of current ("+=5", "-=2", "*=3",
 * "/=4", "*=2+1"). A leading '=' is allowed. nullopt for anything that isn't a finite number.
 */
LILA_UI_API std::optional<double> evalExpression(const QString& text, double current);

/** Lets every spin box in the app take evalExpression text: Enter or leaving the field applies it. Installed once on the application. */
LILA_UI_API void installSpinMath(QObject* parent);

/** A boolean channel typed as on/off, 1/0, true/false or yes/no; nullopt for anything else. */
inline std::optional<bool> parseBool(const QString& text)
{
    QString t = text.trimmed().toLower();
    if (t == "on" || t == "1" || t == "true" || t == "yes")
        return true;
    if (t == "off" || t == "0" || t == "false" || t == "no")
        return false;
    return std::nullopt;
}
} // namespace lila::ui
