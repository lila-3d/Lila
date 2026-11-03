#pragma once

#include "core/PrefStore.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QMainWindow>
#include <QString>
#include <QToolButton>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace lila::ui
{
inline QString qs(const std::string& s) { return QString::fromStdString(s); }

/** A neutral grey from the theme's ramp (see themeShade). */
inline QColor gray(int v, int alpha = 255) { return themeShade(v, alpha); }

/** The application's main window, or null before it exists (batch, tests). */
inline QMainWindow* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
            return mw;
    return nullptr;
}

/** The dock with this object name, docked or floating as its own top-level window. */
inline QDockWidget* findDock(const QString& name)
{
    for (QWidget* w : QApplication::topLevelWidgets())
    {
        if (auto* d = qobject_cast<QDockWidget*>(w); d && w->objectName() == name)
            return d;
        if (auto* d = w->findChild<QDockWidget*>(name))
            return d;
    }
    return nullptr;
}

/** An auto-raised button with a 16 px icon, and text beside it when given. */
inline QToolButton* toolButton(QWidget* parent, const QString& iconName, const QString& tip, const QString& text = {})
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setIconSize(QSize(16, 16));
    if (!text.isEmpty())
    {
        b->setText(text);
        b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    return b;
}

/** Combo items as (label, value) pairs; the value is the item data. */
inline void addChoices(QComboBox* box, std::initializer_list<std::pair<const char*, const char*>> items)
{
    for (auto [label, value] : items)
        box->addItem(label, QString(value));
}

/** Selects the item whose value is the string `value`; anything else leaves the box as it is. */
inline void setChoice(QComboBox* box, const nlohmann::json& value)
{
    if (!value.is_string())
        return;
    int i = box->findData(QString::fromStdString(value.get<std::string>()));
    if (i >= 0)
        box->setCurrentIndex(i);
}

inline std::string currentChoice(const QComboBox* box) { return box->currentData().toString().toStdString(); }

/** "boneLength" -> "Bone Length": a space before each capital, first letter raised. */
inline QString niceName(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (std::isupper(static_cast<unsigned char>(c)))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

/** A value from the "general" prefs file. */
template <class T>
T generalPref(std::string_view key, T fallback)
{
    return PrefStore::get().value<T>("general", key, fallback);
}

/** Screen size of the manipulator and tool handles, in pixels. */
inline double handlePixels() { return generalPref("/manipulator/size", 100.0); }

/** sRGB-encoded channel to linear. */
template <std::floating_point T>
T srgbToLinear(T c)
{
    return c <= T(0.04045) ? c / T(12.92) : std::pow((c + T(0.055)) / T(1.055), T(2.4));
}

/** Linear channel to sRGB-encoded, clamped to 0..1 for display. */
template <std::floating_point T>
T linearToSrgb(T c)
{
    c = std::clamp(c, T(0), T(1));
    return c <= T(0.0031308) ? c * T(12.92) : T(1.055) * std::pow(c, T(1) / T(2.4)) - T(0.055);
}
} // namespace lila::ui
