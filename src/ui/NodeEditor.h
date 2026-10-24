#pragma once

#include "core/Export.h"
#include "ui/GraphBackend.h"

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

/** Dock object name for the node editor dock, which opens on geometry nodes. */
inline constexpr const char* kNodeEditorDock = "nodeEditor";

/**
 * Node editor: a canvas for one group plus a side panel with the group's exposed inputs and the selected node's
 * values, over whichever GraphBackend it is opened on. Every edit goes through the backend's commands, so it all
 * undoes. Follows the selection the way the backend says: for geometry nodes, picking a mesh with a geometry-nodes
 * modifier opens its group.
 */
class LILA_UI_API NodeEditor : public QWidget
{
public:
    /** backend: a registered graph backend id; an unknown one falls back to geometry nodes. */
    explicit NodeEditor(QWidget* parent = nullptr, const std::string& backend = kGeometryNodesBackend);
    ~NodeEditor() override;

    /** Switches to another registered backend, clearing the group on show. False when id isn't registered. */
    bool setBackend(const std::string& id);
    const GraphBackend& backend() const { return *mBackend; }

    void setGroup(const QString& name);
    QString group() const { return mGroup; }
    /** Shows the group with one node selected and framed. */
    void showNode(const QString& group, const std::string& node);
    void frameAll();
    void frameSelected();

    /** The editor the nodeEditor.* actions act on: the focused one, else the last focused. */
    static NodeEditor* active();

    /** Add-node search under the cursor. While a wire is dragged it lists only nodes that connect to it, and links the new one. */
    void addNodeAtCursor();
    void deleteSelected();
    /** Puts the selected nodes on the clipboard as text (CopyNodes), to paste here, in another scene or a post. */
    void copySelected();
    void cutSelected();
    /** Pastes clipboard text at the cursor; text that isn't valid node data reports why and changes nothing. */
    void paste();
    void duplicateSelected();
    void selectAll();
    /** Collapses the selected nodes into a new group run by a Group node in their place (GroupNodes). */
    void groupSelected();
    /** Inlines the selected Group node's nodes (Ungroup). */
    void ungroupSelected();
    /** Opens the group the selected Group node runs. */
    void openSelectedGroup();
    /** Writes the group to a file picked in a dialog. */
    void exportGroup();
    /** Reads group files into the scene and shows the last file's main group. */
    void importGroups(const QStringList& files);
    /** Previews a node through the backend: for geometry nodes, in the group's Viewer. */
    void viewNode(const std::string& node);
    /** What the Send button acts on, as the backend picks it. */
    std::vector<std::string> sendTargets() const;
    /** Runs the backend's SetValue for a side-panel field, keeping the panel (and the field being dragged) in place. */
    void setValueFromField(const nlohmann::json& call);

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
    void followOwner(const GraphBackend::Owner& owner);
    /** The followed owner's path when it runs the group on show, else empty. */
    std::string ownerInView() const;
    /** The followed owner's input values by plug, to tell real edits from evaluation writes. */
    nlohmann::json shownValues() const;
    void buildToolbar();
    /** Runs a backend verb from the UI; failures go to the log. */
    nlohmann::json run(std::string_view verb, const nlohmann::json& call) const;
    QWidget* buildInterface(QWidget* panel, const nlohmann::json& info);
    void editInputInfo(const std::string& input, const nlohmann::json& current);

    GraphBackendPtr mBackend;
    QString mGroup;
    QString mSelectedNode;
    /** The owner the selection led to; Parameters edit its values, which are what the scene uses, not the group's defaults. */
    QString mOwner;
    nlohmann::json mShownValues;
    bool mValueCheck = false;
    std::set<std::string> mSelectNext; // selected by the next refresh, after a paste or duplicate
    QWidget* mBar = nullptr;
    QComboBox* mGroups = nullptr;
    NodeCanvas* mCanvas = nullptr;
    QScrollArea* mSide = nullptr;
    QTimer* mRefresh = nullptr;
    QToolButton* mSendBtn = nullptr;
    int mListener = 0;
    int mExecuteListener = 0;
    int mBackendListener = 0;
    bool mRefreshing = false;
    bool mPanelDirty = false;
};

/**
 * Registers the geometry-nodes backend, "window.nodeEditor", "gn.newGroup" and the nodeEditor.* editing actions. The
 * window and new-group actions need the show hook to bring the dock up.
 */
LILA_UI_API void registerNodeEditorActions();

/** How the actions show the editor: the main window raises its dock. */
LILA_UI_API void setNodeEditorShowHook(std::function<void()> show);
} // namespace lila::ui
