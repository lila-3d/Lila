#pragma once

#include "core/Export.h"

#include "scene/SoftSelect.h"

#include <QWidget>

#include <functional>

class QToolButton;

namespace lila::ui
{
namespace controls
{
class Segmented;
}
class FlowLayout;

/**
 * Compact editor for a soft select falloff curve. Drag keys, click empty space to add one, right-click a key to delete
 * it or set its interpolation, double-click to reset. In draw mode a freehand stroke replaces the curve.
 */
class LILA_UI_API FalloffCurveEditor : public QWidget
{
public:
    explicit FalloffCurveEditor(QWidget* parent = nullptr);

    void setCurve(const scene::softSelect::Curve& c);
    const scene::softSelect::Curve& curve() const { return mCurve; }
    void setDrawMode(bool on);
    bool drawMode() const { return mDrawMode; }
    bool editing() const { return mDrag >= 0 || !mStroke.empty(); }

    /** Called on every edit, while dragging too. */
    std::function<void(const scene::softSelect::Curve&)> onEdited;
    std::function<void(bool)> onDrawModeChanged;

    QSize sizeHint() const override { return {200, 110}; }
    QSize minimumSizeHint() const override { return {120, 70}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF plotRect() const;
    QPointF toScreen(double pos, double value) const;
    glm::dvec2 toCurve(const QPointF& p) const;
    int keyAt(const QPointF& p) const;
    void edited();

    scene::softSelect::Curve mCurve;
    int mDrag = -1;
    int mHover = -1;
    bool mDrawMode = false;
    std::vector<glm::dvec2> mStroke;
};

/**
 * Soft select falloff: preset thumbnails (built-in and saved), the curve editor and the falloff mode, all bound live
 * to the general prefs under /softSelect.
 */
class LILA_UI_API SoftSelectPanel : public QWidget
{
public:
    explicit SoftSelectPanel(QWidget* parent = nullptr);
    ~SoftSelectPanel() override;

private:
    void rebuildUserPresets();
    void syncFromPrefs();
    void markCurrentPreset();
    QToolButton* thumbnail(const scene::softSelect::Preset& p, QWidget* parent, bool user);

    FalloffCurveEditor* mEditor = nullptr;
    QWidget* mPresetBox = nullptr;
    QWidget* mUserBox = nullptr;
    FlowLayout* mUser = nullptr;
    controls::Segmented* mMode = nullptr;
    QToolButton* mDraw = nullptr;
    std::vector<std::pair<QToolButton*, scene::softSelect::Curve>> mThumbs;
    nlohmann::json mUserJson;
    int mPrefListener = 0;
};
} // namespace lila::ui
