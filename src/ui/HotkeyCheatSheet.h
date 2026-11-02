#pragma once

#include "core/Export.h"

#include <QElapsedTimer>
#include <QPointer>
#include <QWidget>

#include <vector>

class QLineEdit;

namespace lila::ui
{
/**
 * Translucent overlay over the main window listing the hotkeys live for the focus widget's context chain, grouped
 * by category. Bound to F1: held, it shows until the release; tapped, it stays open with its filter focused until F1,
 * Esc or a click outside it.
 */
class LILA_UI_API HotkeyCheatSheet : public QWidget
{
    Q_OBJECT

public:
    /** Opens the sheet, or closes one left open by a tap. */
    static void open();
    static void dismiss();
    /** The key's release: closes a held sheet, keeps a tapped one open for typing. */
    static void released();
    static HotkeyCheatSheet* active();

    /** Builds the sheet for focus inside host; exposed for screenshots and tests. */
    HotkeyCheatSheet(QWidget* host, QWidget* focus);

    /** Keeps the sheet open and takes keyboard focus into its filter. */
    void pin();
    bool pinned() const { return mPinned; }
    QLineEdit* filter() const { return mFilter; }
    /** The rows the filter leaves, over every category. */
    int shownRows() const;
    /** The categories the filter leaves, in their drawn order. */
    QStringList shownCategories() const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
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

    void applyFilter();
    QRectF panelRect() const;

    std::vector<Group> mGroups;
    std::vector<Group> mShown;
    QString mTitle;
    QLineEdit* mFilter = nullptr;
    QPointer<QWidget> mFocus;
    QElapsedTimer mOpened;
    bool mPinned = false;
};
} // namespace lila::ui
