// The Tailor menu, and the garment's Attribute Editor section listing its panels and seams.

#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Actions.h>
#include <lila/AttributeEditor.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileDialog.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Node.h>
#include <lila/ViewportTool.h>

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

using namespace lila;
using namespace lila::ui;

namespace tailor
{
namespace
{
QString sideText(const Garment& g, const std::vector<SeamSide>& sides)
{
    QStringList parts;
    for (const SeamSide& s : sides)
    {
        const bool ok = s.panel >= 0 && size_t(s.panel) < g.panels.size();
        parts << QString("%1:%2:%3").arg(ok ? QString::fromStdString(g.panels[size_t(s.panel)].name) : QString("?")).arg(s.from).arg(s.to);
    }
    return parts.join(", ");
}

// TODO: refresh when the garment attr changes; the SDK has no per-attr change signal for sections, so it shows the
// garment as it was when the editor last rebuilt.
QWidget* garmentSection(scene::NodeId id)
{
    const scene::Node* n = scene::Graph::current().node(id);
    if (!n)
        return nullptr;
    Garment g;
    try
    {
        const std::string& text = n->as<std::string>(n->type().attrIndex("garment"));
        if (!text.empty())
            g = garmentFromJson(nlohmann::json::parse(text));
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, std::string("Tailor: ") + e.what());
    }

    auto* w = new QWidget;
    auto* layout = new QVBoxLayout(w);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* tree = new QTreeWidget;
    tree->setColumnCount(4);
    tree->setHeaderLabels({"Name", "State", "Fabric", "Points"});
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree->setRootIsDecorated(true);

    auto* panels = new QTreeWidgetItem(tree, {QString("Panels (%1)").arg(g.panels.size())});
    for (const Panel& p : g.panels)
    {
        const QString fabric = p.fabric >= 0 && size_t(p.fabric) < g.fabrics.size() ? QString::fromStdString(g.fabrics[size_t(p.fabric)].name) : QString();
        new QTreeWidgetItem(panels, {QString::fromStdString(p.name), panelStateName(p.state), fabric, QString::number(p.outline.size())});
    }
    auto* seams = new QTreeWidgetItem(tree, {QString("Seams (%1)").arg(g.seams.size())});
    for (size_t k = 0; k < g.seams.size(); ++k)
    {
        const Seam& s = g.seams[k];
        auto* item = new QTreeWidgetItem(seams, {QString::number(k), s.reversed ? QString("reversed") : QString()});
        item->setText(0, QString("%1  %2  to  %3").arg(k).arg(sideText(g, s.a), sideText(g, s.b)));
        item->setFirstColumnSpanned(true);
    }
    tree->expandAll();
    tree->setMinimumHeight(160);
    layout->addWidget(tree);
    return w;
}

void runLogged(const char* command, const nlohmann::json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, e.what());
    }
}
} // namespace

