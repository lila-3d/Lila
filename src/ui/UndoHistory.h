#pragma once

#include "core/Export.h"

#include <QWidget>

class QLabel;
class QListWidget;

namespace lila::ui::controls
{
class Segmented;
}

namespace lila::ui
{
inline constexpr const char* kUndoHistoryDock = "undoHistory";

/**
 * Compact list of undo steps by their display labels: click a row to undo or redo to it. The footer filters selection
 * steps, holds the flush actions in its overflow menu and shows the history's size.
 */
class LILA_UI_API UndoHistoryPanel : public QWidget
{
public:
    explicit UndoHistoryPanel(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scheduleRefresh();
    void refresh();

    QListWidget* mList = nullptr;
    controls::Segmented* mSteps = nullptr;
    QLabel* mFooter = nullptr;
    bool mPending = false;
};
} // namespace lila::ui
