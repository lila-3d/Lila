#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QDoubleSpinBox;
class QLabel;
class QTimer;
class QToolButton;

namespace lila::ui
{
namespace controls
{
class Segmented;
}

/** Dock object name for the Curve Toolkit. */
inline constexpr const char* kCurveToolkitDock = "curveToolkit";

/**
 * The Curve Toolkit, the Modeling Toolkit for curves: object and point modes, the selected curves' counts, the draw tools
 * as tiles and their options, symmetry, labelled groups of curve commands with the rest in overflow menus, collapsed
 * sections for rarer work and the last operation's live settings.
 */
class LILA_UI_API CurveToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit CurveToolkit(QWidget* parent = nullptr);
    ~CurveToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    /** A tile for a registered action, or null when it isn't one. */
    QToolButton* tile(const std::string& id, const char* iconName, const QString& label, QWidget* parent);
    /** A labelled push button for a registered action, or null when it isn't one. */
    QToolButton* push(const std::string& id, const QString& label, QWidget* parent);
    QToolButton* track(QToolButton* b, const std::string& id);
    void refreshState();

    controls::Segmented* mMode = nullptr;
    controls::Segmented* mDrawSymmetry = nullptr;
    controls::Segmented* mCurveSymmetry = nullptr;
    QToolButton* mOnSurface = nullptr;
    QToolButton* mKeepOnSurface = nullptr;
    QDoubleSpinBox* mOffset = nullptr;
    QDoubleSpinBox* mDegree = nullptr;
    QLabel* mInfo = nullptr;
    QTimer* mRefresh = nullptr;
    std::vector<QToolButton*> mButtons;
    int mGraphListener = 0;
    int mComponentListener = 0;
    int mPrefListener = 0;
};

/** Registers "window.curveToolkit", which shows or hides the dock named kCurveToolkitDock. */
LILA_UI_API void registerCurveToolkitActions();
} // namespace lila::ui
