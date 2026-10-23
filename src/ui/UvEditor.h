#pragma once

#include "core/Export.h"

#include <QWidget>

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace lila::ui
{
/** Dock object name for the UV editor. */
inline constexpr const char* kUvEditorDock = "uvEditor";

class UvView;
class UvPackPanel;

/**
 * UV editor: a 2D view of the hilited meshes' uvs with selection synced to the 3D component selection,
 * a move/rotate/scale manipulator, numeric transforms and buttons for the uv commands.
 */
class LILA_UI_API UvEditor : public QWidget
{
    Q_OBJECT

public:
    explicit UvEditor(QWidget* parent = nullptr);
    ~UvEditor() override;

    UvView* view() const { return mView; }

private:
    /** Runs a command from the UI (echoed), logging failures; returns its result or null. */
    nlohmann::json runCommand(const char* command, const nlohmann::json& call = nlohmann::json::object());
    void refreshSets();
    void showLayout();
    /** uvPack with the Pack panel's options and the editor's resolution and padding. */
    void runPack();

    UvView* mView = nullptr;
    UvPackPanel* mPackPanel = nullptr;
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
} // namespace lila::ui
