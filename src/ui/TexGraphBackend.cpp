#include "ui/GraphBackend.h"

#include "core/Log.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
// Plan.h has a member named like Qt's slots macro.
#pragma push_macro("slots")
#undef slots
#include "tex/NodeDef.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")
#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/NodeEditor.h"
#include "ui/Panels.h"
#include "ui/TexParams.h"
#include "ui/TexPreview.h"
#include "ui/TextureGraphUi.h"
#include "ui/Theme.h"
#include "ui/FileDialog.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QApplication>
#include <QBoxLayout>
#include <QFileDialog>
#include <QPointer>
#include <QToolButton>

#include <algorithm>
#include <set>

namespace lila::ui
{
namespace
{

constexpr int kThumbnail = 96;

std::vector<NodeEditor*>& texEditors()
{
    static std::vector<NodeEditor*> list;
    return list;
}

/** Texture graphs: one per texture set, edited through the tex* commands. */
class TexBackend final : public GraphBackend
{
public:
    std::string id() const override { return kTextureGraphBackend; }
    QString title() const override { return "Texture Graph"; }
    std::string commandPrefix() const override { return "tex"; }

    std::string command(std::string_view verb) const override
    {
        if (verb == "GroupInfo")
            return "texGraphInfo";
        if (verb == "ExportGroup")
            return "texExportGraph";
        if (verb == "ImportGroup")
            return "texImportGraph";
        return GraphBackend::command(verb);
    }

    bool supports(std::string_view verb) const override
    {
        static const std::set<std::string_view> kVerbs = {"GroupInfo", "NodeTypes",  "AddNode",    "DeleteNode",     "Connect",
                                                          "Disconnect", "MoveNodes", "SetValue",   "CopyNodes",      "PasteNodes",
                                                          "DuplicateNodes", "ExportGroup", "ImportGroup", "SetTies", "SetFrames", "SetNodeFlags", "SetBookmarks"};
        return kVerbs.contains(verb);
    }

    void adaptCall(std::string_view verb, json& call) const override
    {
        if (verb == "NodeTypes")
            call.erase("group");
    }

    std::vector<std::string> groupNames(scene::Graph& g) const override { return tex::setNames(g); }
    bool hasGroup(scene::Graph& g, const std::string& group) const override { return tex::findSet(g, group) != nullptr; }
    bool isGroupNode(const scene::Node& n) const override { return n.type().name == tex::kTextureSetType; }
    std::string newGroupAction() const override { return "texture.newSet"; }
    std::vector<std::string> fileSuffixes() const override { return {"lilamat", "lilatexnodes", "lilatexnode"}; }

    /** A selected texture set, else the set bound to a material slot of a selected mesh. */
    Owner followSelection(scene::Graph& g) const override
    {
        for (scene::NodeId id : g.selection())
        {
            scene::Node* n = g.node(id);
            if (!n)
                continue;
            if (n->type().name == tex::kTextureSetType)
                return {n->path(), n->name()};
            if (!n->type().isA("mesh"))
                continue;
            scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
            if (!mesh)
                continue;
            const size_t slotCount = std::max<size_t>(mesh->materialNames().size(), 1);
            for (size_t i = 0; i < slotCount; ++i)
                if (std::string set = textureSetForSlot(g, scene::materials::slotName(*mesh, int32_t(i))); !set.empty())
                    if (scene::Node* s = tex::findSet(g, set))
                        return {s->path(), set};
        }
        return {};
    }

    std::string groupOfOwner(scene::Graph& g, const std::string& path) const override
    {
        scene::Node* n = path.empty() ? nullptr : g.find(path);
        return n && n->type().name == tex::kTextureSetType ? n->name() : std::string();
    }

    QString ownerNoun() const override { return "texture set"; }
    QString groupNoun() const override { return "Texture Set"; }

    bool canViewNodes() const override { return true; }

