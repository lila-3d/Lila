#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include "scene/Plug.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class QLabel;
class QLineEdit;
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
} // namespace attributeEditor

/** Every attribute of the lead selected node, grouped by the type that declares it. Edits go through setAttr. */
class LILA_UI_API AttributeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit AttributeEditor(QWidget* parent = nullptr);
    ~AttributeEditor() override;

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

    QLineEdit* mName = nullptr;
    QLabel* mType = nullptr;
    QWidget* mBody = nullptr;
    QVBoxLayout* mBodyLayout = nullptr;
    scene::NodeId mNode = scene::kNoNode;
    std::vector<std::function<void()>> mUpdaters;
    int mListener = 0;
    int mSectionListener = 0;
    bool mQueued = false;
    bool mRebuild = false;
};
} // namespace lila::ui
