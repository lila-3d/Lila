#include "ui/SculptPanel.h"

#include "brush/BrushUi.h"
#include "ops/Command.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DockSidebar.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/ModelingToolkit.h"
#include "ui/SculptTool.h"
#include "ui/SpinScrub.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>

namespace lila::ui
{
namespace
{
struct BrushInfo
{
    sculpt::Brush brush;
    const char* icon;
    const char* label;
    const char* doc;
};

constexpr std::array<BrushInfo, size_t(sculpt::Brush::Count)> kBrushes = {{
    {sculpt::Brush::Draw, "brush", "Draw", "Push the surface out along its normal; Ctrl digs in."},
    {sculpt::Brush::ClayStrips, "layers-subtract", "Clay Strips", "Build up flat square strips of clay; Ctrl carves them."},
    {sculpt::Brush::Inflate, "balloon", "Inflate", "Swell each point along its own normal; Ctrl deflates."},
    {sculpt::Brush::Grab, "hand-grab", "Grab", "Drag a patch of surface with the cursor."},
    {sculpt::Brush::Smooth, "droplet", "Smooth", "Average out bumps and noise. Shift gives this brush for one stroke."},
    {sculpt::Brush::Flatten, "ironing", "Flatten", "Press toward the average plane under the brush; Ctrl raises contrast."},
    {sculpt::Brush::Pinch, "arrows-minimize", "Pinch", "Pull points toward the brush centre to sharpen edges; Ctrl spreads them."},
    {sculpt::Brush::Crease, "chisel", "Crease", "Cut a sharp groove; Ctrl raises a sharp ridge."},
    {sculpt::Brush::Scrape, "shovel", "Scrape", "Shave off what sticks out of the average plane; Ctrl fills dips."},
    {sculpt::Brush::Mask, "mask", "Mask", "Paint the mask, which shields points from every brush; Ctrl erases it."},
    {sculpt::Brush::Relax, "wave-sine", "Relax", "Even out the point spacing, sliding along the surface so the form stays."},
    {sculpt::Brush::Freeze, "snowflake", "Freeze", "Pin points so no brush moves them; Ctrl unfreezes."},
}};

bool runCommand(const char* name, const json& call) { return actions::run({{"command", name}, {"args", call}}); }

QMainWindow* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
            return mw;
    return nullptr;
}

QDockWidget* sculptDock(bool create)
{
    QMainWindow* mw = mainWindow();
    if (!mw)
        return nullptr;
    if (auto* d = mw->findChild<QDockWidget*>(kSculptDock))
        return d;
    if (!create)
        return nullptr;
    auto* dock = new QDockWidget("Sculpt", mw);
    dock->setObjectName(kSculptDock);
    auto* scroll = new QScrollArea(dock);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(new SculptPanel(scroll));
    dock->setWidget(scroll);
    mw->addDockWidget(Qt::RightDockWidgetArea, dock);
    if (auto* toolkit = mw->findChild<QDockWidget*>(kModelingToolkitDock); toolkit && !toolkit->isFloating() && toolkit->isVisible())
        mw->tabifyDockWidget(toolkit, dock);
    return dock;
}

void showDock()
{
    if (QDockWidget* d = sculptDock(true))
    {
        d->show();
        d->raise();
        flashLocate(d);
    }
}

std::vector<std::string> selectedMeshes()
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> out;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n->path());
    return out;
}

void enterSculpt()
{
    const SculptSettings& st = sculptSettings();
    auto meshes = selectedMeshes();
    if (meshes.empty())
    {
        log::warn("Enter Sculpt: select a mesh");
        return;
    }
    if (runCommand("sculptEnter", {{"args", {meshes.back()}}, {"mode", st.proxyMode}, {"levels", st.levels}, {"resolution", st.resolution}}))
    {
        showDock();
        startSculptTool();
    }
}

void exitSculpt()
{
    if (!sculpt::active())
        return;
    const SculptSettings& st = sculptSettings();
    if (runCommand("sculptExit", {{"method", st.exitMethod}, {"keepProxy", st.keepProxy}}) && sculptToolActive())
        viewportTools::set(nullptr);
}

