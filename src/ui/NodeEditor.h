#pragma once

#include <QWidget>

#include <functional>
#include <set>
#include <string>
#include <vector>

class QComboBox;
class QScrollArea;
class QSplitter;
class QTimer;

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
class NodeEditor : public QWidget
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

private:
    void openAddMenu(QPointF scenePos, const WireDrag* wire);
    void selectAfterRefresh(const std::vector<std::string>& names);
    void showError(const QString& message);
    void scheduleRefresh();
    void refresh();
    void refreshGroups();
    void rebuildSidePanel();
    void followSelection();

    QString mGroup;
    QString mSelectedNode;
    std::set<std::string> mSelectNext; // selected by the next refresh, after a paste or duplicate
    QComboBox* mGroups = nullptr;
    NodeCanvas* mCanvas = nullptr;
    QScrollArea* mSide = nullptr;
    QTimer* mRefresh = nullptr;
    int mListener = 0;
    bool mRefreshing = false;
};

/** Registers "window.nodeEditor", "gn.newGroup" and the nodeEditor.* editing actions. The first two need the show hook to bring the dock up. */
void registerNodeEditorActions();

/** How the actions show the editor: the main window raises its dock. */
void setNodeEditorShowHook(std::function<void()> show);
} // namespace lila::ui
