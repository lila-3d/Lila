#pragma once

#include <QFrame>

class QLabel;
class QToolButton;

namespace lila::ui
{
/**
 * The UV Toolkit's Seam View row: picks the seam view override (uvSeamViewOverride) that Auto UV, Auto Seams and Hose UVs
 * hide seams from instead of their options' views. While one is set the row turns into a warning plate naming it, with a
 * button that clears it, and their option boxes carry the same warning.
 */
class SeamOverrideRow : public QFrame
{
public:
    explicit SeamOverrideRow(QWidget* parent = nullptr);
    ~SeamOverrideRow() override;

    /** Reads the override from the scene and redraws. */
    void refresh();

private:
    void fillMenu();

    QLabel* mLabel = nullptr;
    QToolButton* mPick = nullptr;
    QToolButton* mClear = nullptr;
    int mListener = -1;
    bool mQueued = false;
};
} // namespace lila::ui