QToolButton* actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    if (actions::find(id))
        b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    QObject::connect(b, &QToolButton::clicked, b, [id] { actions::run(json(id)); });
    return b;
}
} // namespace

SculptPanel::SculptPanel(QWidget* parent) : QWidget(parent)
{
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(6, 6, 6, 6);

    const SculptSettings defaults;

    // Session
    auto* session = new QGroupBox("Sculpt Mode", this);
    auto* sform = new QFormLayout(session);
    mStatus = new QLabel(session);
    mStatus->setWordWrap(true);
    sform->addRow(mStatus);
    mMode = new QComboBox(session);
    mMode->addItem("Subdivide", "subdivide");
    mMode->addItem("Voxel remesh", "remesh");
    mMode->addItem("Direct (no proxy)", "direct");
    mMode->setToolTip("Subdivide: a Catmull-Clark proxy the cage is fitted back to exactly. Voxel remesh: a uniform proxy, fitted "
                      "back by projection. Direct: sculpt the mesh itself.");
    setComboDefault(mMode, mMode->findData(QString::fromStdString(defaults.proxyMode)));
    sform->addRow("Proxy", mMode);
    mLevels = new QSpinBox(session);
    mLevels->setRange(1, 6);
    mLevels->setToolTip("Subdivision levels: each one has four times the faces.");
    setSpinDefault(mLevels, defaults.levels);
    sform->addRow("Levels", mLevels);
    mResolution = new QSpinBox(session);
    mResolution->setRange(16, 2048);
    mResolution->setToolTip("Voxels along the longest side, for the voxel remesh proxy.");
    setSpinDefault(mResolution, defaults.resolution);
    sform->addRow("Resolution", mResolution);
    mExitMethod = new QComboBox(session);
    mExitMethod->addItem("Fit subdivision", "fit");
    mExitMethod->addItem("Closest point", "closest");
    mExitMethod->addItem("Along normal", "normal");
    mExitMethod->setToolTip("How Exit Sculpt moves the cage: fit solves for the cage whose subdivision matches the sculpt; the "
                            "others project each cage point onto it.");
    setComboDefault(mExitMethod, mExitMethod->findData(QString::fromStdString(defaults.exitMethod)));
    sform->addRow("Exit by", mExitMethod);
    mKeepProxy = new QCheckBox("Keep sculpt proxy", session);
    mKeepProxy->setToolTip("Store the sculpt as a hidden child mesh so Enter Sculpt can continue from it.");
    setCheckDefault(mKeepProxy, defaults.keepProxy);
    sform->addRow(mKeepProxy);
    auto* buttons = new QHBoxLayout;
    mEnter = new QPushButton(icon("door-enter"), "Enter", session);
    mExit = new QPushButton(icon("door-exit"), "Exit", session);
    mCancel = new QPushButton("Cancel", session);
    mEnter->setToolTip("Enter Sculpt on the selected mesh.");
    mExit->setToolTip("Exit Sculpt, moving the cage onto the sculpt.");
    mCancel->setToolTip("Leave sculpt mode without changing the mesh.");
    buttons->addWidget(mEnter);
    buttons->addWidget(mExit);
    buttons->addWidget(mCancel);
    sform->addRow(buttons);
    col->addWidget(session);

    // Brushes
    auto* brushBox = new QGroupBox("Brushes", this);
    auto* flow = new FlowLayout(3);
    brushBox->setLayout(flow);
    mBrushes = new QButtonGroup(this);
    mBrushes->setExclusive(true);
    for (const BrushInfo& b : kBrushes)
    {
        auto* btn = new QToolButton(brushBox);
        btn->setIcon(icon(b.icon));
        btn->setText(b.label);
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        btn->setMinimumWidth(66);
        btn->setToolTip(QString("%1\n%2").arg(b.label, b.doc));
        flow->addWidget(btn);
        mBrushes->addButton(btn, int(b.brush));
    }
    col->addWidget(brushBox);

    // Brush settings
    auto* settingsBox = new QGroupBox("Brush", this);
    auto* bform = new QFormLayout(settingsBox);
    mRadius = new QDoubleSpinBox(settingsBox);
    mRadius->setRange(2.0, 2000.0);
    mRadius->setDecimals(0);
    mRadius->setSuffix(" px");
    mRadius->setToolTip("Brush radius on screen. Hold F or B and drag in the viewport to resize.");
    setSpinDefault(mRadius, defaults.stroke.radius);
    bform->addRow("Radius", mRadius);
    mStrength = new QDoubleSpinBox(settingsBox);
    mStrength->setRange(0.0, 1.0);
    mStrength->setSingleStep(0.05);
    mStrength->setDecimals(2);
    mStrength->setToolTip("Hold M (or Shift+F) and drag in the viewport, or hold B and drag with the right button, to change.");
    setSpinDefault(mStrength, defaults.stroke.strength);
    bform->addRow("Strength", mStrength);
    mFalloff = new QComboBox(settingsBox);
    for (const char* f : {"Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant"})
        mFalloff->addItem(f);
    setComboDefault(mFalloff, int(sculpt::falloffFromName(defaults.stroke.falloff).value_or(sculpt::Falloff::Smooth)));
    bform->addRow("Falloff", mFalloff);
    mSpacing = new QDoubleSpinBox(settingsBox);
    mSpacing->setRange(0.02, 2.0);
    mSpacing->setSingleStep(0.05);
    mSpacing->setDecimals(2);
    mSpacing->setToolTip("Distance between dabs along a stroke, as a fraction of the radius.");
    setSpinDefault(mSpacing, defaults.stroke.spacing);
    bform->addRow("Spacing", mSpacing);
    mFrontFaces = new QCheckBox("Front faces only", settingsBox);
    mFrontFaces->setToolTip("Leave surfaces facing away from the view, so strokes don't reach through thin parts.");
    setCheckDefault(mFrontFaces, defaults.frontFacesOnly);
    bform->addRow(mFrontFaces);
    mSymmetry = new QComboBox(settingsBox);
    mSymmetry->addItem("Off", "off");
    mSymmetry->addItem("X", "x");
    mSymmetry->addItem("Y", "y");
    mSymmetry->addItem("Z", "z");
    mSymmetry->addItem("Radial", "radial");
    mSymmetry->setToolTip("The mesh's symmetry setting: strokes mirror exactly across it, or repeat exactly round its radial axis.");
    bform->addRow("Symmetry", mSymmetry);
    mShowCage = new QCheckBox("Show cage", settingsBox);
    mShowCage->setToolTip("Draw the low-poly cage over the sculpt as Exit will leave it, refitted as you sculpt.");
    setCheckDefault(mShowCage, defaults.showCage);
    bform->addRow(mShowCage);
    col->addWidget(settingsBox);
    mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return sculptSettings().stroke; },
                                                  [] { sculptSettingsChanged(); });
    col->addWidget(mStrokeOptions);

    // Mask
    auto* maskBox = new QGroupBox("Mask and Freeze", this);
    auto* mflow = new FlowLayout(3);
    maskBox->setLayout(mflow);
    mflow->addWidget(actionButton("sculpt.maskInvert", "contrast", "Invert", maskBox));
    mflow->addWidget(actionButton("sculpt.maskBlur", "blur", "Blur", maskBox));
    mflow->addWidget(actionButton("sculpt.maskFill", "circle-dot", "Fill", maskBox));
    mflow->addWidget(actionButton("sculpt.maskClear", "eraser", "Clear", maskBox));
    mflow->addWidget(actionButton("sculpt.freezeClear", "snowflake", "Unfreeze All", maskBox));
    col->addWidget(maskBox);

    // Standalone tools
    auto* toolsBox = new QGroupBox("Reprojection", this);
    auto* tflow = new FlowLayout(3);
    toolsBox->setLayout(tflow);
    tflow->addWidget(actionButton("mesh.relaxKeepForm", "wave-sine", "Relax (keep form)", toolsBox));
    tflow->addWidget(actionButton("mesh.reproject", "magnet", "Reproject onto Target", toolsBox));
    tflow->addWidget(actionButton("mesh.transferShape", "transform", "Transfer Shape", toolsBox));
    col->addWidget(toolsBox);
    col->addStretch(1);

    syncFromSettings();
    refreshSession();

    auto changed = [] { sculptSettingsChanged(); };
    connect(mBrushes, &QButtonGroup::idClicked, this, [](int id) {
        sculptSettings().brush = sculpt::Brush(id);
        sculptSettingsChanged();
        if (sculpt::active())
            startSculptTool();
    });
    connect(mRadius, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().stroke.radius = v; changed(); });
    connect(mStrength, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().stroke.strength = v; changed(); });
    connect(mSpacing, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().stroke.spacing = v; changed(); });
    connect(mFalloff, &QComboBox::currentIndexChanged, this, [changed](int i) {
        sculptSettings().stroke.falloff = std::string(sculpt::falloffName(sculpt::Falloff(i)));
        changed();
    });
    connect(mFrontFaces, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().frontFacesOnly = on; changed(); });
    connect(mShowCage, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().showCage = on; changed(); });
    connect(mMode, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        sculptSettings().proxyMode = mMode->currentData().toString().toStdString();
        changed();
    });
    connect(mLevels, &QSpinBox::valueChanged, this, [changed](int v) { sculptSettings().levels = v; changed(); });
    connect(mResolution, &QSpinBox::valueChanged, this, [changed](int v) { sculptSettings().resolution = v; changed(); });
    connect(mExitMethod, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        sculptSettings().exitMethod = mExitMethod->currentData().toString().toStdString();
        changed();
    });
    connect(mKeepProxy, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().keepProxy = on; changed(); });
    connect(mSymmetry, &QComboBox::activated, this, [this](int) {
        std::vector<std::string> targets;
        if (auto s = sculpt::active(); s && scene::Graph::current().node(s->node))
            targets.push_back(scene::Graph::current().node(s->node)->path());
        else
            targets = selectedMeshes();
        if (!targets.empty())
            runCommand("setSymmetry", {{"args", targets}, {"axis", mSymmetry->currentData().toString().toStdString()}});
    });
    connect(mEnter, &QPushButton::clicked, this, [] { actions::run(json("sculpt.enter")); });
    connect(mExit, &QPushButton::clicked, this, [] { actions::run(json("sculpt.exit")); });
    connect(mCancel, &QPushButton::clicked, this, [] { actions::run(json("sculpt.cancel")); });

    mSettingsListener = addSculptSettingsListener([this] { syncFromSettings(); });
    auto queue = [this] {
        if (mRefreshQueued)
            return;
        mRefreshQueued = true;
        QTimer::singleShot(0, this, [this] {
            mRefreshQueued = false;
            refreshSession();
        });
    };
    mSessionListener = sculpt::addListener(queue);
    mGraphListener = scene::Graph::current().addListener([queue](scene::Change c, scene::NodeId) {
        if (c == scene::Change::Value || c == scene::Change::Selection || c == scene::Change::Deleted || c == scene::Change::Cleared ||
            c == scene::Change::Reset)
            queue();
    });
}

