#pragma once

#include <QDialog>

#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace lila::ui
{
/**
 * Render resolution and per-camera gate settings. Every control runs setRenderResolution or cameraSettings as it
 * changes, so the script editor echoes it; there's no OK/Apply.
 */
class RenderSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RenderSettingsDialog(QWidget* parent = nullptr);
    ~RenderSettingsDialog() override;

    /** Opens (or raises) the dialog, showing the given camera's settings when one is named. */
    static void open(QWidget* parent, const std::string& camera = {});

private:
    void refresh();
    std::string camera() const;

    int mListener = 0;
    QComboBox* mPreset = nullptr;
    QSpinBox* mWidth = nullptr;
    QSpinBox* mHeight = nullptr;
    QDoubleSpinBox* mDeviceAspect = nullptr;
    QLabel* mPixelAspect = nullptr;
    QComboBox* mCamera = nullptr;
    QComboBox* mFilmFit = nullptr;
    QDoubleSpinBox* mOverscan = nullptr;
    QCheckBox* mGate = nullptr;
    QCheckBox* mFilmGate = nullptr;
    QCheckBox* mMask = nullptr;
    QDoubleSpinBox* mMaskOpacity = nullptr;
};
} // namespace lila::ui
