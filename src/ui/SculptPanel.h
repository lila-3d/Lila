#pragma once

#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace lila::ui
{
/** Dock object name for the sculpt panel. */
inline constexpr const char* kSculptDock = "sculptPanel";

/**
 * Sculpt tool panel: Enter/Exit Sculpt with the proxy options, brush buttons, radius/strength/falloff/spacing,
 * symmetry, mask and freeze operations, and the standalone reprojection tools.
 */
class SculptPanel : public QWidget
{
public:
    explicit SculptPanel(QWidget* parent = nullptr);
    ~SculptPanel() override;

private:
    void syncFromSettings();
    void refreshSession();

    QButtonGroup* mBrushes = nullptr;
    QComboBox* mFalloff = nullptr;
    QDoubleSpinBox* mRadius = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QDoubleSpinBox* mSpacing = nullptr;
    QCheckBox* mFrontFaces = nullptr;
    QCheckBox* mShowCage = nullptr;
    QComboBox* mMode = nullptr;
    QSpinBox* mLevels = nullptr;
    QSpinBox* mResolution = nullptr;
    QComboBox* mExitMethod = nullptr;
    QCheckBox* mKeepProxy = nullptr;
    QComboBox* mSymmetry = nullptr;
    QLabel* mStatus = nullptr;
    QPushButton* mEnter = nullptr;
    QPushButton* mExit = nullptr;
    QPushButton* mCancel = nullptr;
    int mSettingsListener = 0;
    int mSessionListener = 0;
    int mGraphListener = 0;
    bool mRefreshQueued = false;
};

/**
 * Registers the sculpt actions (window.sculpt, sculpt.tool, sculpt.enter, sculpt.exit, sculpt.cancel, sculpt.brush.*,
 * sculpt.mask*, sculpt.freezeClear, mesh.relaxKeepForm, mesh.reproject, mesh.transferShape) and the viewport drawer.
 */
void registerSculptUi();
} // namespace lila::ui