SculptPanel::~SculptPanel()
{
    removeSculptSettingsListener(mSettingsListener);
    sculpt::removeListener(mSessionListener);
    scene::Graph::current().removeListener(mGraphListener);
}

void SculptPanel::syncFromSettings()
{
    const SculptSettings& s = sculptSettings();
    QSignalBlocker b1(mRadius), b2(mStrength), b3(mSpacing), b4(mFalloff), b5(mFrontFaces), b6(mShowCage), b7(mMode), b8(mLevels),
        b9(mResolution), b10(mExitMethod), b11(mKeepProxy);
    if (QAbstractButton* b = mBrushes->button(int(s.brush)))
        b->setChecked(true);
    mRadius->setValue(s.stroke.radius);
    mStrength->setValue(s.stroke.strength);
    mSpacing->setValue(s.stroke.spacing);
    mFalloff->setCurrentIndex(int(sculpt::falloffFromName(s.stroke.falloff).value_or(sculpt::Falloff::Smooth)));
    if (mStrokeOptions)
        mStrokeOptions->sync();
    mFrontFaces->setChecked(s.frontFacesOnly);
    mShowCage->setChecked(s.showCage);
    mMode->setCurrentIndex(std::max(0, mMode->findData(QString::fromStdString(s.proxyMode))));
    mLevels->setValue(s.levels);
    mResolution->setValue(s.resolution);
    mExitMethod->setCurrentIndex(std::max(0, mExitMethod->findData(QString::fromStdString(s.exitMethod))));
    mKeepProxy->setChecked(s.keepProxy);
}

