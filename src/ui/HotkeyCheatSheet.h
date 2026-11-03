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
 * by category. Bound to F1: held, it shows until the release; tapped, or opened any other way (menu, shelf, script),
 * it stays open with its filter focused until F1, Esc or a click outside it.
 */
class LILA_UI_API HotkeyCheatSheet : public QWidget
{
    Q_OBJECT

public:
    /** Opens the sheet (pinned unless a key is opening it), or closes a pinned one. */
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
    /** Row height in pixels; it never drops below a readable size, the list scrolls instead. */
    double lineHeight() const { return mLayout.line; }
    int columns() const { return mLayout.columns; }
    /** How far the list can scroll; zero when everything fits. */
    double maxScroll() const;
    double scroll() const { return mScroll; }
    void scrollTo(double y);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

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

    /** A category header (row -1, repeated as a continuation at a column top) or a row placed in list space. */
    struct Placed
    {
        const Group* group;
        int row;
        int column;
        bool cont;
        QPointF pos;
    };
    struct Layout
    {
        double line = 0;
        int columns = 1;
        double colWidth = 0;
        /** Per column, sized to that column's widest key. */
        std::vector<double> keyWidth;
        double height = 0;
        std::vector<Placed> items;
    };

    void applyFilter();
    void relayout();
    QRectF panelRect() const;
    /** The scrolling region under the title and filter. */
    QRectF listRect() const;

    std::vector<Group> mGroups;
    std::vector<Group> mShown;
    Layout mLayout;
    double mScroll = 0;
    QString mTitle;
    QLineEdit* mFilter = nullptr;
    QPointer<QWidget> mFocus;
    QElapsedTimer mOpened;
    bool mPinned = false;
};
} // namespace lila::ui
