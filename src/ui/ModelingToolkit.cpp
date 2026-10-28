#include "ui/ModelingToolkit.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/DockSidebar.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/ModifierStack.h"
#include "ui/OptionBox.h"
#include "ui/SoftSelectPanel.h"
#include "ui/SpinScrub.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
constexpr const char* kCollapsedPref = "/modelingToolkit/collapsed";

/** Collapsible section header, Maya frameLayout style; the collapsed state is remembered per section. */
QWidget* section(const QString& title, QWidget* content, QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    auto* header = new QToolButton(box);
    header->setText(title);
    header->setCheckable(true);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header->setStyleSheet("QToolButton { text-align: left; font-weight: 600; padding: 3px; }");
    std::string key = std::string(kCollapsedPref) + "/" + title.toStdString();
    bool open = !PrefStore::get().value<bool>("general", key, false);
    header->setChecked(open);
    header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    content->setVisible(open);
    QObject::connect(header, &QToolButton::toggled, content, [header, content, key](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        PrefStore::get().set("general", key, !on);
    });
    v->addWidget(header);
    v->addWidget(content, 1);
    v->addStretch(0);
    return box;
}

QWidget* flowBox(QWidget* parent, FlowLayout** layout)
{
    auto* w = new QWidget(parent);
    *layout = new FlowLayout(3);
    (*layout)->setContentsMargins(2, 0, 2, 2);
    w->setLayout(*layout);
    return w;
}

QDockWidget* findDock(const char* name)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = w->findChild<QDockWidget*>(name))
            return d;
    return nullptr;
}

scene::Node* leadMesh()
{
    scene::Graph& g = scene::Graph::current();
    for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().isA("mesh"))
            return n;
    return nullptr;
}

/** The button text: shortText when its words all appear, in order, in the action's own label, otherwise that label. */
QString buttonText(const std::string& label, const QString& shortText)
{
    QString full = QString::fromStdString(label);
    QStringList words = full.split(' ', Qt::SkipEmptyParts);
    qsizetype at = 0;
    for (const QString& w : shortText.split(' ', Qt::SkipEmptyParts))
    {
        while (at < words.size() && words[at].compare(w, Qt::CaseInsensitive) != 0)
            ++at;
        if (at++ >= words.size())
            return full;
    }
    return shortText.isEmpty() ? full : shortText;
}