void SculptPanel::refreshSession()
{
    scene::Graph& g = scene::Graph::current();
    auto s = sculpt::active();
    scene::Node* n = s ? g.node(s->node) : nullptr;
    if (n)
    {
        const char* modes[] = {"subdivided", "voxel remeshed", "direct"};
        mStatus->setText(QString("Sculpting %1: %2, %L3 faces").arg(QString::fromStdString(n->path()), modes[int(s->mode)]).arg(s->sculpt.mesh().numFaces()));
    }
    else
    {
        auto meshes = selectedMeshes();
        mStatus->setText(meshes.empty() ? "Select a mesh, then Enter." : QString("Ready to sculpt %1.").arg(QString::fromStdString(meshes.back())));
    }
    mEnter->setEnabled(!n && !selectedMeshes().empty());
    mExit->setEnabled(n != nullptr);
    mCancel->setEnabled(n != nullptr);

    scene::Node* sym = n;
    if (!sym)
        for (auto it = g.selection().rbegin(); it != g.selection().rend() && !sym; ++it)
            if (scene::Node* m = g.node(*it); m && m->type().isA("mesh"))
                sym = m;
    QSignalBlocker block(mSymmetry);
    mSymmetry->setEnabled(sym != nullptr);
    mSymmetry->setCurrentIndex(sym ? int(std::clamp<int64_t>(sym->as<int64_t>(scene::MeshNode::kSymmetry), 0, scene::kSymmetryRadial)) : 0);
}

