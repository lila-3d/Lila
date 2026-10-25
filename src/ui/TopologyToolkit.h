#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace lila::ui
{
/** Dock object name for the Topology Toolkit. */
inline constexpr const char* kTopologyToolkitDock = "topologyToolkit";

/**
 * The Topology Toolkit: the retopo session (reference, retopo mesh, symmetry), the topology tools, their settings, the
 * active construct's settings, the list of live constructs, guides, auto retopo and the quality numbers.
 */
class LILA_UI_API TopologyToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit TopologyToolkit(QWidget* parent = nullptr);
    ~TopologyToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    QToolButton* actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent);
    void refreshState();
    void rebuildConstructPanel();

    QLabel* mSession = nullptr;
    QLabel* mStats = nullptr;
    QLabel* mActiveLabel = nullptr;
    QWidget* mConstructPanel = nullptr;
    QVBoxLayout* mConstructLayout = nullptr;
    QListWidget* mList = nullptr;
    QTimer* mRefresh = nullptr;
    std::vector<QToolButton*> mButtons;
    std::string mPanelKey;
    int mGraphListener = 0;
    int mToolListener = 0;
    int mPrefListener = 0;
};

/** Registers "window.topologyToolkit", which shows or hides the dock named kTopologyToolkitDock. */
LILA_UI_API void registerTopologyToolkitActions();
} // namespace lila::ui
