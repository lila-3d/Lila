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
#include "ui/FixedRowLayout.h"
#include "ui/Icons.h"
#include "ui/ModifierStack.h"
#include "ui/OptionBox.h"
#include "ui/SoftSelectPanel.h"
#include "ui/SpinScrub.h"
#include "ui/ToolkitLayout.h"
#include "ui/UiCommon.h"

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
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
constexpr const char* kModelingCollapsed = "/modelingToolkit/collapsed";

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

    auto row = [&](QWidget* parent, std::initializer_list<std::tuple<const char*, const char*, const char*>> items, FixedRowLayout* into) {
        for (auto [id, ic, text] : items)
            if (QToolButton* b = actionButton(id, ic, text, parent))
                into->addWidget(optionBox::withOptionBox(b, id, parent));
        into->endRow();
    };

    // Header: component modes.
    FixedRowLayout* modes = nullptr;
    QWidget* modesBox = rowBox(body, &modes);
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
    FixedRowLayout* opts = nullptr;
    QWidget* optsBox = rowBox(body, &opts);
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
    opts->endRow();
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
    root->addWidget(collapsibleSection("Soft Selection", new SoftSelectPanel(body), body, kModelingCollapsed));

    // Selection helpers.
    FixedRowLayout* sel = nullptr;
    QWidget* selBox = rowBox(body, &sel, kToolkitColumns);
    row(selBox, {{"select.grow", "arrows-maximize", "Grow"}, {"select.shrink", "arrows-minimize", "Shrink"}, {"select.loop", "repeat", "Loop"}, {"select.ring", "topology-ring", "Ring"}}, sel);
    row(selBox, {{"select.border", "border-outer", "Border"}, {"select.allComponents", "border-all", "All"}, {"select.containedFaces", "square-dot", "Contained"}, {"select.paintTool", "brush", "Paint"}}, sel);
    row(selBox, {{"select.toVertices", "point", "To Vertices"}, {"select.toEdges", "line", "To Edges"}, {"select.toFaces", "square", "To Faces"}}, sel);
    root->addWidget(collapsibleSection("Selection", selBox, body, kModelingCollapsed));

    FixedRowLayout* mesh = nullptr;
    QWidget* meshBox = rowBox(body, &mesh, kToolkitColumns);
    row(meshBox, {{"mesh.combine", "layers-linked", "Combine"}, {"mesh.separate", "arrows-split", "Separate"}, {"mesh.extract", "box-padding", "Extract"}, {"mesh.transferAttributes", "arrows-exchange", "Transfer"}}, mesh);
    row(meshBox, {{"mesh.booleanUnion", "layers-union", "Union"}, {"mesh.booleanDifference", "layers-difference", "Difference"}, {"mesh.booleanIntersection", "layers-intersect", "Intersection"}, {"mesh.mirror", "flip-horizontal", "Mirror"}}, mesh);
    row(meshBox, {{"mesh.subdivide", "sphere", "Subdivide"}, {"mesh.triangulate", "triangle", "Triangulate"}, {"mesh.quadrangulate", "square-toggle", "Quadrangulate"}, {"mesh.quadRemesh", "grid-4x4", "Remesh"}}, mesh);
    row(meshBox, {{"mesh.fillHole", "square-plus", "Fill Hole"}, {"mesh.gridFill", "grid-pattern", "Grid Fill"}, {"mesh.repair", "wand", "Repair"}, {"modifier.decimate", "grain", "Decimate"}}, mesh);
    row(meshBox, {{"mesh.relax", "wave-sine", "Relax"}, {"mesh.flip", "arrows-exchange-2", "Flip"}, {"mesh.conform", "refresh", "Conform"}, {"mesh.bakeModifierStack", "stack-pop", "Bake Stack"}}, mesh);
    root->addWidget(collapsibleSection("Mesh", meshBox, body, kModelingCollapsed));

    FixedRowLayout* comps = nullptr;
    QWidget* compsBox = rowBox(body, &comps, kToolkitColumns);
    row(compsBox, {{"mesh.extrude", "cube-plus", "Extrude"}, {"mesh.bevel", "vector-bezier", "Bevel"}, {"mesh.bridge", "arrows-join", "Bridge"}, {"mesh.inset", "box-model-2", "Inset"}}, comps);
    row(compsBox, {{"mesh.poke", "topology-star", "Poke"}, {"mesh.connect", "line-dashed", "Connect"}, {"mesh.spin", "rotate-clockwise", "Spin"}, {"mesh.dissolve", "eraser", "Dissolve"}}, comps);
    row(compsBox, {{"mesh.merge", "arrow-merge", "Merge"}, {"mesh.mergeCenter", "focus-centered", "To Center"}, {"mesh.collapse", "arrows-diagonal-minimize", "Collapse"}, {"edit.delete", "trash", "Delete"}}, comps);
    root->addWidget(collapsibleSection("Components", compsBox, body, kModelingCollapsed));

    FixedRowLayout* tools = nullptr;
    QWidget* toolsBox = rowBox(body, &tools, kToolkitColumns);
    row(toolsBox, {{"mesh.insertLoop", "separator-vertical", "Insert Loop"}, {"mesh.splitRings", "columns-3", "Split Rings"}, {"mesh.offsetLoop", "columns-2", "Offset Loop"}, {"mesh.knife", "slice", "Knife"}}, tools);
    row(toolsBox, {{"mesh.targetWeld", "target-arrow", "Target Weld"}, {"retopo.draw", "pencil", "Retopo Draw"}}, tools);
    root->addWidget(collapsibleSection("Tools", toolsBox, body, kModelingCollapsed));

    FixedRowLayout* curves = nullptr;
    QWidget* curvesBox = rowBox(body, &curves, kToolkitColumns);
    row(curvesBox, {{"window.curveToolkit", "tools", "Toolkit"}, {"curve.ezCurve", "line", "EZ"}, {"curve.bezierPen", "vector-bezier", "Pen"}, {"curve.epTool", "vector-spline", "EP"}}, curves);
    row(curvesBox, {{"curve.cvTool", "vector-bezier-2", "CV"}, {"curve.kappa", "ease-in-out", "Kappa"}, {"curve.pencil", "scribble", "Pencil"}, {"curve.fromEdges", "polygon", "Edges"}}, curves);
    row(curvesBox, {{"curve.fillet", "vector-bezier-arc", "Fillet"}, {"curve.offset", "ripple", "Offset"}}, curves);
    row(curvesBox, {{"curve.extrude", "route", "Sweep"}, {"curve.revolve", "rotate-360", "Revolve"}, {"curve.loft", "stack-3", "Loft"}, {"curve.pipeTool", "route-2", "Pipe"}}, curves);
    row(curvesBox, {{"curve.pipeRouteTool", "route-alt-left", "Route"}, {"curve.pipeSupports", "building-bridge", "Supports"}, {"curve.carveChannel", "shovel", "Channel"}}, curves);
    row(curvesBox, {{"curve.text", "typography", "Text"}, {"curve.textMesh", "badge-3d", "Text Mesh"}, {"curve.bevelPlus", "text-size", "Bevel+"}}, curves);
    root->addWidget(collapsibleSection("Curves", curvesBox, body, kModelingCollapsed));

    FixedRowLayout* shape = nullptr;
    QWidget* shapeBox = rowBox(body, &shape, kToolkitColumns);
    row(shapeBox, {{"mesh.editEdgeFlow", "ease-in-out", "Edge Flow"}, {"mesh.straighten", "line", "Straighten"}, {"mesh.circularize", "circle", "Circularize"}, {"mesh.spherize", "sphere", "Spherize"}}, shape);
    row(shapeBox, {{"mesh.flatten", "layers-subtract", "Flatten"}, {"mesh.conformShape", "shape", "To Shape"}, {"mesh.conformToObject", "magnet", "To Object"}}, shape);
    root->addWidget(collapsibleSection("Shape", shapeBox, body, kModelingCollapsed));

    FixedRowLayout* bevel = nullptr;
    QWidget* bevelBox = rowBox(body, &bevel, kToolkitColumns);
    row(bevelBox, {{"modifier.autoBevel", "border-corner-rounded", "Auto Bevel"}, {"select.foldEdges", "chart-arcs", "Fold Edges"}, {"bevel.measureThin", "ruler-measure", "Measure Thin"}, {"bevel.showEdges", "eye", "Show Edges"}}, bevel);
    row(bevelBox, {{"bevel.editEdges", "edit", "Edit Edges"}, {"bevel.applyEdges", "check", "Apply"}, {"bevel.cancelEdges", "x", "Cancel"}, {"bevel.clearOverrides", "eraser", "Clear Overrides"}}, bevel);
    root->addWidget(collapsibleSection("Auto Bevel", bevelBox, body, kModelingCollapsed));

    auto* optionsBox = new QWidget(body);
    auto* optionsLayout = new QVBoxLayout(optionsBox);
    optionsLayout->setContentsMargins(4, 0, 4, 4);
    optionsLayout->addWidget(new LastOpEditor(optionsBox));
    root->addWidget(collapsibleSection("Tool Options", optionsBox, body, kModelingCollapsed));

    // The full stack panel, so each modifier's parameters are edited right here.
    auto* modsBox = new QWidget(body);
    auto* modsLayout = new QVBoxLayout(modsBox);
    modsLayout->setContentsMargins(2, 0, 2, 2);
    auto* stack = new ModifierStackWidget(modsBox);
    stack->setMinimumHeight(420);
    modsLayout->addWidget(stack);
    root->addWidget(collapsibleSection("Modifiers", modsBox, body, kModelingCollapsed), 1);
    // The panel stops at its narrowest fixed row instead of clipping it.
    scroll->setMinimumWidth(body->minimumSizeHint().width() + scroll->verticalScrollBar()->sizeHint().width());

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
