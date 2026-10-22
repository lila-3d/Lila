#pragma once

#include <QString>

#include <optional>

namespace lila::ui
{
/** Maya-style relative edits: "+=5", "-=2", "*=2", "/=4"; otherwise a plain value. */
inline std::optional<double> evalExpression(const QString& text, double current)
{
    QString t = text.trimmed();
    bool ok = false;
    if (t.size() > 2 && t[1] == '=' && QString("+-*/").contains(t[0]))
    {
        double v = t.mid(2).trimmed().toDouble(&ok);
        if (!ok)
            return std::nullopt;
        switch (t[0].toLatin1())
        {
        case '+': return current + v;
        case '-': return current - v;
        case '*': return current * v;
        case '/': return v != 0.0 ? std::optional(current / v) : std::nullopt;
        }
    }
    if (t.startsWith('='))
        t = t.mid(1);
    double v = t.toDouble(&ok);
    return ok ? std::optional(v) : std::nullopt;
}

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
