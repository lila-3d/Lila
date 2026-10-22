#pragma once

#include <QWidget>

class QLabel;
class QListWidget;
class QToolButton;

namespace lila::ui
{
inline constexpr const char* kUndoHistoryDock = "undoHistory";

/** Compact list of undo steps: click a row to undo or redo to it. */
class UndoHistoryPanel : public QWidget
{
public:
    explicit UndoHistoryPanel(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scheduleRefresh();
    void refresh();

    QListWidget* mList = nullptr;
    QToolButton* mShowSelection = nullptr;
    QToolButton* mHideSelection = nullptr;
    QLabel* mFooter = nullptr;
    bool mPending = false;
};
} // namespace lila::ui
