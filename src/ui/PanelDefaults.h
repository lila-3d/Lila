#pragma once

#include "core/Export.h"

#include <string>

class QAbstractButton;
class QToolButton;
class QWidget;

/**
 * A panel's settings as a set: every field under a root that has a default (setSpinDefault, setComboDefault,
 * setCheckDefault, setToggleDefault) and an objectName, which names it in the prefs. Users can reset them, save the
 * current values as their own defaults, and go back to the built-in ones.
 */
namespace lila::ui::panelDefaults
{
/** A checkable button's default, for reset. */
LILA_UI_API void setToggleDefault(QAbstractButton* button, bool on);

/** Every field back to its default: the user's when saved, else the built-in one. */
LILA_UI_API void reset(QWidget* root);

/** Stores the fields' current values under prefsKey as the user's defaults; middle-click and reset now restore them. */
LILA_UI_API void saveAsDefaults(QWidget* root, const std::string& prefsKey);

/** Forgets the user's defaults and resets every field to the built-in default. */
LILA_UI_API void resetBuiltIn(QWidget* root, const std::string& prefsKey);

/**
 * Makes the stored user defaults the fields' defaults, and applies them to fields still at the built-in default (a
 * value the panel remembered from last time stays). Call once the panel is built.
 */
LILA_UI_API void load(QWidget* root, const std::string& prefsKey);

/** A Reset button for the panel: click resets; its menu saves the current values as defaults or goes back to built-in. */
LILA_UI_API QToolButton* resetButton(QWidget* root, const std::string& prefsKey, QWidget* parent);
} // namespace lila::ui::panelDefaults
