#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "ui/OutlinerFilter.h"

#include <QTreeWidget>
#include <QWidget>

#include <set>
#include <unordered_map>
#include <vector>

namespace lila::ui
{
/** Mime format of an outliner MMB drag: the dragged nodes' names, one per line, so views can take the drop. */
inline constexpr const char* kNodeNamesMime = "application/x-lila-node-names";

/**
 * Maya's outliner: the DAG hierarchy with a type icon per row, referenced rows badged and italic, hidden rows dimmed.
 * LMB drag selects a range of rows; MMB drag moves rows onto another to parent them, or between rows to place them
 * there; dropped on a viewport, a camera row makes that view look through it. Right-click for the common object actions, F with the cursor over it to reveal the selection. Rows start
 * collapsed and keep their open state across rebuilds; ancestors of the selection are shaded.
 */
class LILA_UI_API Outliner : public QTreeWidget
{
    Q_OBJECT

public:
    explicit Outliner(QWidget* parent = nullptr);
    ~Outliner() override;

    /** Expands the parents of the selected objects and scrolls the first one into view. */
    void revealSelected();
    /** Starts editing the name of the first selected object's row. */
    void renameSelected();
    void expandEverything();
    void collapseEverything();
    /** Rebuilds the rows once the event loop comes round, however many changes arrive first. */
    void scheduleRebuild();
    /** Re-reads each row's icon and colour on the next event loop pass, without rebuilding the tree. */
    void scheduleRestyle();
    /** Shows only the rows the filter matches, with their ancestors dimmed and opened so the hierarchy reads. */
    void setFilter(outliner::Filter filter);

protected:
    void resizeEvent(QResizeEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    /** The rename key pressed again while a name is being edited cancels the edit. */
    void keyPressEvent(QKeyEvent* e) override;

private:
    void rebuild();
    /** Expands the parents of the rows and scrolls the first one into view. */
    void reveal(const std::vector<scene::NodeId>& ids, QAbstractItemView::ScrollHint hint = QAbstractItemView::EnsureVisible);
    /** Updates each row's icon, font and color from its node's type, reference and visibility. */
    void restyle();
    void syncSelectionFromScene();
    void pushSelectionToScene();
    void showContextMenu(const QPoint& pos);
    /** Object sets as rows after the hierarchy, each with its members as rows under it. */
    void addSetRows();
    /** The right-click menu of a set row or one of its member rows. */
    void showSetMenu(QTreeWidgetItem* item, const QPoint& globalPos);
    void startMove();
    /** Shades the rows of the selection's unselected ancestors. */
    void shadeAncestors();
    /** Hides the rows the filter leaves out and opens the ones leading to a match. */
    void applyFilter();

    std::unordered_map<scene::NodeId, QTreeWidgetItem*> mItems;
    /** Rows start collapsed, as in Maya; these were opened. */
    std::set<scene::NodeId> mExpanded;
    std::vector<uint64_t> mEventSubs;
    /** Set rows start collapsed; these were opened. */
    std::set<scene::NodeId> mOpenSets;
    int mListener = 0;
    bool mQueued = false;
    bool mRestyleQueued = false;
    bool mSyncing = false;
    /** Rows being moved by an MMB drag. */
    std::vector<scene::NodeId> mMoving;
    QPoint mMovePress;
    /** Set while an LMB drag selects rows, so the scene hears one selection at the end instead of one per row. */
    bool mDragSelecting = false;
    bool mSelectionPending = false;
    /** A rebuild was skipped while the pane was collapsed to nothing; it runs when the pane opens. */
    bool mStale = false;
    outliner::Filter mFilter;
    /** Ancestors kept only to place a match, drawn dimmed. */
    std::set<scene::NodeId> mKept;
};

/** Expands every visible outliner down to the selected objects and scrolls to them. */
LILA_UI_API void revealSelectionInOutliners();

/**
 * The outliner panel: an icon row (iconBars::kOutliner) on top, then Maya's hidden second outliner sits under the first, collapsed to a grab handle at the bottom.
 * Drag the handle up to open it; each pane scrolls, expands and filters on its own. The split is remembered. Each pane
 * has a filter box on top (see outliner::Filter for the syntax).
 */
class LILA_UI_API OutlinerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit OutlinerPanel(QWidget* parent = nullptr);
};
} // namespace lila::ui
