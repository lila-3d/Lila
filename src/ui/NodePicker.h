#pragma once

#include "core/Export.h"

#include "scene/Plug.h"

#include <nlohmann/json.hpp>

#include <QWidget>

#include <string>
#include <vector>

class QLineEdit;
class QToolButton;
class QAction;
class QCompleter;
class QStandardItemModel;

namespace lila::ui
{
/**
 * A field naming scene nodes of one type, as flags tagged with FlagDef::nodeType take them: type with completion, a
 * dropdown of the scene's matching nodes (visible ones first, hidden ones dimmed), an eyedropper that takes a mesh
 * clicked in the viewport, and a button that loads the selection. Outliner drags drop onto it. A name that doesn't
 * resolve to a node of the type gets a warning icon; a node renamed while it is shown keeps its place.
 *
 * Single: the value is a name, "" for none. Multiple: a list of names, edited as comma separated text; loading the
 * selection replaces it (Shift adds), and the eyedropper adds or removes each mesh clicked until Esc.
 */
class LILA_UI_API NodePicker : public QWidget
{
    Q_OBJECT

public:
    /** type: a node type, several joined with '|' ("mesh|curve"), or "node" for any. */
    NodePicker(std::string type, bool multiple, QWidget* parent = nullptr);
    ~NodePicker() override;

    /** Shows a value (a name, or a list of names) without emitting edited. */
    void setValue(const nlohmann::json& value);
    nlohmann::json value() const;
    /** Grey text while empty, e.g. "Selected mesh" for a flag that falls back to the selection. */
    void setPlaceholder(const QString& text);
    /** What the viewport readout calls the field while the eyedropper is on. */
    void setSubject(const QString& subject) { mSubject = subject; }

    /** Whether the node is of the type(s) this picker takes. */
    bool accepts(scene::NodeId id) const;
    /** Toggles a node's name in a multiple picker, or sets it in a single one, and emits edited. */
    void pick(scene::NodeId id);

    bool multiple() const { return mMultiple; }
    const QString& subject() const { return mSubject; }
    /** Shows the eyedropper button pressed while its viewport tool is active. */
    void setEyedropperOn(bool on);
    /** "mesh", "mesh or curve", "node". */
    QString typeText() const;
    /** "meshes", "meshes or curves", "nodes". */
    QString pluralText() const;

Q_SIGNALS:
    void edited(const nlohmann::json& value);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    std::vector<std::string> names() const;
    void setNames(const std::vector<std::string>& names, bool emit);
    void commit();
    void validate();
    void fillModel();
    void showDropdown();
    void loadSelected(bool add);
    void startEyedropper();
    void dropNames(const QStringList& names);
    void onGraphChange(int change, scene::NodeId id);

    std::vector<std::string> mTypes;
    bool mMultiple = false;
    QString mSubject;
    QLineEdit* mLine = nullptr;
    QToolButton* mDrop = nullptr;
    QToolButton* mDropper = nullptr;
    QToolButton* mLoad = nullptr;
    QAction* mWarning = nullptr;
    QCompleter* mCompleter = nullptr;
    QStandardItemModel* mModel = nullptr;
    /** The nodes the shown names resolved to, so a rename can follow them. */
    std::vector<scene::NodeId> mIds;
    int mListener = -1;
};
} // namespace lila::ui