    void viewNode(const ViewRequest& r) const override
    {
        texture2d::Source s{r.group, r.node, {}, {}};
        if (r.nodeType == tex::kMaterialOutputType)
            s = {r.group, {}, {}, "baseColor"};
        else if (r.outputs.empty())
            return;
        texture2d::view(s);
        panels::show(kTexture2dDock);
    }

    SocketStyle socketStyle(const std::string& type) const override
    {
        if (type == "gray")
            return {themeColor("socketFloat"), SocketShape::Circle, SocketMark::Dot};
        if (type == "sdf2d")
            return {themeColor("socketRotation"), SocketShape::Circle, SocketMark::Hole};
        if (type == "material")
            return {themeColor("socketGeometry"), SocketShape::Square, SocketMark::None, 6.0};
        return GraphBackend::socketStyle(type);
    }

    bool valueless(const std::string& type) const override { return type == "material"; }

    double wireWidth(const std::string& type) const override { return type == "material" ? 3.5 : 2.0; }

    std::optional<MergeNode> mergeNode(const std::string& type, const std::string& op) const override
    {
        if (type == "material")
            return MergeNode{"materialBlend", json::object(), "a", "b", "material"};
        // Blend has no divide; the rest map onto its modes.
        if (op == "divide")
            return std::nullopt;
        return MergeNode{"blend", {{"mode", op}}, "background", "foreground", "out"};
    }

    bool canLink(const std::string& from, const std::string& to) const override
    {
        tex::PortType a, b;
        return tex::parsePortType(from, &a) && tex::parsePortType(to, &b) && tex::linkable(a, b);
    }

    QColor headerColor(const std::string& type, const std::string& category) const override
    {
        static const std::pair<std::string_view, const char*> kTokens[] = {
            {"Noise", "nodeTexture"},      {"Pattern", "nodeTexture"},  {"Filter", "nodeColor"},   {"Transform", "nodeGeometry"},
            {"Input", "nodeInput"},        {"Normal", "nodeAttribute"}, {"Material", "nodeMesh"},  {"Output", "nodeOutput"},
            {"Utility", "nodeUtilities"},  {"Graphs", "nodeGroupIo"},
        };
        if (type != tex::kGroupInputType && type != tex::kGroupOutputType)
            for (auto& [name, token] : kTokens)
                if (category == name)
                    return themeColor(token);
        return GraphBackend::headerColor(type, category);
    }

    std::vector<std::string> inputTypes() const override { return {"float", "gray", "color", "vector"}; }

    QString canvasHint() const override
    {
        return GraphBackend::canvasHint() +
               " Ctrl+Shift+click a node to show it in the 2D View and on the meshes this set textures. Drop a .lilamat or "
               ".lilatexnodes file here to import it.";
    }

    int thumbnailSize(const std::string& type) const override
    {
        return type == tex::kMaterialOutputType || type == tex::kGroupInputType || type == tex::kGroupOutputType ? 0 : kThumbnail;
    }

    QImage thumbnail(const std::string& group, const json&, const std::string& node, std::function<void()> ready) const override
    {
        texPreview::Result r = texPreview::get({group, node, {}, {}, 128}, std::move(ready));
        if (!r.image)
            return {};
        return texPreview::toQImage(*r.image, r.type, texPreview::isColor(r.type, node));
    }

    QWidget* nodePanel(QWidget* parent, NodeEditor& editor, const json& node) const override
    {
        if (node.value("type", "") == tex::kGroupInputType)
            return nullptr;
        std::string group = editor.group().toStdString();
        std::string name = node.value("name", "");
        QPointer<NodeEditor> ed(&editor);
        return texParams::nodePanel(parent, node, [ed, group, name](const std::string& id, const json& v) {
            if (ed)
                ed->setValueFromField(argsCall({group, name + "." + id, v}));
        });
    }