void registerUi(ext::Context& ctx)
{
    actions::add(ctx, {.id = "tailor.createBlock", .label = "Garment from Block", .category = "Tailor",
                       .doc = "Start a garment from a parametric block and drape it onto the selected mesh.",
                       .python = "tailorCreateBlock(block='tunic', collider='Mannequin')", .command = "tailorCreateBlock",
                       .call = nlohmann::json::object(), .icon = "shirt"});
    actions::add(ctx, {.id = "tailor.importGarmentCode",
                       .label = "Import GarmentCode Pattern...",
                       .category = "Tailor",
                       .run =
                           [] {
                               const QString path = fileDialog::getOpenFileName(nullptr, "Import GarmentCode Pattern", {}, "Pattern JSON (*.json)");
                               if (!path.isEmpty())
                                   runLogged("tailorImportGarmentCode", {{"file", path.toStdString()}});
                           },
                       .doc = "Import a GarmentCode or ChatGarment sewing pattern and drape it onto the selected mesh.",
                       .python = "tailorImportGarmentCode(file='tee_spec.json', collider='Body')"});
    actions::add(ctx, {.id = "tailor.simulate", .label = "Drape", .category = "Tailor",
                       .doc = "Run the cloth solver on the selected garment until it settles.", .python = "tailorSimulate('Garment', frames=200)",
                       .command = "tailorSimulate", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.setFabric", .label = "Set Fabric", .category = "Tailor",
                       .doc = "Set the selected garment's fabric preset.", .python = "tailorSetFabric('Garment', fabric='Wool')",
                       .command = "tailorSetFabric", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.sew", .label = "Sew", .category = "Tailor",
                       .doc = "Sew two runs of panel outline on the selected garment together.",
                       .python = "tailorSew('Garment', sideA='Front:1:2', sideB='Back:3:4')", .command = "tailorSew",
                       .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.mannequin", .label = "Mannequin", .category = "Tailor",
                       .doc = "Make a stand-in body to drape garments onto; blocks fit whatever body they drape onto.",
                       .python = "tailorMannequin(height=180, chest=96)", .command = "tailorMannequin", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.bake", .label = "Bake to Game Mesh", .category = "Tailor",
                       .doc = "Make a plain mesh from the selected garment's drape, skinned like its collider when that is skinned.",
                       .python = "tailorBake('Garment')", .command = "tailorBake", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.live", .label = "Live Drape", .category = "Tailor",
                       .doc = "Keep the selected garment simulating in real time.", .python = "tailorLive('Garment')",
                       .command = "tailorLive", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.stopLive", .label = "Stop Live Drape", .category = "Tailor",
                       .doc = "Stop the selected garment where it got to.", .python = "tailorLive('Garment', on=False)",
                       .command = "tailorLive", .call = {{"on", false}}});
    actions::add(ctx, {.id = "tailor.drawPanel", .label = "Draw Panel on Body", .category = "Tailor",
                       .run = [] { viewportTools::activate("tailor.drawPanel"); },
                       .doc = "Click an outline on the selected body or garment's body; Enter makes it a panel, M mirrors it.",
                       .icon = "pencil"});
    actions::add(ctx, {.id = "tailor.grab", .label = "Grab Cloth", .category = "Tailor",
                       .run = [] { viewportTools::activate("tailor.grab"); },
                       .doc = "Pull a live garment around in the viewport; the selected garment goes live if none is.",
                       .icon = "hand-grab"});
    actions::add(ctx, {.id = "tailor.freeze", .label = "Freeze Garment", .category = "Tailor",
                       .doc = "Freeze the selected garment's panels: they collide but no longer move.", .python = "tailorFreeze('Garment')",
                       .command = "tailorFreeze", .call = nlohmann::json::object()});
    actions::add(ctx, {.id = "tailor.unfreeze", .label = "Unfreeze Garment", .category = "Tailor",
                       .doc = "Set the selected garment's panels back to active.", .python = "tailorFreeze('Garment', state='active')",
                       .command = "tailorFreeze", .call = {{"state", "active"}}});

    registerPatternEditor(ctx);
    registerGrabTool(ctx);
    registerAnimation(ctx);
    menus::addMenu(ctx, {.id = "Tailor", .title = "Tailor", .before = "Rig", .sets = {"Modeling", "Animation"}});
    int sep = 0;
    for (const char* a : {"tailor.mannequin", "tailor.createBlock", "tailor.drawPanel", "tailor.importGarmentCode", "", "tailor.simulate", "tailor.setFabric",
                          "tailor.sew", "window.tailorPatternEditor", "", "tailor.live", "tailor.stopLive", "tailor.grab", "",
                          "tailor.freeze", "tailor.unfreeze", "", "tailor.bake"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("tailor.menu.") + a, .menu = "Tailor", .action = a});
        else
            menus::addItem(ctx, {.id = "tailor.menu.sep" + std::to_string(sep++), .menu = "Tailor", .kind = menus::Kind::Separator});
    }

    attributeEditor::addSection(ctx, kGarmentType, "Panels and Seams", [](scene::NodeId id) { return garmentSection(id); });
}
} // namespace tailor
