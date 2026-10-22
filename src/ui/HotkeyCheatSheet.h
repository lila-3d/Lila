#pragma once

#include <QWidget>

#include <vector>

namespace lila::ui
{
/**
 * Translucent overlay over the main window listing the hotkeys live for the focus widget's context chain, grouped
 * by category. Bound to holding F1; closes on release, a click or another key.
 */
class HotkeyCheatSheet : public QWidget
{
    Q_OBJECT

public:
    static void open();
    static void dismiss();
    static HotkeyCheatSheet* active();

    /** Builds the sheet for focus inside host; exposed for screenshots and tests. */
    HotkeyCheatSheet(QWidget* host, QWidget* focus);

protected:
    void paintEvent(QPaintEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Row
    {
        QString key;
        QString label;
        QString context; // empty for global
    };
    struct Group
    {
        QString title;
        std::vector<Row> rows;
    };

    std::vector<Group> mGroups;
    QString mTitle;
};
} // namespace lila::ui