    void addToolbarButtons(QBoxLayout* bar, NodeEditor& editor) const override
    {
        auto* view = new QToolButton(&editor);
        view->setText("2D View");
        view->setToolTip("Show the 2D View; Ctrl+Shift+click a node to show its output there");
        QObject::connect(view, &QToolButton::clicked, &editor, [] { panels::show(kTexture2dDock); });
        bar->addWidget(view);
        auto* exportMaps = new QToolButton(&editor);
        exportMaps->setText("Export Maps");
        exportMaps->setToolTip("Write this set's maps through its export preset (texExport)");
        QObject::connect(exportMaps, &QToolButton::clicked, &editor, [&editor] {
            if (!editor.group().isEmpty())
                if (json r = runUi("texExport", argsCall({editor.group().toStdString()})); r.is_object())
                    log::info("exported {} maps", r.value("files", json::array()).size());
        });
        bar->addWidget(exportMaps);
    }
};

NodeEditor* texEditor()
{
    if (NodeEditor* e = NodeEditor::active(); e && e->backend().id() == kTextureGraphBackend)
        return e;
    for (NodeEditor* e : texEditors())
        if (e->isVisible())
            return e;
    return texEditors().empty() ? nullptr : texEditors().front();
}

/** "<slot>Tex" for the selected mesh's first material slot, so the new set drives it. Empty when no mesh is selected. */
std::string selectedSlot()
{
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            if (scene::MeshPtr mesh = n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
                return scene::materials::slotName(*mesh, 0);
    return {};
}

std::string uniqueName(std::string base)
{
    std::string clean;
    for (char c : base)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            clean += c;
    if (clean.empty() || std::isdigit(static_cast<unsigned char>(clean.front())))
        clean = "TextureSet";
    std::string name = clean;
    for (int i = 1; !scene::Graph::current().findAll(name).empty(); ++i)
        name = clean + std::to_string(i);
    return name;
}

void openSet(const std::string& set)
{
    panels::show(kTextureGraphDock);
    for (NodeEditor* e : texEditors())
        e->setGroup(qs(set));
}

void newSet()
{
    std::string slot = selectedSlot();
    json call = argsCall({uniqueName(slot.empty() ? "TextureSet" : slot + "Tex")});
    if (!slot.empty())
        call["settings"] = {{"material", slot}};
    json r = runUi("texCreateSet", call);
    if (r.is_string())
        openSet(r.get<std::string>());
}

QString fileFilter()
{
    return "Texture graphs (*.lilamat *.lilatexnodes *.lilatexnode);;Materials (*.lilamat);;Node libraries (*.lilatexnodes *.lilatexnode)";
}

void importFiles()
{
    QStringList files = ui::fileDialog::getOpenFileNames(QApplication::activeWindow(), "Import Texture Graphs", QString(), fileFilter());
    UndoChunk chunk("Import Texture Graphs");
    std::string last;
    for (const QString& f : files)
        if (json r = runUi("texImportGraph", argsCall({f.toStdString()})); r.is_array() && !r.empty())
            last = r.back().get<std::string>();
    if (!last.empty())
        openSet(last);
}

void exportFile(const char* suffix)
{
    std::string set = currentTextureSet();
    if (set.empty())
    {
        log::warn("No texture set to export");
        return;
    }
    QString path = ui::fileDialog::getSaveFileName(QApplication::activeWindow(), "Export Texture Graph", qs(set) + "." + suffix,
                                                std::string_view(suffix) == "lilamat" ? "Materials (*.lilamat)" : "Node libraries (*.lilatexnodes)");
    if (path.isEmpty())
        return;
    if (json r = runUi("texExportGraph", argsCall({set, path.toStdString()})); r.is_string())
        log::info("exported {}", r.get<std::string>());
}
} // namespace

void registerTexGraphBackend()
{
    if (graphBackends::find(kTextureGraphBackend))
        return;
    graphBackends::add(std::make_shared<TexBackend>());
}

std::string currentTextureSet()
{
    scene::Graph& g = scene::Graph::current();
    if (NodeEditor* e = texEditor(); e && tex::findSet(g, e->group().toStdString()))
        return e->group().toStdString();
    if (std::string s = texture2d::current().set; !s.empty() && tex::findSet(g, s))
        return s;
    std::vector<std::string> names = tex::setNames(g);
    return names.empty() ? std::string() : names.front();
}

void registerTextureGraphUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    registerTexGraphBackend();