void runCommand(const char* name, const json& call)
{
    try
    {
        ops::execute(name, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}
} // namespace

ModelingToolkit::ModelingToolkit(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto* body = new QWidget(scroll);
    auto* root = new QVBoxLayout(body);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);
    scroll->setWidget(body);

    auto row = [&](QWidget* parent, std::initializer_list<std::tuple<const char*, const char*, const char*>> items, FlowLayout* into) {
        for (auto [id, ic, text] : items)
            if (QToolButton* b = actionButton(id, ic, text, parent))
                into->addWidget(optionBox::withOptionBox(b, id, parent));
    };

    // Header: component modes.
    FlowLayout* modes = nullptr;
    QWidget* modesBox = flowBox(body, &modes);
    mModes = new QButtonGroup(this);
    mModes->setExclusive(true);
    int modeIndex = 0;
    for (auto [id, ic, text] : {std::tuple{"select.objectMode", "cube", "Object"}, {"select.vertexMode", "point", "Vertex"},
                                {"select.edgeMode", "line", "Edge"}, {"select.faceMode", "square", "Face"}})
    {
        if (QToolButton* b = actionButton(id, ic, text, modesBox))
        {
            b->setCheckable(true);
            mModes->addButton(b, modeIndex);
            modes->addWidget(b);
        }
        ++modeIndex;
    }
    row(modesBox, {{"window.uvEditor", "texture", "UV"}}, modes);
    root->addWidget(modesBox);

    // Selection options: soft select, symmetry, camera-based picking.
    FlowLayout* opts = nullptr;
    QWidget* optsBox = flowBox(body, &opts);
    mSoft = actionButton("softSelect.toggle", "ripple", "Soft", optsBox);
    if (mSoft)
    {
        mSoft->setCheckable(true);
        opts->addWidget(mSoft);
    }
    mSoftRadius = new QDoubleSpinBox(optsBox);
    mSoftRadius->setRange(0.01, 100000.0);
    mSoftRadius->setDecimals(2);
    mSoftRadius->setSuffix(" cm");
    mSoftRadius->setKeyboardTracking(false);
    setSpinDefault(mSoftRadius, 25.0);
    mSoftRadius->setToolTip("Soft selection falloff radius, world space (hold B and drag in the viewport to set it)");
    connect(mSoftRadius, &QDoubleSpinBox::valueChanged, this,
            [](double r) { PrefStore::get().set("general", "/softSelect/radius", r); });
    opts->addWidget(mSoftRadius);
    auto* symLabel = new QLabel("Symmetry", optsBox);
    opts->addWidget(symLabel);
    mSymmetry = new QComboBox(optsBox);
    mSymmetry->addItems({"Off", "X", "Y", "Z", "Radial"});
    mSymmetry->setToolTip("Object-space mirror plane, or radial symmetry, for component edits on the selected meshes (setSymmetry)");
    connect(mSymmetry, &QComboBox::activated, this, [](int i) {
        static const char* ids[] = {"mesh.symmetryOff", "mesh.symmetryX", "mesh.symmetryY", "mesh.symmetryZ", "mesh.symmetryRadial"};
        actions::run(json(ids[std::clamp(i, 0, 4)]));
    });
    opts->addWidget(mSymmetry);
    mCameraBased = actionButton("select.toggleCameraBased", "camera", "Camera-Based", optsBox);
    if (mCameraBased)
    {
        mCameraBased->setCheckable(true);
        opts->addWidget(mCameraBased);
    }
    mBackface = actionButton("select.toggleBackfaceCulling", "eye-off", "Backfaces", optsBox);
    if (mBackface)
    {
        mBackface->setCheckable(true);
        opts->addWidget(mBackface);
    }
    mPaintRadius = new QSpinBox(optsBox);
    mPaintRadius->setRange(2, 1000);
    mPaintRadius->setSuffix(" px");
    mPaintRadius->setKeyboardTracking(false);
    mPaintRadius->setToolTip("Paint Select brush radius on screen (B + drag or [ ] while painting)");
    connect(mPaintRadius, &QSpinBox::valueChanged, this, [](int r) { PrefStore::get().set("general", "/paintSelect/radius", double(r)); });
    opts->addWidget(mPaintRadius);
    row(optsBox, {{"select.byAngle", "angle", "By Angle"}}, opts);
    root->addWidget(optsBox);
    root->addWidget(section("Soft Selection", new SoftSelectPanel(body), body));

    // Selection helpers.
    FlowLayout* sel = nullptr;
    QWidget* selBox = flowBox(body, &sel);
    row(selBox,
        {{"select.grow", "arrows-maximize", "Grow"}, {"select.shrink", "arrows-minimize", "Shrink"}, {"select.loop", "repeat", "Loop"},
         {"select.ring", "topology-ring", "Ring"}, {"select.border", "border-outer", "Border"}, {"select.allComponents", "border-all", "All"},
         {"select.toVertices", "point", "To Vertices"}, {"select.toEdges", "line", "To Edges"}, {"select.toFaces", "square", "To Faces"},
         {"select.containedFaces", "square-dot", "Contained"}, {"select.paintTool", "brush", "Paint"}},
        sel);
    root->addWidget(section("Selection", selBox, body));

    FlowLayout* mesh = nullptr;
    QWidget* meshBox = flowBox(body, &mesh);
    row(meshBox,
        {{"mesh.combine", "layers-linked", "Combine"}, {"mesh.separate", "arrows-split", "Separate"}, {"mesh.extract", "box-padding", "Extract"},
         {"mesh.booleanUnion", "layers-union", "Union"}, {"mesh.booleanDifference", "layers-difference", "Difference"},
         {"mesh.booleanIntersection", "layers-intersect", "Intersection"}, {"mesh.subdivide", "sphere", "Subdivide"},
         {"mesh.mirror", "flip-horizontal", "Mirror"}, {"mesh.triangulate", "triangle", "Triangulate"},
         {"mesh.quadrangulate", "square-toggle", "Quadrangulate"}, {"mesh.fillHole", "square-plus", "Fill Hole"},
         {"mesh.repair", "wand", "Repair"}, {"modifier.decimate", "grain", "Decimate"}, {"mesh.relax", "wave-sine", "Relax"},
         {"mesh.quadRemesh", "grid-4x4", "Remesh"}, {"mesh.flip", "arrows-exchange-2", "Flip"}, {"mesh.conform", "refresh", "Conform"},
         {"mesh.transferAttributes", "arrows-exchange", "Transfer"}, {"mesh.bakeModifierStack", "stack-pop", "Bake Stack"}},
        mesh);
    root->addWidget(section("Mesh", meshBox, body));

    FlowLayout* comps = nullptr;
    QWidget* compsBox = flowBox(body, &comps);
    row(compsBox,
        {{"mesh.extrude", "cube-plus", "Extrude"}, {"mesh.bevel", "vector-bezier", "Bevel"}, {"mesh.bridge", "arrows-join", "Bridge"},
         {"mesh.inset", "box-model-2", "Inset"}, {"mesh.poke", "topology-star", "Poke"}, {"mesh.merge", "arrow-merge", "Merge"},
         {"mesh.mergeCenter", "focus-centered", "To Center"}, {"mesh.collapse", "arrows-diagonal-minimize", "Collapse"},
         {"mesh.connect", "line-dashed", "Connect"}, {"mesh.dissolve", "eraser", "Dissolve"}, {"mesh.spin", "rotate-clockwise", "Spin"},
         {"edit.delete", "trash", "Delete"}},
        comps);
    root->addWidget(section("Components", compsBox, body));

    FlowLayout* tools = nullptr;
    QWidget* toolsBox = flowBox(body, &tools);
    row(toolsBox, {{"mesh.insertLoop", "separator-vertical", "Insert Loop"}, {"mesh.splitRings", "columns-3", "Split Rings"}, {"mesh.offsetLoop", "columns-2", "Offset Loop"}, {"mesh.knife", "slice", "Knife"}, {"mesh.targetWeld", "target-arrow", "Target Weld"},
                   {"retopo.draw", "pencil", "Retopo Draw"}}, tools);
    root->addWidget(section("Tools", toolsBox, body));

    FlowLayout* curves = nullptr;
    QWidget* curvesBox = flowBox(body, &curves);
    row(curvesBox, {{"window.curveToolkit", "tools", "Toolkit"}, {"curve.ezCurve", "line", "EZ"}, {"curve.bezierPen", "vector-bezier", "Pen"}, {"curve.epTool", "vector-spline", "EP"}, {"curve.cvTool", "vector-bezier-2", "CV"},
                    {"curve.pencil", "scribble", "Pencil"}, {"curve.fromEdges", "polygon", "Edges"}, {"curve.extrude", "route", "Sweep"},
                    {"curve.revolve", "rotate-360", "Revolve"}, {"curve.loft", "stack-3", "Loft"}, {"curve.pipeTool", "route-2", "Pipe"},
                    {"curve.fillet", "vector-bezier-arc", "Fillet"}, {"curve.offset", "ripple", "Offset"}, {"curve.text", "typography", "Text"},
                    {"curve.textMesh", "badge-3d", "Text Mesh"}, {"curve.bevelPlus", "text-size", "Bevel+"}},
        curves);
    root->addWidget(section("Curves", curvesBox, body));

    FlowLayout* shape = nullptr;
    QWidget* shapeBox = flowBox(body, &shape);
    row(shapeBox,
        {{"mesh.editEdgeFlow", "ease-in-out", "Edge Flow"}, {"mesh.straighten", "line", "Straighten"}, {"mesh.circularize", "circle", "Circularize"}, {"mesh.spherize", "sphere", "Spherize"},
         {"mesh.flatten", "layers-subtract", "Flatten"}, {"mesh.conformShape", "shape", "To Shape"}, {"mesh.conformToObject", "magnet", "To Object"}},
        shape);
    root->addWidget(section("Shape", shapeBox, body));

    FlowLayout* bevel = nullptr;
    QWidget* bevelBox = flowBox(body, &bevel);
    row(bevelBox,
        {{"modifier.autoBevel", "border-corner-rounded", "Auto Bevel"}, {"select.foldEdges", "chart-arcs", "Fold Edges"},
         {"bevel.editEdges", "edit", "Edit Edges"}, {"bevel.applyEdges", "check", "Apply"}, {"bevel.cancelEdges", "x", "Cancel"},
         {"bevel.measureThin", "ruler-measure", "Measure Thin"}, {"bevel.clearOverrides", "eraser", "Clear Overrides"},
         {"bevel.showEdges", "eye", "Show Edges"}},
        bevel);
    root->addWidget(section("Auto Bevel", bevelBox, body));

    auto* optionsBox = new QWidget(body);
    auto* optionsLayout = new QVBoxLayout(optionsBox);
    optionsLayout->setContentsMargins(4, 0, 4, 4);
    optionsLayout->addWidget(new LastOpEditor(optionsBox));
    root->addWidget(section("Tool Options", optionsBox, body));

    // The full stack panel, so each modifier's parameters are edited right here.
    auto* modsBox = new QWidget(body);
    auto* modsLayout = new QVBoxLayout(modsBox);
    modsLayout->setContentsMargins(2, 0, 2, 2);
    auto* stack = new ModifierStackWidget(modsBox);
    stack->setMinimumHeight(420);
    modsLayout->addWidget(stack);
    root->addWidget(section("Modifiers", modsBox, body), 1);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(30);
    connect(mRefresh, &QTimer::timeout, this, [this] {
        refreshState();
    });
    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) { mRefresh->start(); });
    mComponentListener = scene::components::addListener([this] { mRefresh->start(); });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
    refreshState();
}

