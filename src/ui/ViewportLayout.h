#pragma once

#include "core/Export.h"

#include <QWidget>

#include <array>

class QGridLayout;

namespace lila::ui
{
class ViewportPanel;
class ViewportWidget;

/**
 * The central viewport area: one view, or Maya's four (top, persp, front, side). Tapping Space toggles between them
 * around the view under the cursor. Actions that target "the viewport" act on the active one: the last clicked.
 * Each panel's view, its camera poses and the camera node it looks through save with the scene ("ui"/"viewports").
 */
class LILA_UI_API ViewportLayout : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportLayout(QWidget* parent = nullptr);

    ViewportWidget* active() const { return mActive; }
    /** Top, persp, front and side panels, in Maya's quad order. */
    const std::array<ViewportWidget*, 4>& views() const { return mViews; }
    bool quad() const { return mQuad; }
    void setQuad(bool on);
    void toggleQuad();

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void relayout();

    std::array<ViewportPanel*, 4> mPanels{}; // each a view with its toolbar
    std::array<ViewportWidget*, 4> mViews{}; // top, persp, front, side (Maya's quad order)
    ViewportWidget* mActive = nullptr;
    QGridLayout* mGrid = nullptr;
    bool mQuad = false;
};
} // namespace lila::ui
