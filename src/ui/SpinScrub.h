#pragma once

#include "core/Export.h"

#include <QRect>
#include <QString>

#include <functional>

class QAbstractSpinBox;
class QCheckBox;
class QComboBox;
class QObject;
class QWidget;

namespace lila::ui
{
/**
 * Makes every spin box in the app a virtual slider: middle-drag left/right steps its value by its single step every few
 * pixels (Shift x10, Ctrl x0.1 on decimal fields). A middle click (no drag) restores the box's kDefaultProperty, when it
 * has one. Installed once on the application.
 */
LILA_UI_API void installSpinScrub(QObject* parent);

/** Dynamic property holding a spin box's default value (a double), restored by a middle click on the box. */
inline constexpr const char* kDefaultProperty = "lilaDefault";

/** Gives a spin box a default to restore on middle click (on it, or on its buddy label); its tooltip then says so. */
LILA_UI_API void setSpinDefault(QAbstractSpinBox* box, double value);

/** The same for a check box, or a combo box (by item index). The form label naming the field as buddy resets it too. */
LILA_UI_API void setCheckDefault(QCheckBox* box, bool on);
LILA_UI_API void setComboDefault(QComboBox* box, int index);

/** "Middle-click to reset to default (X)", the line added to the tooltip of every field that resets. */
LILA_UI_API QString resetHint(const QString& defaultText);

/** Briefly lights up a field (or a rect of it, in its own coordinates) to acknowledge a reset. */
LILA_UI_API void flashReset(QWidget* w, QRect rect = {});

/**
 * Makes a middle click (not a drag) on target call reset and flash `flash` (target when null). Its tooltip gains
 * resetHint(defaultText). Middle drags and every other event pass through untouched.
 */
LILA_UI_API void onMiddleClickReset(QWidget* target, std::function<void()> reset, const QString& defaultText, QWidget* flash = nullptr);
} // namespace lila::ui