ModelingToolkit::~ModelingToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* ModelingToolkit::actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent)
{
    const Action* action = actions::find(id);
    if (!action)
        return nullptr;
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setText(buttonText(action->label, text));
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setProperty("lilaAction", QString::fromStdString(id));
    // Checkable buttons mirror state that refreshState owns, so the click only runs the action.
    connect(b, &QToolButton::clicked, this, [this, id] {
        if ((QApplication::keyboardModifiers() & Qt::ShiftModifier) && optionBox::available(id))
            optionBox::open(id);
        else
            actions::run(json(id));
        mRefresh->start();
    });
    mButtons.push_back(b);
    return b;
}

void ModelingToolkit::refreshTooltips()
{
    for (QToolButton* b : mButtons)
    {
        std::string id = b->property("lilaAction").toString().toStdString();
        if (actions::find(id))
            b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    }
}

void ModelingToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // Hotkeys are live-editable, so tooltips pick up the current keys whenever the panel shows.
    refreshTooltips();
    refreshState();
}

void ModelingToolkit::refreshState()
{
    int mode = int(scene::components::mode());
    if (QAbstractButton* b = mModes->button(mode); b && !b->isChecked())
        b->setChecked(true);
    PrefStore& p = PrefStore::get();
    if (mSoft)
        mSoft->setChecked(p.value<bool>("general", "/softSelect/enabled", false));
    if (!mSoftRadius->hasFocus())
    {
        QSignalBlocker block(mSoftRadius);
        mSoftRadius->setValue(p.value<double>("general", "/softSelect/radius", 25.0));
    }
    if (mCameraBased)
        mCameraBased->setChecked(p.value<bool>("general", "/select/cameraBased", false));
    if (mBackface)
        mBackface->setChecked(p.value<bool>("general", "/select/backfaceCulling", false));
    if (!mPaintRadius->hasFocus())
    {
        QSignalBlocker block(mPaintRadius);
        mPaintRadius->setValue(int(std::lround(p.value<double>("general", "/paintSelect/radius", 30.0))));
    }
    scene::Node* lead = leadMesh();
    mSymmetry->setEnabled(lead != nullptr);
    if (lead)
    {
        QSignalBlocker block(mSymmetry);
        mSymmetry->setCurrentIndex(int(std::clamp<int64_t>(lead->as<int64_t>(scene::MeshNode::kSymmetry), 0, scene::kSymmetryRadial)));
    }
}

void registerModelingToolkitActions()
{
    actions::add({.id = "window.modelingToolkit", .label = "Modeling Toolkit", .category = "Windows", .run = [] {
                      toggleDock(findDock(kModelingToolkitDock));
                  },
                  .doc = "Component modes, selection options, modeling commands, live tool options and the modifier stack in one panel.",
                  .python = "lila.ui.run_action(\"window.modelingToolkit\")"});
}
} // namespace lila::ui
