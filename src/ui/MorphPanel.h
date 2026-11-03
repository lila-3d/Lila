#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>

class QLabel;
class QMenu;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
inline constexpr const char* kMorphTargetsDock = "morphTargets";
inline constexpr const char* kDrivenChannelsDock = "drivenChannels";

/**
 * The morph targets of the selected mesh (or the selected target's mesh): a weight slider each, the driver of a driven
 * weight, in-betweens, an edit toggle, and a toolbar to add, remove, mirror, extract and sculpt correctives. Clicking a
 * row makes it the target the Deform menu's morph tools act on.
 */
class LILA_UI_API MorphTargetsPanel : public QWidget
{
public:
    explicit MorphTargetsPanel(QWidget* parent = nullptr);
    ~MorphTargetsPanel() override;

    /** A target row's right-click menu: reset its weight, set its slider range. */
    void fillTargetMenu(QMenu* menu, QTreeWidgetItem* item);

protected:
    void showEvent(QShowEvent* e) override;

private:
    enum Column
    {
        kName,
        kSlider,
        kValue,
        kEdit,
        kInfo,
    };

    void scheduleRefresh();
    void refresh();

    QLabel* mHeader = nullptr;
    QTreeWidget* mTree = nullptr;
    std::string mMorph;
    int mGraphListener = 0;
    bool mPending = false;
};

/** Driven channels: driver to driven, curve name, and the selected channel's keys as an editable table and a curve preview. */
class LILA_UI_API DrivenChannelsPanel : public QWidget
{
public:
    explicit DrivenChannelsPanel(QWidget* parent = nullptr);
    ~DrivenChannelsPanel() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scheduleRefresh();
    void refresh();
    void refreshKeys();
    void commitKeys();
    std::string current() const;

    QTreeWidget* mTree = nullptr;
    QTableWidget* mKeys = nullptr;
    QWidget* mPlot = nullptr;
    int mGraphListener = 0;
    bool mPending = false;
    bool mFilling = false;
};

/** The morph target the morph tools act on: the panel's current row, else a selected morphTarget node. Empty for none. */
LILA_UI_API std::string currentMorphTarget();

/** Deform menu morph, corrective and driven channel actions, and the two panels with their window.* toggles. */
LILA_UI_API void registerMorphUi();
} // namespace lila::ui
