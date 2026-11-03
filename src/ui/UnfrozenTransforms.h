#pragma once

#include "core/Export.h"
#include "scene/Plug.h"

#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;

namespace lila::ui
{
inline constexpr const char* kUnfrozenTransformsDock = "unfrozenTransforms";

/**
 * Lists the loaded objects (or the scene) whose transforms are not frozen, from the unfrozenTransforms command, with
 * channel and rotate step filters that update the list as they change. Picking rows selects those objects without
 * changing the list, so it doubles as a selector for stepping through them.
 */
class LILA_UI_API UnfrozenTransformsPanel : public QWidget
{
public:
    explicit UnfrozenTransformsPanel(QWidget* parent = nullptr);
    ~UnfrozenTransformsPanel() override;

    /** Checks the current selection from now on, or the whole scene when nothing is selected. */
    void loadSelection();

protected:
    void showEvent(QShowEvent* e) override;

private:
    void scheduleRefresh(int delayMs = 0);
    void refresh();
    void selectRows();
    void syncFromScene();
    void updateButtons();

    QComboBox* mScope = nullptr;
    QPushButton* mLoad = nullptr;
    QCheckBox* mTranslate = nullptr;
    QCheckBox* mRotate = nullptr;
    QCheckBox* mScale = nullptr;
    QComboBox* mStep = nullptr;
    QCheckBox* mInvert = nullptr;
    QTreeWidget* mList = nullptr;
    QLabel* mCount = nullptr;
    QPushButton* mSelectAll = nullptr;
    QPushButton* mFreeze = nullptr;
    QTimer* mRefreshTimer = nullptr;
    std::vector<scene::NodeId> mLoaded;
    int mGraphListener = 0;
    bool mSyncing = false;
    bool mSyncPending = false;
};

/** The panel, its window.unfrozenTransforms toggle and modify.findUnfrozenTransforms, which loads the selection into it. */
LILA_UI_API void registerUnfrozenTransformsPanel();
} // namespace lila::ui