void registerSculptUi()
{
    installSculptDrawer();

    actions::add({.id = "window.sculpt",
                  .label = "Sculpt Panel",
                  .category = "Windows",
                  .run = [] {
                      if (QDockWidget* d = sculptDock(false))
                          toggleDock(d);
                      else
                          revealDock(sculptDock(true));
                  },
                  .doc = "Sculpt mode, brushes, mask and reprojection tools.",
                  .python = "lila.ui.run_action(\"window.sculpt\")"});
    actions::add({.id = "sculpt.tool",
                  .label = "Sculpt",
                  .category = "Sculpt",
                  .run = [] {
                      if (sculpt::active() && sculptToolActive())
                          return exitSculpt();
                      showDock();
                      if (!sculpt::active())
                      {
                          if (selectedMeshes().empty())
                          {
                              log::warn("Sculpt: select a mesh");
                              return;
                          }
                          enterSculpt();
                      }
                      startSculptTool();
                  },
                  .doc = "Sculpt the selected mesh: enters sculpt mode if needed and picks up the sculpt brush. Again while sculpting exits sculpt mode.",
                  .python = "lila.ui.run_action(\"sculpt.tool\")"});
    actions::add({.id = "sculpt.enter",
                  .label = "Enter Sculpt",
                  .category = "Sculpt",
                  .run = enterSculpt,
                  .doc = "Build a high-resolution sculpt proxy of the selected mesh's cage (settings in the Sculpt panel).",
                  .python = ops::toPython("sculptEnter", {{"mode", "subdivide"}, {"levels", 2}})});
    actions::add({.id = "sculpt.exit",
                  .label = "Exit Sculpt",
                  .category = "Sculpt",
                  .run = exitSculpt,
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Move the low-poly cage onto the sculpt and leave sculpt mode.",
                  .python = ops::toPython("sculptExit", {{"method", "fit"}})});
    actions::add({.id = "sculpt.cancel",
                  .label = "Cancel Sculpt",
                  .category = "Sculpt",
                  .run = [] {
                      if (runCommand("sculptCancel", json::object()) && sculptToolActive())
                          viewportTools::set(nullptr);
                  },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Leave sculpt mode without changing the mesh.",
                  .python = ops::toPython("sculptCancel", json::object())});
    for (const BrushInfo& b : kBrushes)
    {
        std::string name(sculpt::brushName(b.brush));
        sculpt::Brush brush = b.brush;
        actions::add({.id = "sculpt.brush." + name,
                      .label = std::string("Sculpt Brush: ") + b.label,
                      .category = "Sculpt",
                      .run = [brush] {
                          sculptSettings().brush = brush;
                          sculptSettingsChanged();
                          if (sculpt::active())
                              startSculptTool();
                      },
                      .doc = b.doc,
                      .python = "lila.ui.run_action(\"sculpt.brush." + name + "\")"});
    }
    auto maskAction = [](std::string id, std::string label, std::string op, std::string layer, std::string doc) {
        json call = {{"op", op}, {"layer", layer}};
        actions::add({.id = id,
                      .label = label,
                      .category = "Sculpt",
                      .run = [call] { runCommand("sculptMask", call); },
                      .enabled = [] { return sculpt::active() != nullptr; },
                      .doc = doc,
                      .python = ops::toPython("sculptMask", call)});
    };
    maskAction("sculpt.maskInvert", "Invert Mask", "invert", "mask", "Masked points become free and free ones masked.");
    maskAction("sculpt.maskClear", "Clear Mask", "clear", "mask", "Unmask everything.");
    maskAction("sculpt.maskFill", "Mask All", "fill", "mask", "Mask everything (then paint with Ctrl to free areas).");
    maskAction("sculpt.maskBlur", "Blur Mask", "blur", "mask", "Soften the mask's edges.");
    maskAction("sculpt.freezeClear", "Unfreeze All", "clear", "freeze", "Free every frozen point.");

    actions::add({.id = "mesh.relaxKeepForm",
                  .label = "Relax (Keep Form)",
                  .category = "Mesh",
                  .run = [] { runCommand("relaxMesh", {{"iterations", 10}, {"strength", 0.5}, {"pinCreases", true}, {"pinSeams", true}}); },
                  .doc = "Even out the point spacing of the selected components (or whole meshes), sliding points along the "
                         "surface so the shape stays. Borders, creases and UV seams stay put.",
                  .python = ops::toPython("relaxMesh", {{"iterations", 10}, {"pinCreases", true}, {"pinSeams", true}})});
    actions::add({.id = "mesh.reproject",
                  .label = "Reproject onto Target",
                  .category = "Mesh",
                  .run = [] {
                      auto meshes = selectedMeshes();
                      if (meshes.size() < 2)
                      {
                          log::warn("Reproject onto Target: select the target surface, then the meshes to move");
                          return;
                      }
                      runCommand("shrinkwrap", {{"args", std::vector<std::string>(meshes.begin() + 1, meshes.end())}, {"target", meshes.front()}});
                  },
                  .doc = "Shrinkwrap meshes onto a surface: select the target first, then the meshes to move. The shrinkwrap "
                         "command also projects along normals or a direction, with an offset.",
                  .python = ops::toPython("shrinkwrap", {{"args", {"low"}}, {"target", "high"}, {"method", "closest"}, {"offset", 0.0}})});
    actions::add({.id = "mesh.transferShape",
                  .label = "Transfer Shape (High to Low)",
                  .category = "Mesh",
                  .run = [] {
                      auto meshes = selectedMeshes();
                      if (meshes.size() < 2)
                      {
                          log::warn("Transfer Shape: select the high poly first, then the low-poly cages");
                          return;
                      }
                      runCommand("transferShape", {{"args", meshes}});
                  },
                  .doc = "Fit low-poly cages to a high poly (an imported sculpt) so their subdivision matches it, keeping UVs. "
                         "Select the high poly first.",
                  .python = ops::toPython("transferShape", {{"args", {"high", "low"}}, {"method", "fit"}, {"levels", 2}})});
}
} // namespace lila::ui
