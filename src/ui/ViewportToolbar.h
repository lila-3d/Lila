#pragma once

#include "core/Export.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QDoubleSpinBox;
class QFrame;
class QMenu;
class QTimer;
class QToolButton;

namespace lila::ui
{
class ViewportWidget;
enum class Tool;

/**
 * Checked state of a stateful display action ("viewport.toggleGrid", "viewport.view.top", ...) as seen from view, so
 * menus and toolbars agree; nullopt for actions that are not toggles. view may be null for the global ones.
 */
LILA_UI_API std::optional<bool> viewportActionState(const std::string& id, const ViewportWidget* view);

/**
 * Maya's camera menu for a view: the built-in views, every camera node in the scene (checked when the view looks
 * through it), Look Through Selected and Create Camera from View. `activate` runs first on each pick, so actions that
 * target the active view act on this one.
 */
LILA_UI_API void fillCameraMenu(QMenu* menu, ViewportWidget* view, std::function<void()> activate = {});

/** Fills menu with the built-in HDRIs, the procedural studio and Browse..., checking the current environment. */
LILA_UI_API void fillEnvironmentMenu(QMenu* menu);

/**
 * Maya's panel icon row: camera, gates, shading, lighting, AO, AA, environment, isolate, show filters, culling, outline,
 * level of detail and framing for one viewport. Buttons run registered actions on that viewport and track their state from prefs,
 * render settings and the view itself. Feature options the scene cannot use hide until it can (see viewportContext).
 * Whatever does not fit the width goes into a chevron menu at the end.
 */
class LILA_UI_API ViewportToolbar : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportToolbar(ViewportWidget* view, QWidget* parent = nullptr);
    ~ViewportToolbar() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    struct Item
    {
        QWidget* widget;
        std::function<void(QMenu*)> overflow; // adds the item to the chevron menu; null for separators
        uint32_t needs = 0;                   // viewportContext features any of which makes it useful; 0 for always
        std::string stateId;                  // a toggle that stays visible while on, so it can be turned off
        bool relevant = true;
    };

    QToolButton* actionButton(const char* id, const char* iconName, uint32_t needs = 0);
    QToolButton* menuButton(const char* iconName, const QString& tip, std::function<void(QMenu*)> fill);
    void separator();
    /** Makes this panel's view the active one, so actions that target "the viewport" act on it. */
    void activateView();
    void refresh();
    /** Re-decides which items the scene can use; true when any changed. */
    bool refreshRelevance();
    void refreshTooltips();
    void layoutItems();

    ViewportWidget* mView;
    std::vector<Item> mItems;
    std::vector<std::pair<QToolButton*, std::string>> mActionButtons;
    QToolButton* mCameraButton = nullptr;
    QToolButton* mSamplesButton = nullptr;
    QToolButton* mChevron = nullptr;
    int mPrefListener = 0;
    int mRenderSettingsListener = 0;
    int mToolsListener = 0;
    int mContextListener = 0;
};

/**
 * The row along the bottom of a viewport panel, in groups: the highlight under the cursor, the object selection masks,
 * the display overlays and filters, Retopo Draw's line snapping while that tool runs, symmetry, the increment snaps, and
 * exposure and gamma. Groups that do not fit the width go into a chevron menu, the highlight first and the snaps last.
 * Buttons for features the scene cannot use hide until it can (see viewportContext).
 */
class LILA_UI_API ViewportBottomBar : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportBottomBar(ViewportWidget* view, QWidget* parent = nullptr);
    ~ViewportBottomBar() override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    bool event(QEvent* e) override;

private:
    struct Group
    {
        QWidget* row;
        QFrame* line;                         // between it and its neighbour on the same side
        bool right;                           // laid out from the right edge inward
        int keep;                             // higher keeps its place longer as the bar narrows
        std::function<bool()> active;         // null for always
        std::function<void(QMenu*)> overflow; // its actions in the chevron menu
    };

    QWidget* group(bool right, int keep, std::function<void(QMenu*)> overflow, std::function<bool()> active = {});
    QToolButton* toggleButton(QWidget* row, const std::string& action, const char* iconName, uint32_t needs = 0);
    QDoubleSpinBox* field(QWidget* row, const char* key, double fallback, double lo, double hi, double step, const QString& tip);
    /** UE's snap pair: a toggle for t's increment snap and a dropdown of its steps. */
    void snapGroup(QWidget* row, Tool t, const char* iconName, std::vector<double> steps, const QString& unit);
    /** A toggle for the selected meshes' symmetry and a dropdown of its axis. */
    void buildSymmetryRow();
    void refreshSymmetry();
    /** The objects and components highlighted under the cursor before a click. */
    void buildHighlightRow();
    /** Maya's object selection masks. */
    void buildMaskRow();
    /** Origin axis, x-ray, x-ray joints, the painterly filter and the mesh display aids. */
    void buildDisplayRow();
    /** Reset, exposure and gamma. */
    void buildExposureRow();
    /** Presenter mode and, while it is on, editing its layout. */
    void buildPresenterRow();
    /** Retopo Draw's line snap angle and space and its surface, while the tool runs. */
    void buildRetopoRow();
    void refreshRetopo();
    void activateView();
    void refresh();
    /** Shows only the buttons the scene can use; true when any changed. */
    bool refreshRelevance();
    void refreshTooltips();
    void layoutItems();

    struct Relevant
    {
        QToolButton* button;
        uint32_t needs;     // viewportContext features any of which makes it useful
        std::string action; // stays visible while on, so it can be turned off
    };

    ViewportWidget* mView;
    std::vector<Group> mGroups;
    std::vector<std::pair<QToolButton*, std::string>> mActionButtons;
    std::vector<Relevant> mRelevant;
    std::vector<std::pair<QDoubleSpinBox*, std::pair<std::string, double>>> mFields;
    QToolButton* mChevron = nullptr;
    struct SnapGroup
    {
        Tool tool;
        QToolButton* toggle;
        QToolButton* value;
        QString unit;
    };
    std::vector<SnapGroup> mSnapGroups;
    QWidget* mSnapRow = nullptr;
    QWidget* mSymmetryRow = nullptr;
    QWidget* mRetopoRow = nullptr;
    QToolButton* mRetopoSpace = nullptr;
    QToolButton* mRetopoAngle = nullptr;
    QToolButton* mRetopoSurface = nullptr;
    QToolButton* mSymmetryToggle = nullptr;
    QToolButton* mPresenterLayout = nullptr;
    QToolButton* mSymmetryAxis = nullptr;
    QTimer* mSymmetryRefresh = nullptr;
    int mGraphListener = 0;
    int mPrefListener = 0;
    int mToolsListener = 0;
    int mViewportToolListener = 0;
    int mContextListener = 0;
};

/** One viewport with its toolbar above it and its bottom bar below, as a cell of the viewport layout. */
class LILA_UI_API ViewportPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportPanel(QWidget* parent = nullptr);
    ~ViewportPanel() override;

    ViewportWidget* view() const { return mView; }

private:
    ViewportWidget* mView;
    ViewportToolbar* mToolbar;
    ViewportBottomBar* mBottomBar;
    int mPrefListener = 0;
};
} // namespace lila::ui
