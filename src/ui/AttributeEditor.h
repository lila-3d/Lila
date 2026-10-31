#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include "scene/Node.h"
#include "scene/Plug.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

namespace lila::ui
{
namespace attributeEditor
{
/** Builds an extra section for a node (tools, status); return null to skip it. */
using SectionFactory = std::function<QWidget*(scene::NodeId)>;

/** A section added below a type's attributes (Maya's AETemplate callbacks). Registered under "<typeName>/<title>". */
struct Section
{
    std::string typeName;
    std::string title;
    SectionFactory make;
};
LILA_UI_API Registry<Section>& registry();

/** Adds the section to the editor of every node whose type is or derives from typeName. Open editors rebuild. */
LILA_UI_API void addSection(std::string typeName, std::string title, SectionFactory fn, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t addSection(ext::Context& ctx, std::string typeName, std::string title, SectionFactory fn);
LILA_UI_API bool removeSection(const std::string& typeName, const std::string& title);
/** Builds the added sections that apply to a node, as (title, widget), for other panels that show its tools too. */
LILA_UI_API std::vector<std::pair<std::string, QWidget*>> buildSections(scene::NodeId id);

/**
 * Which of a type's attributes the editor shows for one node, so settings that do not apply to it (a point light's cone)
 * stay out of the way. The editor rebuilds when an attribute named in triggers changes.
 */
struct AttrFilter
{
    std::string typeName;
    std::vector<std::string> triggers;
    std::function<bool(const scene::Node& node, const std::string& attr)> shows;
};
/** Holds "lila.light" from first use. */
LILA_UI_API Registry<AttrFilter>& filters();

/** Scrolls open editors to the issue's attribute or component once they show its object (select it after). */
LILA_UI_API void revealIssue(uint64_t id);
} // namespace attributeEditor

/** Every attribute of the lead selected node, grouped by the type that declares it. Edits go through setAttr. */
class LILA_UI_API AttributeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit AttributeEditor(QWidget* parent = nullptr);
    ~AttributeEditor() override;

    /** Scrolls to the widget showing the issue's attribute or component; false when none does. */
    bool reveal(uint64_t issue);

private:
    void scheduleRefresh();
    void rebuild();
    void updateValues();
    void set(const std::string& attr, const scene::json& value);
    /** Gives each of the attrs (an array, or a grouped array's children) element `element`, holding its default. */
    void addElement(const std::vector<int>& attrs, int element);
    /** Removes the element attr names (attr[3], or group[3].child for the whole group[3]). */
    void removeElement(const std::string& attr);
    QWidget* colorSwatch(const scene::AttrDef& def, scene::NodeId id, int index, bool readOnly);
    /** Narrows each form's label column, down to its labels' elided minimum, before its fields drop below their minimum. */
    void fitLabels();
    bool eventFilter(QObject* watched, QEvent* event) override;
    /** The node's issues above its sections, each one a link to where it points. */
    QWidget* issueList();


    QLineEdit* mName = nullptr;
    QLabel* mType = nullptr;
    QScrollArea* mScroll = nullptr;
    QWidget* mBody = nullptr;
    QVBoxLayout* mBodyLayout = nullptr;
    scene::NodeId mNode = scene::kNoNode;
    std::vector<std::function<void()>> mUpdaters;
    int mListener = 0;
    int mSectionListener = 0;
    int mIssueListener = 0;
    /** The shown node had issues at the last rebuild, so a change to them rebuilds. */
    bool mHadIssues = false;
    /** Rows by attribute name and sections by lowercased component, for reveal. */
    std::map<std::string, QWidget*> mAttrWidgets, mComponentWidgets;
    bool mQueued = false;
    bool mRebuild = false;
    /** Attributes of mNode whose change rebuilds the editor (an AttrFilter's triggers). */
    std::set<int> mTriggers;
};
} // namespace lila::ui
