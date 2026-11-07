#include "ui/ClayUi.h"

#include "clay/ClayNodes.h"
#include "core/Log.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Shelf.h"
#include "ui/SpinScrub.h"
#include "ui/ValueReset.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::NodeId;

void clayAction(std::string id, std::string label, std::string command, json call, std::string icon, std::string doc)
{
    std::string py = ops::toPython(command, call);
    actions::add({.id = std::move(id), .label = std::move(label), .category = "Mesh", .doc = std::move(doc), .python = py,
                  .command = std::move(command), .call = std::move(call), .icon = std::move(icon)});
}

void runClay(const char* name, json flags)
{
    try
    {
        ops::execute(name, flags, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

/** The clay object's surface settings and commands, so they sit with the object rather than on its hidden surface node. */
QWidget* clayPanel(NodeId id)
{
    Graph& g = Graph::current();
    const scene::Node* object = g.node(id);
    const scene::Node* surface = object ? clay::surfaceOf(*object) : nullptr;
    if (!surface)
        return nullptr;
    g.evaluate();
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);

    const std::string objectPath = object->path(), surfacePath = surface->path();
    auto at = [&](const char* attr) { return surface->type().attrIndex(attr); };
    const std::string warning = surface->as<std::string>(at("warning"));
    size_t quads = 0;
    if (const scene::MeshPtr& m = surface->as<scene::MeshPtr>(at("outMesh")))
        quads = m->faceCapacity();
    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(6);
    if (!warning.empty())
    {
        // The triangle carries the warning with colour off; the hue only speeds reading it.
        auto* mark = new QLabel;
        mark->setPixmap(icon("alert-triangle").pixmap(16, 16));
        mark->setAlignment(Qt::AlignTop);
        statusRow->addWidget(mark);
    }
    auto* status = new QLabel(warning.empty() ? QString("%1 shapes, %2 quads").arg(surface->elements(at("items")).size()).arg(quads)
                                              : QString::fromStdString(warning));
    status->setWordWrap(true);
    if (!warning.empty())
    {
        status->setStyleSheet(QString("color: %1;").arg(themeColor("issueWarning").name()));
        status->setProperty("lilaRawColors", true);
    }
    statusRow->addWidget(status, 1);
    v->addLayout(statusRow);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto setAttr = [surfacePath](const char* attr, json value) { runClay("setAttr", {{"args", {surfacePath + "." + attr, value}}}); };
    auto attrDefault = [&](const char* attr) { return valueReset::plugDefault(surface->type().attrs[size_t(at(attr))]).value_or(json()); };
    auto spin = [&](const char* attr, const char* label, const char* tip) {
        auto* s = new controls::SliderField;
        s->setRange(0.01, 5.0);
        s->setSingleStep(0.05);
        s->setDecimals(2);
        s->setSuffix(" cm");
        s->setValue(surface->as<double>(at(attr)));
        if (const json d = attrDefault(attr); d.is_number())
            setSpinDefault(s, d.get<double>());
        s->setToolTip(QString("%1: %2").arg(label, tip));
        QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [setAttr, attr](double value) { setAttr(attr, value); });
        form->addRow(label, s);
    };
    spin("voxelSize", "Voxel Size", "Surface detail while editing. Smaller is finer and slower.");
    spin("finalVoxelSize", "Final Voxel Size", "Surface detail Convert Clay to Mesh uses.");
    auto check = [&](const char* label, bool on, bool def, const char* tip, std::function<void(bool)> set) {
        auto* c = new QCheckBox(label);
        c->setChecked(on);
        setCheckDefault(c, def);
        c->setToolTip(tip);
        QObject::connect(c, &QCheckBox::toggled, c, std::move(set));
        form->addRow("", c);
    };
    check("Mirror X", surface->as<int64_t>(at("symmetry")) == 1, attrDefault("symmetry") == 1, "Mirror the clay across the object's YZ plane.",
          [setAttr](bool on) { setAttr("symmetry", on ? "x" : "off"); });
    check("Sharp Edges", surface->as<bool>(at("sharpEdges")), attrDefault("sharpEdges") == true, "Keep hard joins and box edges sharp. For hard-surface clay; slower.",
          [setAttr](bool on) { setAttr("sharpEdges", on); });
    check("Colors", surface->as<bool>(at("colors")), attrDefault("colors") == true, "Write each shape's color to the clay color set.", [setAttr](bool on) { setAttr("colors", on); });
    v->addLayout(form);

    std::vector<QWidget*> buttons;
    auto button = [&](const char* text, const char* cmd, json flags, const char* tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(QString("%1: %2").arg(text, tip));
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        controls::setRole(b, "push");
        flags["args"] = {objectPath};
        QObject::connect(b, &QToolButton::clicked, b, [cmd, flags] { runClay(cmd, flags); });
        buttons.push_back(b);
    };
    button("Add Shape", "addClayPrimitive", json::object(), "Add a sphere after the other shapes.");
    button("Add Cut", "addClayPrimitive", {{"operation", "subtract"}}, "Add a sphere that carves the clay before it.");
    button("Refresh", "refreshClay", json::object(), "Rewire the shapes in Outliner order.");
    button("Convert", "convertClayToMesh", json::object(), "Bake to a plain mesh at the final voxel size.");
    v->addWidget(controls::grid(buttons, 2, w));
    return w;
}
} // namespace

void registerClayUi()
{
    clayAction("clay.create", "Create Clay", "createClay", json::object(), "droplet",
               "Creates a clay object: shapes that melt into each other with smooth joins, shown as one mesh. Starts with a sphere.");
    clayAction("clay.addShape", "Add Clay Shape", "addClayPrimitive", json::object(), "circle-plus",
               "Adds a sphere, capsule, box or other shape to the selected clay object, after its other shapes. Pick the shape and join "
               "in the option box.");
    clayAction("clay.addCut", "Add Clay Cut", "addClayPrimitive", {{"operation", "subtract"}}, "circle-minus",
               "Adds a shape that carves into the clay before it.");
    clayAction("clay.group", "Group Clay Shapes", "groupClay", json::object(), "box-multiple",
               "Groups the selected clay shapes, so they join each other first and a cut among them reaches only the group.");
    clayAction("clay.refresh", "Refresh Clay", "refreshClay", json::object(), "refresh",
               "Rewires a clay object after its shapes were reordered or reparented in the Outliner. Shapes join in Outliner order.");
    clayAction("clay.convert", "Convert Clay to Mesh", "convertClayToMesh", json::object(), "transform",
               "Bakes the selected clay object into a plain mesh at its final voxel size, optionally quad remeshed in the option box.");

    registerClayTool();

    json shelf = json::array();
    for (const char* id : {"clay.create", "clay.tool", "clay.addShape", "clay.addCut", "clay.group", "clay.convert"})
        if (const Action* act = actions::find(id))
            shelf.push_back({{"label", act->label}, {"icon", act->icon}, {"target", id}, {"tooltip", act->doc}});
    shelves::add({"Clay", std::move(shelf)});
    attributeEditor::addSection("clay", "Clay Surface", [](NodeId id) { return clayPanel(id); });
}
} // namespace lila::ui
