#pragma once

#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QDoubleSpinBox;
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
std::optional<bool> viewportActionState(const std::string& id, const ViewportWidget* view);

/**
 * Maya's camera menu for a view: the built-in views, every camera node in the scene (checked when the view looks
 * through it), Look Through Selected and Create Camera from View. `activate` runs first on each pick, so actions that
 * target the active view act on this one.
 */
void fillCameraMenu(QMenu* menu, ViewportWidget* view, std::function<void()> activate = {});

/** Fills menu with the built-in HDRIs, the procedural studio and Browse..., checking the current environment. */
void fillEnvironmentMenu(QMenu* menu);

/**
 * Maya's panel icon row: camera, gates, shading, lighting, AO, AA, environment, isolate, x-ray, framing, exposure and
 * gamma for one viewport. Buttons run registered actions on that viewport and track their state from prefs, render
 * settings and the view itself. Whatever does not fit the width goes into a chevron menu at the end.
 */
class ViewportToolbar : public QWidget
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
    };

    QToolButton* actionButton(const char* id, const char* iconName);
    QToolButton* menuButton(const char* iconName, const QString& tip, std::function<void(QMenu*)> fill);
    QDoubleSpinBox* field(const char* key, double fallback, double lo, double hi, double step, const QString& tip);
    void separator();
    /** UE's snap pair at the right end: a toggle for t's increment snap and a dropdown of its steps. */
    void snapGroup(Tool t, const char* iconName, std::vector<double> steps, const QString& unit);
    /** The far-right symmetry pair: a toggle for the selected meshes' symmetry and a dropdown of its axis. */
    void buildSymmetryRow();
    void refreshSymmetry();
    /**
     * The object highlight under the cursor and Maya's object selection masks, centered in the gap between the left
     * row and the right-hand groups.
     */
    void buildMaskRow();
    /** Makes this panel's view the active one, so actions that target "the viewport" act on it. */
    void activateView();
    void refresh();
    void refreshTooltips();
    void layoutItems();

    ViewportWidget* mView;
    std::vector<Item> mItems;
    std::vector<std::pair<QToolButton*, std::string>> mActionButtons;
    std::vector<std::pair<QDoubleSpinBox*, std::pair<std::string, double>>> mFields;
    QToolButton* mCameraButton = nullptr;
    QToolButton* mSamplesButton = nullptr;
    QToolButton* mChevron = nullptr;
    struct SnapGroup
    {
        Tool tool;
        QToolButton* toggle;
        QToolButton* value;
        QString unit;
    };
    std::vector<SnapGroup> mSnapGroups;
    QWidget* mSnapRow = nullptr; // right-aligned, outside the overflowing left row
    QWidget* mSymmetryRow = nullptr; // right of the snap row
    QWidget* mMaskRow = nullptr;
    std::vector<QAction*> mMaskActions;
    QToolButton* mSymmetryToggle = nullptr;
    QToolButton* mSymmetryAxis = nullptr;
    QTimer* mSymmetryRefresh = nullptr;
    int mGraphListener = 0;
    int mPrefListener = 0;
    int mRenderSettingsListener = 0;
    int mToolsListener = 0;
};

/** One viewport with its toolbar above it, as a cell of the viewport layout. */
class ViewportPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ViewportPanel(QWidget* parent = nullptr);
    ~ViewportPanel() override;

    ViewportWidget* view() const { return mView; }

private:
    ViewportWidget* mView;
    ViewportToolbar* mToolbar;
    int mPrefListener = 0;
};
} // namespace lila::ui
