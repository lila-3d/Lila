#pragma once

#include <QDialog>

namespace lila::ui
{
/**
 * Settings window. Every control writes its pref the moment it changes (prefs are live), so there's no OK/Apply:
 * close it when done. Anything not shown here is still editable in the JSON files.
 */
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    static void open(QWidget* parent);
};
} // namespace lila::ui
