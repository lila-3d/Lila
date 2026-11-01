#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QIcon;
class QLabel;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace lila::brush::ui
{
class StrokeOptions;
}

namespace lila::ui
{
class FlowLayout;

/** Dock object name for the sculpt panel. */
inline constexpr const char* kSculptDock = "sculptPanel";

/**
 * Sculpt tool panel: Enter/Exit Sculpt with the proxy options, brush buttons (registered brushes included),
 * radius/strength/falloff/spacing and which of them brushes share, symmetry, the display material and wireframe, polypaint,
 * mask and freeze operations, and the standalone reprojection tools.
 */
class LILA_UI_API SculptPanel : public QWidget
{
public:
    explicit SculptPanel(QWidget* parent = nullptr);
    ~SculptPanel() override;

private:
    void syncFromSettings();
    void refreshSession();
    QToolButton* brushButton(const QIcon& icon, const QString& label, const QString& doc);
    void rebuildCustomBrushes();

    QGroupBox* mBrushBox = nullptr;
    FlowLayout* mBrushFlow = nullptr;
    QButtonGroup* mBrushes = nullptr;
    /** Registered brush names, in button order. */
    std::vector<std::string> mCustomBrushes;
    int mBrushRegistryListener = 0;
    QToolButton* mColor = nullptr;
    QCheckBox* mPaint = nullptr;
    QCheckBox* mShareRadius = nullptr;
    QCheckBox* mShareStrength = nullptr;
    QCheckBox* mShareStroke = nullptr;
    QCheckBox* mWireframe = nullptr;
    QComboBox* mMaterial = nullptr;
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
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
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
LILA_UI_API void registerSculptUi();
} // namespace lila::ui
