#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QButtonGroup;
class QComboBox;
class QDoubleSpinBox;
class QListWidget;
class QTimer;
class QToolButton;

namespace lila::ui
{
/** Dock object name for the Modeling Toolkit. */
inline constexpr const char* kModelingToolkitDock = "modelingToolkit";

/**
 * Maya's Modeling Toolkit: component modes, selection options and helpers, then collapsible sections of mesh,
 * component and tool buttons (each runs a registered action), the last operation's live settings and the selected
 * mesh's modifier stack.
 */
class LILA_UI_API ModelingToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit ModelingToolkit(QWidget* parent = nullptr);
    ~ModelingToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    /** Button for a registered action; text is a short form of the action's label and falls back to it when it is not one. */
    QToolButton* actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent);
    void refreshTooltips();
    void refreshState();

    QButtonGroup* mModes = nullptr;
    QToolButton* mSoft = nullptr;
    QDoubleSpinBox* mSoftRadius = nullptr;
    QComboBox* mSymmetry = nullptr;
    QToolButton* mCameraBased = nullptr;
    QTimer* mRefresh = nullptr;
    std::vector<QToolButton*> mButtons;
    int mGraphListener = 0;
    int mComponentListener = 0;
    int mPrefListener = 0;
};

/** Registers "window.modelingToolkit", which shows or hides the dock named kModelingToolkitDock. */
LILA_UI_API void registerModelingToolkitActions();
} // namespace lila::ui
