#pragma once

#include "core/Export.h"

#include <QWidget>

#include <nlohmann/json.hpp>

#include <functional>
#include <set>
#include <string>
#include <vector>

class QComboBox;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolButton;

namespace lila::scene
{
class Node;
}

namespace lila::ui
{
class NodeCanvas;
struct WireDrag;

/** Dock object name for the geometry-nodes editor. */
inline constexpr const char* kNodeEditorDock = "nodeEditor";

/**
 * Geometry-nodes editor: a canvas for one group plus a side panel with the group's exposed inputs and the selected
 * node's values. Every edit goes through the gn commands, so it all undoes. Follows the selection: picking a mesh
 * with a geometry-nodes modifier opens its group.
 */
class LILA_UI_API NodeEditor : public QWidget
{
public:
    explicit NodeEditor(QWidget* parent = nullptr);
    ~NodeEditor() override;

    void setGroup(const QString& name);
    QString group() const { return mGroup; }
    void frameAll();
    void frameSelected();

    /** The editor the nodeEditor.* actions act on: the focused one, else the last focused. */
    static NodeEditor* active();

    /** Add-node search under the cursor. While a wire is dragged it lists only nodes that connect to it, and links the new one. */
    void addNodeAtCursor();
    void deleteSelected();
    /** Puts the selected nodes on the clipboard as text (gnCopyNodes), to paste here, in another scene or a post. */
    void copySelected();
    void cutSelected();
    /** Pastes clipboard text at the cursor; text that isn't valid node data reports why and changes nothing. */
    void paste();
    void duplicateSelected();
    void selectAll();
    /** Collapses the selected nodes into a new group run by a Group node in their place (gnGroupNodes). */
    void groupSelected();
    /** Inlines the selected Group node's nodes (gnUngroup). */
    void ungroupSelected();
    /** Opens the group the selected Group node runs. */
    void openSelectedGroup();
    /** Writes the group to a .lilanodes file picked in a dialog. */
    void exportGroup();
    /** Reads .lilanodes or .json files into the scene and shows the last file's main group. */
    void importGroups(const QStringList& files);
    /** Shows a node in a Viewer: reuses the group's first Viewer or adds one, links the node's outputs and activates it. */
    void viewNode(const std::string& node);
    /**
     * Meshes the Send button acts on: the selected meshes running this group, else the mesh of the modifier in view,
     * else every mesh running it.
     */
    std::vector<std::string> sendTargets() const;

private:
    void openAddMenu(QPointF scenePos, const WireDrag* wire);
    void selectAfterRefresh(const std::vector<std::string>& names);
    void showError(const QString& message);
    /** panel false: only the canvas needs it, so a field being scrubbed in the side panel survives. */
    void scheduleRefresh(bool panel = true);
    void refreshSendButton();
    void refresh(bool panel = true);
    void refreshGroups();
    void rebuildSidePanel();
    void followSelection();
    void followModifier(const scene::Node& modifier, const QString& group);
    /** The followed modifier when it uses the group on show, else null. */
    scene::Node* modifierInView() const;
    /** The followed modifier's input values by plug, to tell real edits from evaluation writes. */
    nlohmann::json shownValues() const;
    QWidget* buildInterface(QWidget* panel, const nlohmann::json& info);
    void editInputInfo(const std::string& input, const nlohmann::json& current);

    QString mGroup;
    QString mSelectedNode;
    /** The gn modifier the selection led to; Parameters edit its values, which are what the mesh uses, not the group's defaults. */
    QString mModifier;
    nlohmann::json mShownValues;
    bool mValueCheck = false;
    std::set<std::string> mSelectNext; // selected by the next refresh, after a paste or duplicate
    QComboBox* mGroups = nullptr;
    NodeCanvas* mCanvas = nullptr;
    QScrollArea* mSide = nullptr;
    QTimer* mRefresh = nullptr;
    QToolButton* mSendBtn = nullptr;
    int mListener = 0;
    int mExecuteListener = 0;
    bool mRefreshing = false;
    bool mPanelDirty = false;
};

/** Registers "window.nodeEditor", "gn.newGroup" and the nodeEditor.* editing actions. The first two need the show hook to bring the dock up. */
LILA_UI_API void registerNodeEditorActions();

/** How the actions show the editor: the main window raises its dock. */
LILA_UI_API void setNodeEditorShowHook(std::function<void()> show);
} // namespace lila::ui
