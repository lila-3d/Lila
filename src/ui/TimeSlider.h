#pragma once

#include "core/Export.h"

#include <QWidget>

class QComboBox;
class QSpinBox;
class QToolButton;

namespace lila::ui
{
class TimeRuler;
class RangeBar;

/**
 * Maya's time and range sliders in one slim strip. Top row: frame ruler with the selection's key ticks, the current
 * frame and the playback buttons. Bottom row: the range slider between its start/end fields, fps, loop and auto
 * key. Drag the ruler to scrub; right-click it for key actions. Times are seconds underneath and shown as frames.
 */
class LILA_UI_API TimeSlider : public QWidget
{
public:
    explicit TimeSlider(QWidget* parent = nullptr);
    ~TimeSlider() override;

    QSize sizeHint() const override;

private:
    void refreshOptions();
    void refreshTime();

    TimeRuler* mRuler = nullptr;
    RangeBar* mRange = nullptr;
    QSpinBox* mFrame = nullptr;
    QSpinBox* mAnimStart = nullptr;
    QSpinBox* mMin = nullptr;
    QSpinBox* mMax = nullptr;
    QSpinBox* mAnimEnd = nullptr;
    QComboBox* mFps = nullptr;
    QToolButton* mPlay = nullptr;
    QToolButton* mPlayBack = nullptr;
    QToolButton* mLoop = nullptr;
    QToolButton* mAutoKey = nullptr;
    int mGraphListener = 0;
    int mPlaybackListener = 0;
};
} // namespace lila::ui
