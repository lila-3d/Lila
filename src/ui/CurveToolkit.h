#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QButtonGroup;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTimer;
class QToolButton;

namespace lila::ui
{
/** Dock object name for the Curve Toolkit. */
inline constexpr const char* kCurveToolkitDock = "curveToolkit";

/**
 * The Curve Toolkit, the Modeling Toolkit for curves: object and point modes, the draw tools' surface options, the
 * selected curves' counts, then collapsible sections of curve tools and commands and the last operation's live settings.
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
    /** Button for a registered action, or null when it isn't one; text is a short form of its label. */
    QToolButton* actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent);
    void refreshState();

    QButtonGroup* mModes = nullptr;
    QToolButton* mOnSurface = nullptr;
    QToolButton* mKeepOnSurface = nullptr;
    QDoubleSpinBox* mOffset = nullptr;
    QSpinBox* mDegree = nullptr;
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
