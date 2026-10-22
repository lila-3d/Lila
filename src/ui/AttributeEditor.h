#pragma once

#include "scene/Plug.h"

#include <QWidget>

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
/** Adds the section to the editor of every node whose type is or derives from typeName. */
void addSection(std::string typeName, std::string title, SectionFactory fn);
/** Builds the added sections that apply to a node, as (title, widget), for other panels that show its tools too. */
std::vector<std::pair<std::string, QWidget*>> buildSections(scene::NodeId id);
} // namespace attributeEditor

/** Every attribute of the lead selected node, grouped by the type that declares it. Edits go through setAttr. */
class AttributeEditor : public QWidget
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

    QLineEdit* mName = nullptr;
    QLabel* mType = nullptr;
    QWidget* mBody = nullptr;
    QVBoxLayout* mBodyLayout = nullptr;
    scene::NodeId mNode = scene::kNoNode;
    std::vector<std::function<void()>> mUpdaters;
    int mListener = 0;
    bool mQueued = false;
    bool mRebuild = false;
};
} // namespace lila::ui
