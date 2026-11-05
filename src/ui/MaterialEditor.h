#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <QWidget>

#include <functional>
#include <vector>

class QLineEdit;
class QListWidget;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace lila::scene
{
class Node;
}

namespace lila::ui
{
/** Dock object name of the material editor. */
inline constexpr const char* kMaterialEditorDock = "materialEditor";

/**
 * Lists the scene's materials and edits the selected one: sliders, color swatches, texture slots with thumbnails, a
 * fake-shaded preview ball and an assign-to-selection button. Every edit goes through commands, so it is undoable;
 * a slider drag is one undo step.
 */
class LILA_UI_API MaterialEditor : public QWidget
{
    Q_OBJECT

public:
    explicit MaterialEditor(QWidget* parent = nullptr);
    ~MaterialEditor() override;

    void showMaterial(const std::string& name);
    /** Side-panel width, like the channel box, rather than whatever the scroll area's contents ask for. */
    QSize sizeHint() const override { return {340, 640}; }

private:
    /** Shows the first material of the last selected mesh. */
    void followSelection();
    void scheduleRefresh();
    void refreshList();
    /** Hides the materials whose names don't contain the filter text, ignoring case. */
    void applyFilter();
    void rebuild();
    void updateValues();
    /** Keeps the body wide enough for its rows, so it never scrolls sideways. */
    void fitWidth();
    void setAttr(int attr, const scene::json& value);
    scene::Node* current() const;

    QLineEdit* mFilter = nullptr;
    QListWidget* mList = nullptr;
    QLabel* mPreview = nullptr;
    QScrollArea* mScroll = nullptr;
    QWidget* mBody = nullptr;
    QVBoxLayout* mBodyLayout = nullptr;
    QPushButton* mAssign = nullptr;
    scene::NodeId mNode = scene::kNoNode;
    std::vector<std::function<void()>> mUpdaters;
    int mListener = 0;
    int mTextureListener = 0;
    bool mQueued = false;
    bool mDragging = false;
    int mShownModel = -1; // shading model whose inputs the panel shows
};

/**
 * Registers "window.materialEditor" (shows the dock named kMaterialEditorDock, or a floating editor when the main
 * window has none), "material.assignNew" (creates a material and assigns it to the selection) and
 * "viewport.textured" (shaded with materials in the viewport; hotkey 6). Also registers the material node type and
 * commands when the scene has not yet.
 */
LILA_UI_API void registerMaterialUi();
} // namespace lila::ui
