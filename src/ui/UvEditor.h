#pragma once

#include "core/Export.h"

#include <QWidget>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

class QComboBox;
class QDoubleSpinBox;
class QHideEvent;
class QLabel;
class QScrollArea;
class QSplitter;
class QShowEvent;
class QSpinBox;
class QToolButton;

namespace lila::ui
{
/** Dock object name for the UV editor. */
inline constexpr const char* kUvEditorDock = "uvEditor";

class UvView;
class UvPackPanel;

/**
 * UV editor: a 2D view of the hilited meshes' uvs with selection synced to the 3D component selection, a toolbar of
 * tools, modes and display, and the UV Toolkit column beside the view with the uv commands in sections.
 */
class LILA_UI_API UvEditor : public QWidget
{
    Q_OBJECT

public:
    explicit UvEditor(QWidget* parent = nullptr);
    ~UvEditor() override;

    UvView* view() const { return mView; }
    /** Shows the UV Toolkit with its Pack options open. */
    void showPackOptions();
    /** Shows the UV Toolkit column beside the view. */
    void showToolkit();

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private:
    /** Runs a command from the UI (echoed), logging failures; returns its result or null. */
    nlohmann::json runCommand(const char* command, const nlohmann::json& call = nlohmann::json::object());
    void refreshSets();
    void showLayout();
    /** uvPack with the Pack panel's options and the editor's resolution and padding. */
    void runPack();

    UvView* mView = nullptr;
    UvPackPanel* mPackPanel = nullptr;
    QToolButton* mPackOptions = nullptr;
    QToolButton* mPackSection = nullptr;
    QToolButton* mToolkitToggle = nullptr;
    QScrollArea* mToolkitScroll = nullptr;
    QSplitter* mSplit = nullptr;
    bool mToolkitSized = false;
    QComboBox* mSet = nullptr;
    QComboBox* mDisplay = nullptr;
    QSpinBox* mResolution = nullptr;
    QDoubleSpinBox* mPadding = nullptr;
    QDoubleSpinBox* mDensity = nullptr;
    QDoubleSpinBox* mMoveU = nullptr;
    QDoubleSpinBox* mMoveV = nullptr;
    QDoubleSpinBox* mRotate = nullptr;
    QDoubleSpinBox* mScaleU = nullptr;
    QDoubleSpinBox* mScaleV = nullptr;
    QLabel* mStatus = nullptr;
    int mGraphListener = 0;
    int mComponentListener = 0;
    int mLiveListener = 0;
};

/** Registers "window.uvEditor" and the uv.* actions (unwrap, auto, pack...) for menus, hotkeys and shelves. */
LILA_UI_API void registerUvActions();

/** How "window.uvEditor" toggles the panel: the main window raises its dock, or tucks it away when showing. */
LILA_UI_API void setUvEditorOpener(std::function<void()> open);

/** Shows the UV editor with its Pack options open: the uv.pack* actions' option box. */
LILA_UI_API void showUvPackOptions();

/** Shows the UV editor with its UV Toolkit column. */
LILA_UI_API void showUvToolkit();
} // namespace lila::ui
