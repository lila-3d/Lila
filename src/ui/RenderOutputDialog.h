#pragma once

#include "core/Export.h"

#include <QDialog>

#include <nlohmann/json.hpp>

#include <string>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace lila::ui
{
/**
 * What a render looks through and writes: camera, size, samples, motion blur, frame range, images and video. The
 * settings save with the scene (RenderSettings::output) and are what the render commands use when a flag is left out.
 */
class LILA_UI_API RenderOutputDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RenderOutputDialog(QWidget* parent = nullptr);
    ~RenderOutputDialog() override;

    static void open(QWidget* parent);
    /** Renders the current frame with the window's settings and shows it in the open window; the image's path, empty on failure. */
    static std::string renderFrame();
    /** Renders the frame range into images and video with the window's settings. */
    static void renderSequence();

private:
    void refresh();
    void store(const char* key, const nlohmann::json& value);
    void showImage(const QString& path);

    int mListener = 0;
    QComboBox* mCamera = nullptr;
    QDoubleSpinBox* mScale = nullptr;
    QSpinBox* mSamples = nullptr;
    QCheckBox* mMotionBlur = nullptr;
    QCheckBox* mTransparent = nullptr;
    QDoubleSpinBox* mStart = nullptr;
    QDoubleSpinBox* mEnd = nullptr;
    QDoubleSpinBox* mStep = nullptr;
    QLineEdit* mImages = nullptr;
    QLineEdit* mVideo = nullptr;
    QComboBox* mCodec = nullptr;
    QDoubleSpinBox* mQuality = nullptr;
    QLabel* mSize = nullptr;
    QLabel* mPreview = nullptr;
};
} // namespace lila::ui