    panels::add({.id = kTextureGraphDock,
                 .title = "Texture Graph",
                 .create =
                     [](QWidget* p) -> QWidget* {
                         auto* e = new NodeEditor(p, kTextureGraphBackend);
                         texEditors().push_back(e);
                         QObject::connect(e, &QObject::destroyed, [e] { std::erase(texEditors(), e); });
                         return e;
                     },
                 .area = Qt::BottomDockWidgetArea,
                 .icon = "texture",
                 .open = false,
                 .toggleAction = "window.textureGraph",
                 .docs = "node-editor"});
    panels::add({.id = kTexture2dDock,
                 .title = "2D View",
                 .create = [](QWidget* p) -> QWidget* { return new Texture2DView(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "photo",
                 .open = false,
                 .toggleAction = "window.texture2d"});

    actions::add({.id = "texture.newSet",
                  .label = "New Texture Set",
                  .category = "Texture",
                  .run = [] { newSet(); },
                  .doc = "Make a texture set (a texture graph with a Material Output and the standard channels) and open it in the "
                         "Texture Graph. With a mesh selected, it drives the mesh's first material slot.",
                  .python = "lila.ops.texCreateSet(\"RockTex\", settings={\"material\": \"Rock\"})"});
    actions::add({.id = "texture.import",
                  .label = "Import Texture Graph...",
                  .category = "Texture",
                  .run = [] { importFiles(); },
                  .doc = "Read .lilamat materials as new texture sets, or add a .lilatexnodes node library to your node library.",
                  .python = "lila.ops.texImportGraph(\"rock.lilamat\")"});
    actions::add({.id = "texture.exportMaterial",
                  .label = "Export Material...",
                  .category = "Texture",
                  .run = [] { exportFile("lilamat"); },
                  .doc = "Write the current texture set's graph and settings to a .lilamat file to share or reuse.",
                  .python = "lila.ops.texExportGraph(\"RockTex\", \"rock.lilamat\")"});
    actions::add({.id = "texture.exportNodes",
                  .label = "Export Node Library...",
                  .category = "Texture",
                  .run = [] { exportFile("lilatexnodes"); },
                  .doc = "Write the current texture set's node groups to a .lilatexnodes library, each as a node others can add.",
                  .python = "lila.ops.texExportGraph(\"RockTex\", \"rock.lilatexnodes\")"});
    actions::add({.id = "texture.exportMaps",
                  .label = "Export Maps",
                  .category = "Texture",
                  .run =
                      [] {
                          if (std::string set = currentTextureSet(); !set.empty())
                              if (json r = runUi("texExport", argsCall({set})); r.is_object())
                                  log::info("exported {} maps", r.value("files", json::array()).size());
                      },
                  .doc = "Evaluate the current texture set and write its maps through its export preset (Unreal by default).",
                  .python = "lila.ops.texExport(\"RockTex\")"});
    actions::add({.id = "texture.clearPreview",
                  .label = "Clear Mesh Preview",
                  .category = "Texture",
                  .run = [] { texture2d::clearMeshPreview(); },
                  .doc = "Stop showing the 2D View's output on meshes; they draw their materials again.",
                  .python = "lila.ui.run_action(\"texture.clearPreview\")"});

    menus::addMenu({.id = "Texture", .title = "Texture", .before = "Rig", .sets = {"Modeling"}});
    menus::addActions("Texture", {"texture.newSet", "", "window.textureGraph", "window.texture2d", "", "texture.import", "texture.exportMaterial",
                                  "texture.exportNodes", "texture.exportMaps", "", "texture.clearPreview"});
}
} // namespace lila::ui
