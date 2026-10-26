#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <QWidget>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class QLabel;
class QMenu;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace lila::ui
{
/** Dock object name for the Modifier Stack panel. */
inline constexpr const char* kModifierStackDock = "modifierStack";

class ModifierCardList;

/**
 * A mesh's modifier stack as Blender-style cards: the source (generator or base mesh) first, then each modifier in
 * evaluation order. Cards rename, toggle viewport and export, apply, duplicate, delete, reorder (buttons or drag by
 * the grip) and edit every parameter inline, bound live to the DG. Every edit is a registered command.
 *
 * Embeddable: a host can follow the selection (the default) or pin a mesh with setMesh, and hide the header row when
 * it has its own Add Modifier control.
 */
class LILA_UI_API ModifierStackWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ModifierStackWidget(QWidget* parent = nullptr);
    ~ModifierStackWidget() override;

    /** Pins the panel to a mesh; kNoNode follows the lead selected mesh (or the mesh of a selected modifier). */
    void setMesh(scene::NodeId mesh);
    /** The mesh being shown, kNoNode when none. */
    scene::NodeId mesh() const { return mMesh; }

    /** Shows or hides the top row (mesh name, Add Modifier, Apply All / Collapse menu). */
    void setHeaderVisible(bool on);

signals:
    /** The shown mesh changed (selection moved, or setMesh). */
    void meshChanged(scene::NodeId mesh);

protected:
    void showEvent(QShowEvent* e) override;

private:
    friend class ModifierCardList;

    void scheduleRefresh();
    void refresh();
    void rebuild();
    void updateValues();
    scene::NodeId resolveMesh() const;
    std::vector<uint64_t> signature(scene::NodeId mesh) const;
    QWidget* buildSourceCard(scene::NodeId source);
    QWidget* buildModifierCard(scene::NodeId mod, int index, int count);
    QWidget* buildHeaderFor(QWidget* card, QWidget* body, scene::NodeId id, bool isModifier, int index, int count);
    QWidget* buildParams(scene::NodeId id, bool isModifier);
    /** A connected mesh input: the source's name and a button to set it from the selection. Null when not offered. */
    QWidget* buildMeshInputRow(const scene::PlugRef& ref, QWidget* parent);
    /** An array modifier's centre object: its name, and buttons to use the selected object or the mesh's pivot. */
    QWidget* buildArrayCenterRow(scene::NodeId modifier, QWidget* parent);
    void setCollapsed(scene::NodeId id, bool collapsed, bool all);
    /** Delete: removes the modifier whose card holds focus, else the one under the cursor. */
    void removeCardModifier();
    /** Drop target: the modifier dragged to position `to` in the stack (0 is applied first). */
    void dropModifier(const std::string& name, int to);

    QWidget* mHeader = nullptr;
    QLabel* mTitle = nullptr;
    QToolButton* mAdd = nullptr;
    QToolButton* mMore = nullptr;
    QScrollArea* mScroll = nullptr;
    ModifierCardList* mList = nullptr;
    QVBoxLayout* mListLayout = nullptr;
    QLabel* mFooter = nullptr;
    QTimer* mRefresh = nullptr;

    scene::NodeId mPinned = scene::kNoNode;
    scene::NodeId mMesh = scene::kNoNode;
    std::vector<uint64_t> mSignature;
    std::vector<std::function<void()>> mUpdaters;
    std::set<scene::NodeId> mCollapsed;
    std::map<scene::NodeId, std::pair<QWidget*, QWidget*>> mCards; // id -> (card, body)
    int mListener = 0;
    bool mBuilt = false;
};

/**
 * Fills menu with every registered modifier kind (from the type registry, so extension modifiers appear). Picking one adds
 * it to the selected meshes and shows the Modifier Stack panel. The rows refill each time the menu opens, ahead of any other
 * rows it holds. stackRow ends them with a Modifier Stack... row; without it they sit under an Add section, for a menu with
 * sections of its own.
 */
LILA_UI_API void fillAddModifierMenu(QMenu* menu, bool stackRow = true);

/** Tabler icon name for a modifier kind ("bevel") or node type; extensions may set their own. */
LILA_UI_API void setModifierIcon(const std::string& kindOrType, const QString& tablerName);
LILA_UI_API QString modifierIconName(const std::string& kindOrType);

/** Shows and raises the Modifier Stack dock. */
LILA_UI_API void showModifierStack();

/** Registers "window.modifierStack" and "modifier.applyAll". */
LILA_UI_API void registerModifierStackActions();
} // namespace lila::ui
