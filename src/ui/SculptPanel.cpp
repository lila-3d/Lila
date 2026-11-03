#include "ui/SculptPanel.h"

#include "brush/Alphas.h"
#include "brush/BrushUi.h"
#include "brush/Presets.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "core/Log.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"
#include "vcolor/Paint.h"
#include "ui/ColorPopup.h"
#include "ui/ReferenceOverlay.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DockSidebar.h"
#include "ui/FileDialog.h"
#include "ui/FixedRowLayout.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/ModelingToolkit.h"
#include "ui/SculptTool.h"
#include "ui/SpinScrub.h"
#include "ui/UiCommon.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileSystemWatcher>
#include <QImageReader>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QUrl>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
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
    {sculpt::Brush::DamStandard, "slash", "Dam Standard", "Cut a narrow V groove that pinches its edges in, for wrinkles and seams; Ctrl raises a sharp ridge."},
    {sculpt::Brush::TrimDynamic, "cut", "Trim Dynamic", "Cut flat to a plane just under the brush, for hard planes; Ctrl fills up to it."},
    {sculpt::Brush::Polish, "sparkles", "Polish", "Smooth along the surface's normal only and ease onto its plane: crisp, flat forms."},
    {sculpt::Brush::Fold, "ripple", "Fold", "Raise a cloth fold along the stroke, pinched from the sides; Ctrl presses a crease in."},
    {sculpt::Brush::Mask, "mask", "Mask", "Paint the mask, which shields points from every brush; Ctrl erases it."},
    {sculpt::Brush::Relax, "wave-sine", "Relax", "Even out the point spacing, sliding along the surface so the form stays."},
    {sculpt::Brush::Freeze, "snowflake", "Freeze", "Pin points so no brush moves them; Ctrl unfreezes."},
    {sculpt::Brush::Paint, "paint", "Paint", "Paint polypaint in the Polypaint color without moving anything. The mask shields points."},
}};

/** Brush button ids past the built-ins pick a registered brush, and past those a preset. */
constexpr int kCustomBrushId = 1000;
constexpr int kPresetId = 2000;

/** A preset's button icon: its own icon, else its alpha, else its brush's. */
QIcon presetIcon(const brush::presets::Preset& p)
{
    if (!p.icon.empty())
        return icon(QString::fromStdString(p.icon));
    if (const std::string file = brush::alphas::resolve(p.settings.alphaPath); !file.empty())
    {
        QImageReader reader(QString::fromStdString(file));
        reader.setScaledSize(QSize(48, 48));
        if (QImage img = reader.read(); !img.isNull())
            return QIcon(QPixmap::fromImage(img));
    }
    if (auto b = sculpt::brushFromName(p.brush))
        for (const BrushInfo& info : kBrushes)
            if (info.brush == *b)
                return icon(info.icon);
    if (const sculpt::BrushDef* def = sculpt::brushRegistry().find(p.brush); def && !def->icon.empty())
        return icon(QString::fromStdString(def->icon));
    return icon("puzzle");
}

std::vector<std::string> droppedPresets(const QMimeData* mime)
{
    const QString ext = QString::fromUtf8(brush::presets::kExtension.data(), qsizetype(brush::presets::kExtension.size()));
    std::vector<std::string> out;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && url.toLocalFile().endsWith(ext, Qt::CaseInsensitive))
            out.push_back(url.toLocalFile().toStdString());
    return out;
}

QColor swatchColor(const glm::vec3& linear)
{
    const glm::vec4 s = vcolor::linearToSrgb(glm::vec4(glm::clamp(linear, 0.0f, 1.0f), 1.0f));
    return QColor::fromRgbF(s.r, s.g, s.b);
}

bool runActionWithOptions(const char* name, const json& call) { return actions::run({{"command", name}, {"args", ops::options::callWith(name, call)}}); }

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
    setDockDocs(dock, "sculpting");
    mw->addDockWidget(Qt::RightDockWidgetArea, dock);
    if (auto* toolkit = mw->findChild<QDockWidget*>(kModelingToolkitDock); toolkit && !toolkit->isFloating() && toolkit->isVisible())
        mw->tabifyDockWidget(toolkit, dock);
    // Wide enough for five brush columns with their labels and the session rows. Set as the tab extent too, or picking
    // the tab in a narrower group keeps the group's width.
    const int width = std::max(dock->sizeHint().width(), dock->fontMetrics().averageCharWidth() * 84);
    dock->setProperty(kDefaultTabExtent, width);
    mw->resizeDocks({dock}, {width}, Qt::Horizontal);
    return dock;
}

void showDock()
{
    if (QDockWidget* d = sculptDock(true))
        revealDock(d);
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
    if (runActionWithOptions("sculptEnter", {{"args", {meshes.back()}}, {"mode", st.proxyMode}, {"levels", st.levels}, {"resolution", st.resolution}}))
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
    if (runActionWithOptions("sculptExit", {{"method", st.exitMethod}, {"lowLevel", st.lowLevel}, {"high", st.highPoly}, {"highLevel", st.highLevel}}) &&
        sculptToolActive())
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
    mStatus->setObjectName("sculptStatus");
    sform->addRow(mStatus);
    mMode = new QComboBox(session);
    mMode->addItem("Subdivision levels", "subdivide");
    mMode->addItem("Voxel remesh", "remesh");
    mMode->addItem("Mesh itself", "direct");
    mMode->setItemData(0, "A subdivided copy with levels to step between. Exit fits the mesh back to it exactly.", Qt::ToolTipRole);
    mMode->setItemData(1, "An even voxel remesh, free of the mesh's topology. Exit projects the mesh onto it.", Qt::ToolTipRole);
    mMode->setItemData(2, "The mesh's own points, no copy.", Qt::ToolTipRole);
    mMode->setToolTip("What you sculpt on. The mesh keeps its topology and UVs whichever you pick.");
    setComboDefault(mMode, mMode->findData(QString::fromStdString(defaults.proxyMode)));
    sform->addRow("Sculpt on", mMode);
    mLevels = new QSpinBox(session);
    mLevels->setRange(1, 6);
    mLevels->setToolTip("Subdivision levels to start with; each has four times the faces. Add or step between them while sculpting.");
    setSpinDefault(mLevels, defaults.levels);
    sform->addRow("Levels", mLevels);
    mLevelRow = new QWidget(session);
    mLevelRow->setObjectName("sculptLevelRow");
    {
        auto* row = new QHBoxLayout(mLevelRow);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        mLevelDown = actionButton("sculpt.levelDown", "chevron-down", QString(), mLevelRow);
        mLevelUp = actionButton("sculpt.levelUp", "chevron-up", QString(), mLevelRow);
        mLevelLabel = new QLabel(mLevelRow);
        mLevelLabel->setMinimumWidth(QFontMetrics(mLevelLabel->font()).horizontalAdvance("0 / 0  "));
        mLevelLabel->setAlignment(Qt::AlignCenter);
        mLevelLabel->setObjectName("sculptLevel");
        mAddLevel = actionButton("sculpt.addLevel", "square-plus", "Add", mLevelRow);
        mDeleteHigher = actionButton("sculpt.deleteHigher", "trash-x", "Delete Higher", mLevelRow);
        row->addWidget(mLevelDown);
        row->addWidget(mLevelLabel);
        row->addWidget(mLevelUp);
        row->addWidget(mAddLevel);
        row->addWidget(mDeleteHigher);
        row->addStretch(1);
    }
    sform->addRow("Level", mLevelRow);
    mResolution = new QSpinBox(session);
    mResolution->setRange(16, 2048);
    mResolution->setToolTip("Voxels along the longest side of the remesh.");
    setSpinDefault(mResolution, defaults.resolution);
    sform->addRow("Resolution", mResolution);
    mExitMethod = new QComboBox(session);
    mExitMethod->addItem("Game mesh", "closest");
    mExitMethod->addItem("Game mesh, along normals", "normal");
    mExitMethod->addItem("Subdivision cage", "fit");
    mExitMethod->setItemData(0, "Each point moves to the nearest spot on the sculpt, so the low poly lies on it. Bake normals from the "
                                "high poly onto it.", Qt::ToolTipRole);
    mExitMethod->setItemData(1, "Each point moves straight in or out along its normal onto the sculpt. Try it when Game mesh slides "
                                "points sideways into a crease.", Qt::ToolTipRole);
    mExitMethod->setItemData(2, "Points float off the sculpt so that subdividing the low poly gives the sculpt back. For a mesh you "
                                "subdivide afterwards, not one you bake.", Qt::ToolTipRole);
    mExitMethod->setToolTip("What Exit makes of the mesh you entered with. It keeps its topology and UVs either way. Show cage previews it.");
    setComboDefault(mExitMethod, mExitMethod->findData(QString::fromStdString(defaults.exitMethod)));
    {
        auto* lowRow = new QWidget(session);
        auto* row = new QHBoxLayout(lowRow);
        row->setContentsMargins(0, 0, 0, 0);
        mExitMethod->setParent(lowRow);
        mLowLevel = new QSpinBox(lowRow);
        mLowLevel->setRange(0, 6);
        mLowLevel->setPrefix("Level ");
        mLowLevel->setToolTip("The level the low poly comes out at. Level 0 keeps the mesh's topology; each level above subdivides "
                              "it once more, UVs included.");
        setSpinDefault(mLowLevel, defaults.lowLevel);
        auto* duplicate = actionButton("sculpt.duplicateLowPoly", "copy", QString(), lowRow);
        row->addWidget(mExitMethod, 1);
        row->addWidget(mLowLevel);
        row->addWidget(duplicate);
        sform->addRow("Low poly", lowRow);
    }
    {
        auto* highRow = new QWidget(session);
        auto* row = new QHBoxLayout(highRow);
        row->setContentsMargins(0, 0, 0, 0);
        mHighPoly = new QComboBox(highRow);
        mHighPoly->addItem("Highest level", "highest");
        mHighPoly->addItem("Below highest", "belowHighest");
        mHighPoly->addItem("Level", "level");
        mHighPoly->addItem("Don't keep", "none");
        mHighPoly->setItemData(1, "Keeps a level some steps below the highest: a lighter high poly to bake from.", Qt::ToolTipRole);
        mHighPoly->setItemData(3, "Keeps nothing: the sculpt and its levels are gone after Exit.", Qt::ToolTipRole);
        mHighPoly->setToolTip("Exit keeps the sculpt as the hidden child <mesh>_high, at this level, to bake from. Every level "
                              "comes back the next time you sculpt the mesh.");
        setComboDefault(mHighPoly, mHighPoly->findData(QString::fromStdString(defaults.highPoly)));
        mHighLevel = new QSpinBox(highRow);
        mHighLevel->setRange(0, 6);
        setSpinDefault(mHighLevel, defaults.highLevel);
        row->addWidget(mHighPoly, 1);
        row->addWidget(mHighLevel);
        sform->addRow("High poly", highRow);
    }
    auto* buttons = new QHBoxLayout;
    mEnter = new QPushButton(icon("door-enter"), "Enter", session);
    mExit = new QPushButton(icon("door-exit"), "Exit", session);
    mPreview = new QPushButton(icon("eye"), "Preview", session);
    mPreview->setObjectName("sculptPreview");
    mPreview->setCheckable(true);
    mPreview->setToolTip("Show the low poly Exit would leave, isolated, before you commit. Again to sculpt on. Shift+V");
    mCancel = new QPushButton("Cancel", session);
    mEnter->setToolTip("Sculpt the selected mesh.");
    mExit->setToolTip("Move the mesh onto the sculpt and leave sculpt mode.");
    mCancel->setToolTip("Leave sculpt mode; the mesh stays as it was.");
    buttons->addWidget(mEnter);
    buttons->addWidget(mPreview);
    buttons->addWidget(mExit);
    buttons->addWidget(mCancel);
    sform->addRow(buttons);
    col->addWidget(session);

    // Brushes
    mBrushBox = new QGroupBox("Brushes", this);
    mBrushFlow = new FixedRowLayout(5, 3);
    mBrushBox->setLayout(mBrushFlow);
    mBrushes = new QButtonGroup(this);
    mBrushes->setExclusive(true);
    for (const BrushInfo& b : kBrushes)
        mBrushes->addButton(brushButton(icon(b.icon), b.label, b.doc), int(b.brush));
    rebuildCustomBrushes();
    col->addWidget(mBrushBox);

    // Presets
    mPresetBox = new QGroupBox("Presets", this);
    auto* presetCol = new QVBoxLayout(mPresetBox);
    mPresetFlow = new FlowLayout(3);
    presetCol->addLayout(mPresetFlow);
    auto* presetRow = new QHBoxLayout;
    auto* savePresetButton = new QToolButton(mPresetBox);
    savePresetButton->setIcon(icon("bookmark-plus"));
    savePresetButton->setText("Save Preset");
    savePresetButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    savePresetButton->setToolTip("Save the current brush and its settings, alpha included, as a preset");
    auto* importButton = new QToolButton(mPresetBox);
    importButton->setIcon(icon("file-import"));
    importButton->setText("Import");
    importButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    importButton->setToolTip("Install .lilabrush files (or drop them on this panel)");
    presetRow->addWidget(savePresetButton);
    presetRow->addWidget(importButton);
    presetRow->addStretch(1);
    presetCol->addLayout(presetRow);
    col->addWidget(mPresetBox);
    connect(savePresetButton, &QToolButton::clicked, this, [this] { savePreset(); });
    connect(importButton, &QToolButton::clicked, this, [this] { importPresets(); });
    setAcceptDrops(true);
    mPresetWatcher = new QFileSystemWatcher(this);
    connect(mPresetWatcher, &QFileSystemWatcher::directoryChanged, this, [] { brush::presets::notifyChanged(); });
    rebuildPresets();

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
    auto* shareRow = new QHBoxLayout;
    const SculptSettings shareDefaults;
    mShareRadius = new QCheckBox("Radius", settingsBox);
    mShareStrength = new QCheckBox("Strength", settingsBox);
    mShareStroke = new QCheckBox("Stroke", settingsBox);
    mShareRadius->setToolTip("Every brush uses the same radius; off, each brush keeps its own");
    mShareStrength->setToolTip("Every brush uses the same strength; off, each brush keeps its own");
    mShareStroke->setToolTip("Every brush uses the same falloff, spacing, alpha, stabiliser and the rest of the Stroke group; off, "
                             "each brush keeps its own");
    setCheckDefault(mShareRadius, shareDefaults.shareRadius);
    setCheckDefault(mShareStrength, shareDefaults.shareStrength);
    setCheckDefault(mShareStroke, shareDefaults.shareStroke);
    shareRow->addWidget(mShareRadius);
    shareRow->addWidget(mShareStrength);
    shareRow->addWidget(mShareStroke);
    shareRow->addStretch(1);
    bform->addRow("Shared", shareRow);
    mShowCage = new QCheckBox("Show cage", settingsBox);
    mShowCage->setToolTip("Draw the low poly over the sculpt as Exit will leave it, refitted as you sculpt.");
    setCheckDefault(mShowCage, defaults.showCage);
    bform->addRow(mShowCage);
    mSymmetryLine = new QCheckBox("Symmetry line", settingsBox);
    mSymmetryLine->setToolTip("The red line along the mesh's mirror plane.");
    setCheckDefault(mSymmetryLine, defaults.symmetryLine);
    bform->addRow(mSymmetryLine);
    col->addWidget(settingsBox);
    mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return sculptSettings().stroke; },
                                                  [] { sculptSettingsChanged(); });
    col->addWidget(mStrokeOptions);

    // Display
    auto* displayBox = new QGroupBox("Display", this);
    auto* dform = new QFormLayout(displayBox);
    mMaterial = new QComboBox(displayBox);
    for (const auto& [id, label] : sculptMaterials())
        mMaterial->addItem(QString::fromStdString(label), QString::fromStdString(id));
    mMaterial->setToolTip("The material the sculpt is drawn with while sculpting; the mesh's own comes back on exit");
    setComboDefault(mMaterial, std::max(0, mMaterial->findData(QString::fromStdString(SculptSettings{}.material))));
    dform->addRow("Material", mMaterial);
    mWireframe = new QCheckBox("Wireframe", displayBox);
    mWireframe->setToolTip("The sculpt mesh's wireframe (Shift+F in the viewport)");
    setCheckDefault(mWireframe, SculptSettings{}.wireframe);
    dform->addRow(mWireframe);
    mWireOpacity = new QSlider(Qt::Horizontal, displayBox);
    mWireOpacity->setRange(5, 100);
    mWireOpacity->setToolTip("How strongly the wireframe draws over the sculpt");
    dform->addRow("Wire Opacity", mWireOpacity);
    col->addWidget(displayBox);

    // Polypaint
    auto* paintBox = new QGroupBox("Polypaint", this);
    auto* pform = new QFormLayout(paintBox);
    mColor = new QToolButton(paintBox);
    mColor->setMinimumWidth(60);
    mColor->setToolTip("The color the Paint brush, and shape brushes with Paint while sculpting, lay down");
    auto* colorRow = new QHBoxLayout;
    colorRow->setSpacing(2);
    colorRow->addWidget(mColor);
    pform->addRow("Color", colorRow);
    mPaint = new QCheckBox("Paint while sculpting", paintBox);
    mPaint->setToolTip("Shape brushes also paint the color wherever they move the surface");
    setCheckDefault(mPaint, SculptSettings{}.paint);
    pform->addRow(mPaint);
    auto* pflow = new QHBoxLayout;
    pflow->addWidget(actionButton("sculpt.colorFill", "bucket-droplet", "Fill", paintBox));
    pflow->addWidget(actionButton("sculpt.colorClear", "eraser", "Clear", paintBox));
    pflow->addStretch(1);
    pform->addRow(pflow);
    col->addWidget(paintBox);

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
    col->addWidget(referenceOverlays::createSculptSection(this));
    col->addStretch(1);

    syncFromSettings();
    refreshSession();

    auto changed = [] { sculptSettingsChanged(); };
    connect(mBrushes, &QButtonGroup::idClicked, this, [this](int id) {
        if (id >= kPresetId && size_t(id - kPresetId) < mPresets.size())
        {
            if (!selectSculptPreset(mPresets[size_t(id - kPresetId)]))
                syncFromSettings();
        }
        else if (id >= kCustomBrushId && size_t(id - kCustomBrushId) < mCustomBrushes.size())
            selectSculptBrush(sculpt::Brush::Custom, mCustomBrushes[size_t(id - kCustomBrushId)]);
        else
            selectSculptBrush(sculpt::Brush(id));
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
    connect(mPaint, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().paint = on; changed(); });
    connect(mShareRadius, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().shareRadius = on; changed(); });
    connect(mShareStrength, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().shareStrength = on; changed(); });
    connect(mShareStroke, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().shareStroke = on; changed(); });
    connect(mWireframe, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().wireframe = on; changed(); });
    connect(mWireOpacity, &QSlider::valueChanged, this, [changed](int v) { sculptSettings().wireOpacity = v / 100.0; changed(); });
    connect(mMaterial, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        sculptSettings().material = mMaterial->currentData().toString().toStdString();
        changed();
    });
    connect(mColor, &QToolButton::clicked, this, [this, changed] {
        auto apply = [changed](const QColor& c) {
            sculptSettings().color = glm::vec3(vcolor::srgbToLinear(glm::vec4(float(c.redF()), float(c.greenF()), float(c.blueF()), 1.0f)));
            changed();
        };
        if (QColor c = colorPopup::getColor(swatchColor(sculptSettings().color), mColor, "Polypaint Color", false, apply); c.isValid())
            apply(c);
    });
    colorRow->addWidget(colorPopup::eyedropperButton(paintBox, [changed](const glm::dvec3& c) {
        sculptSettings().color = glm::vec3(c);
        changed();
    }));
    colorRow->addStretch(1);
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
    connect(mHighPoly, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        sculptSettings().highPoly = mHighPoly->currentData().toString().toStdString();
        changed();
    });
    connect(mHighLevel, &QSpinBox::valueChanged, this, [changed](int v) { sculptSettings().highLevel = v; changed(); });
    connect(mLowLevel, &QSpinBox::valueChanged, this, [changed](int v) { sculptSettings().lowLevel = v; changed(); });
    connect(mSymmetryLine, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().symmetryLine = on; changed(); });
    connect(mSymmetry, &QComboBox::activated, this, [this](int) {
        std::vector<std::string> targets;
        if (auto s = sculpt::active(); s && scene::Graph::current().node(s->node))
            targets.push_back(scene::Graph::current().node(s->node)->path());
        else
            targets = selectedMeshes();
        if (!targets.empty())
            runActionWithOptions("setSymmetry", {{"args", targets}, {"axis", mSymmetry->currentData().toString().toStdString()}});
    });
    connect(mEnter, &QPushButton::clicked, this, [] { actions::run(json("sculpt.enter")); });
    connect(mExit, &QPushButton::clicked, this, [] { actions::run(json("sculpt.exit")); });
    connect(mPreview, &QPushButton::clicked, this, [] { actions::run(json("sculpt.previewLowPoly")); });
    connect(mCancel, &QPushButton::clicked, this, [] { actions::run(json("sculpt.cancel")); });

    mSettingsListener = addSculptSettingsListener([this] { syncFromSettings(); });
    mBrushRegistryListener = sculpt::brushRegistry().addListener([this](RegistryChange, const std::string&, const std::string&) {
        QTimer::singleShot(0, this, [this] {
            rebuildCustomBrushes();
            rebuildPresets();
            syncFromSettings();
        });
    });
    mPresetListener = brush::presets::addListener([this] {
        if (mPresetsQueued)
            return;
        mPresetsQueued = true;
        QTimer::singleShot(0, this, [this] {
            mPresetsQueued = false;
            rebuildPresets();
            syncFromSettings();
        });
    });
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
    sculpt::brushRegistry().removeListener(mBrushRegistryListener);
    brush::presets::removeListener(mPresetListener);
    removeSculptSettingsListener(mSettingsListener);
    sculpt::removeListener(mSessionListener);
    scene::Graph::current().removeListener(mGraphListener);
}

QToolButton* SculptPanel::brushButton(const QIcon& ic, const QString& label, const QString& doc, QLayout* flow)
{
    flow = flow ? flow : mBrushFlow;
    auto* btn = new QToolButton(flow == mBrushFlow ? mBrushBox : mPresetBox);
    btn->setIcon(ic);
    btn->setText(label);
    btn->setCheckable(true);
    btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    if (flow != mBrushFlow)
        btn->setMinimumWidth(66);
    btn->setToolTip(doc.isEmpty() ? label : QString("%1\n%2").arg(label, doc));
    flow->addWidget(btn);
    return btn;
}

void SculptPanel::rebuildCustomBrushes()
{
    for (size_t k = 0; k < mCustomBrushes.size(); ++k)
        if (QAbstractButton* b = mBrushes->button(kCustomBrushId + int(k)))
        {
            mBrushes->removeButton(b);
            delete b;
        }
    mCustomBrushes = sculpt::brushRegistry().idsInOrder();
    for (size_t k = 0; k < mCustomBrushes.size(); ++k)
    {
        const sculpt::BrushDef& def = *sculpt::brushRegistry().find(mCustomBrushes[k]);
        const QString label = QString::fromStdString(def.label.empty() ? mCustomBrushes[k] : def.label);
        mBrushes->addButton(brushButton(icon(QString::fromStdString(def.icon.empty() ? "puzzle" : def.icon)), label, QString::fromStdString(def.doc)),
                            kCustomBrushId + int(k));
    }
}

void SculptPanel::rebuildPresets()
{
    for (size_t k = 0; k < mPresets.size(); ++k)
        if (QAbstractButton* b = mBrushes->button(kPresetId + int(k)))
        {
            mBrushes->removeButton(b);
            delete b;
        }
    mPresets.clear();
    for (const brush::presets::Preset& p : brush::presets::library("sculpt"))
    {
        QString doc = QString::fromStdString(p.doc);
        const QString where = QString::fromStdString(p.sourceLabel + (p.category.empty() ? "" : " / " + p.category));
        doc += doc.isEmpty() ? where : "\n" + where;
        QToolButton* btn = brushButton(presetIcon(p), QString::fromStdString(p.label), doc, mPresetFlow);
        const size_t index = mPresets.size();
        btn->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(btn, &QToolButton::customContextMenuRequested, this, [this, btn, index](const QPoint& at) { presetMenu(index, btn->mapToGlobal(at)); });
        mBrushes->addButton(btn, kPresetId + int(index));
        mPresets.push_back(p.id);
    }

    if (!mPresetWatcher->directories().isEmpty())
        mPresetWatcher->removePaths(mPresetWatcher->directories());
    std::error_code ec;
    for (const brush::presets::Folder& f : brush::presets::folders())
        if (std::filesystem::is_directory(f.dir, ec))
            mPresetWatcher->addPath(QString::fromStdWString(f.dir.wstring()));
}

void SculptPanel::savePreset()
{
    SculptSettings& s = sculptSettings();
    std::optional<brush::presets::Preset> current = s.preset.empty() ? std::nullopt : brush::presets::find(s.preset);
    const bool own = current && (current->source == "user" || current->source == "project");
    const std::string brushName = s.brush == sculpt::Brush::Custom ? s.custom : std::string(sculpt::brushName(s.brush));
    QString suggested = current ? QString::fromStdString(current->label)
                        : s.brush == sculpt::Brush::Custom ? QString::fromStdString(s.custom)
                                                           : QString(kBrushes[size_t(s.brush)].label);
    bool ok = false;
    const QString label = QInputDialog::getText(this, "Save Brush Preset", "Name", QLineEdit::Normal, suggested, &ok).trimmed();
    if (!ok || label.isEmpty())
        return;
    // Saving under the open preset's own name updates it in place; any other name makes a new one.
    const bool replace = own && label.toStdString() == current->label;
    json call = {{"args", {label.toStdString()}},
                 {"brush", brushName},
                 {"settings", s.stroke.toJson()},
                 {"alpha", s.stroke.alphaPath},
                 {"scope", replace ? current->source : std::string("user")},
                 {"replace", replace}};
    json result;
    try
    {
        result = ops::execute("brushPresetSave", call, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::error("{}", e.what());
        return;
    }
    const std::string id = result.value("id", "");
    if (id.empty())
        return;
    if (replace)
        s.perBrush.erase("preset:" + id);
    rebuildPresets();
    selectSculptPreset(id);
}

void SculptPanel::importPresets()
{
    const fileDialog::ContextScope scope("brushPresets", "Brush Presets");
    const QStringList files = fileDialog::getOpenFileNames(this, "Import Brush Presets", {}, "Brush presets (*.lilabrush)");
    if (files.isEmpty())
        return;
    json args = json::array();
    for (const QString& f : files)
        args.push_back(f.toStdString());
    runActionWithOptions("brushPresetInstall", {{"args", args}});
}

void SculptPanel::presetMenu(size_t index, const QPoint& globalPos)
{
    if (index >= mPresets.size())
        return;
    std::optional<brush::presets::Preset> p = brush::presets::find(mPresets[index]);
    if (!p)
        return;
    QMenu menu(this);
    menu.addAction(icon("file-export"), "Export...", this, [this, p] {
        const fileDialog::ContextScope scope("brushPresets", "Brush Presets");
        const QString file = fileDialog::getSaveFileName(this, "Export Brush Preset", QString::fromStdString(p->label + ".lilabrush"),
                                                         "Brush presets (*.lilabrush)");
        if (!file.isEmpty())
            runActionWithOptions("brushPresetExport", {{"args", {p->id}}, {"file", file.toStdString()}});
    });
    menu.addAction(icon("folder-open"), "Show in Folder", this, [p] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(p->file.parent_path().wstring())));
    });
    if (p->source == "user" || p->source == "project")
    {
        menu.addSeparator();
        menu.addAction(icon("trash"), "Delete", this, [p] {
            if (!runActionWithOptions("brushPresetDelete", {{"args", {p->id}}}))
                return;
            SculptSettings& s = sculptSettings();
            s.perBrush.erase("preset:" + p->id);
            if (s.preset == p->id)
                selectSculptBrush(s.brush, s.custom);
        });
    }
    menu.exec(globalPos);
}

void SculptPanel::dragEnterEvent(QDragEnterEvent* e)
{
    if (droppedPresets(e->mimeData()).empty())
        return e->ignore();
    e->acceptProposedAction();
}

void SculptPanel::dropEvent(QDropEvent* e)
{
    std::vector<std::string> files = droppedPresets(e->mimeData());
    if (files.empty())
        return e->ignore();
    e->acceptProposedAction();
    // After the drop returns, so Explorer's drag loop isn't held.
    QTimer::singleShot(0, this, [files] { runActionWithOptions("brushPresetInstall", {{"args", files}}); });
}

void SculptPanel::syncFromSettings()
{
    const SculptSettings& s = sculptSettings();
    QSignalBlocker b1(mRadius), b2(mStrength), b3(mSpacing), b4(mFalloff), b5(mFrontFaces), b6(mShowCage), b7(mMode), b8(mLevels),
        b9(mResolution), b10(mExitMethod), b11(mHighPoly), b19(mHighLevel), b20(mSymmetryLine), b21(mLowLevel), b12(mPaint), b13(mShareRadius), b14(mShareStrength), b15(mShareStroke),
        b16(mWireframe), b17(mMaterial), b18(mWireOpacity);
    mShareRadius->setChecked(s.shareRadius);
    mShareStrength->setChecked(s.shareStrength);
    mShareStroke->setChecked(s.shareStroke);
    mWireframe->setChecked(s.wireframe);
    mWireOpacity->setValue(int(std::lround(s.wireOpacity * 100.0)));
    mWireOpacity->setEnabled(s.wireframe);
    mMaterial->setCurrentIndex(std::max(0, mMaterial->findData(QString::fromStdString(s.material))));
    int checked = int(s.brush);
    if (auto preset = std::ranges::find(mPresets, s.preset); !s.preset.empty() && preset != mPresets.end())
        checked = kPresetId + int(preset - mPresets.begin());
    else if (s.brush == sculpt::Brush::Custom)
    {
        auto it = std::ranges::find(mCustomBrushes, s.custom);
        checked = it == mCustomBrushes.end() ? -1 : kCustomBrushId + int(it - mCustomBrushes.begin());
    }
    if (QAbstractButton* b = mBrushes->button(checked))
        b->setChecked(true);
    else if (QAbstractButton* on = mBrushes->checkedButton())
    {
        mBrushes->setExclusive(false);
        on->setChecked(false);
        mBrushes->setExclusive(true);
    }
    mPaint->setChecked(s.paint);
    mColor->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid rgb(20,20,20); min-height: 16px; }").arg(swatchColor(s.color).name()));
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
    mHighPoly->setCurrentIndex(std::max(0, mHighPoly->findData(QString::fromStdString(s.highPoly))));
    mHighLevel->setValue(s.highLevel);
    mLowLevel->setValue(s.lowLevel);
    mSymmetryLine->setChecked(s.symmetryLine);
    refreshSession();
}

void SculptPanel::refreshSession()
{
    scene::Graph& g = scene::Graph::current();
    auto s = sculpt::active();
    scene::Node* n = s ? g.node(s->node) : nullptr;
    const bool levels = n && s->stack;
    bool resuming = false;
    int resumingLevels = 0;
    if (n)
    {
        const QString name = QString::fromStdString(n->name());
        const int faces = int(s->sculpt.mesh().numFaces());
        if (levels)
            mStatus->setText(QString("Sculpting %1: level %2 of %3, %L4 faces").arg(name).arg(s->level).arg(s->stack->top()).arg(faces));
        else
            mStatus->setText(QString("Sculpting %1: %2, %L3 faces").arg(name, s->mode == sculpt::ProxyMode::Remesh ? "voxel remesh" : "the mesh itself").arg(faces));
    }
    else
    {
        auto meshes = selectedMeshes();
        scene::Node* m = meshes.empty() ? nullptr : g.find(meshes.back());
        const std::optional<sculpt::KeptSculpt> kept = m ? sculpt::keptSculpt(*m) : std::nullopt;
        resuming = kept.has_value();
        resumingLevels = kept ? kept->levels : 0;
        if (!m)
            mStatus->setText("Select a mesh, then Enter.");
        else if (kept && kept->levels > 0)
            mStatus->setText(QString("Ready: %1, resumes level %2 of %3").arg(QString::fromStdString(m->name()))
                                 .arg(kept->level >= 0 ? kept->level : kept->levels).arg(kept->levels));
        else if (kept)
            mStatus->setText(QString("Ready: %1, resumes its sculpt").arg(QString::fromStdString(m->name())));
        else
            mStatus->setText(QString("Ready: %1").arg(QString::fromStdString(m->name())));
    }
    // How a sculpt starts is fixed once it has; the level row replaces the starting levels while it runs.
    const std::string& mode = sculptSettings().proxyMode;
    mMode->setEnabled(!n);
    mLevels->setEnabled(!n && !resuming && mode == "subdivide");
    const std::string& high = sculptSettings().highPoly;
    mHighLevel->setVisible(high == "belowHighest" || high == "level");
    mHighLevel->setPrefix(high == "level" ? "Level " : "");
    mHighLevel->setSuffix(high == "belowHighest" ? " below" : "");
    // Levels that exist: the open sculpt's, else the kept one's, else the ones Enter will make.
    const int top = levels ? s->stack->top() : resumingLevels > 0 ? resumingLevels : mode == "subdivide" ? sculptSettings().levels : 0;
    {
        QSignalBlocker b1(mHighLevel), b2(mLowLevel);
        if (high == "level")
            mHighLevel->setRange(std::min(1, top), top);
        else
            mHighLevel->setRange(0, std::max(top - 1, 0));
        mLowLevel->setRange(0, top);
    }
    mLowLevel->setEnabled(top > 0);
    // Sculpting the mesh itself leaves nothing to keep.
    mHighPoly->setEnabled(n ? s->mode != sculpt::ProxyMode::Direct : mode != "direct");
    mResolution->setEnabled(!n && mode == "remesh");
    if (auto* form = qobject_cast<QFormLayout*>(mLevelRow->parentWidget()->layout()))
    {
        form->setRowVisible(mLevels, !levels);
        form->setRowVisible(mLevelRow, levels);
    }
    if (levels)
    {
        mLevelLabel->setText(QString("%1 / %2").arg(s->level).arg(s->stack->top()));
        mLevelDown->setEnabled(s->level > 0);
        mLevelUp->setEnabled(s->level < s->stack->top());
        mAddLevel->setEnabled(s->stack->top() < 6);
        mDeleteHigher->setEnabled(s->level > 0 && s->level < s->stack->top());
    }
    mEnter->setEnabled(!n && !selectedMeshes().empty());
    mExit->setEnabled(n != nullptr);
    mPreview->setEnabled(n != nullptr);
    {
        QSignalBlocker block(mPreview);
        mPreview->setChecked(sculptPreviewing());
    }
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

    ops::registerCommand({
        .name = "sculptBrush",
        .doc = "Pick the sculpt tool's brush: a built-in or registered brush by name, or a brush preset by id (see brushPresets). "
               "Returns {brush, preset}.",
        .flags = {{.name = "brush", .type = ArgType::String, .doc = "A brush name: draw, clayStrips, damStandard, ... or a registered brush's."},
                  {.name = "preset", .type = ArgType::String, .doc = "A brush preset id, such as user/Skin Pores."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (const std::string preset = a.get<std::string>("preset"); !preset.empty())
            {
                if (!selectSculptPreset(preset))
                    throw CommandError("sculptBrush: no usable preset '" + preset + "'");
            }
            else if (a.has("brush"))
            {
                sculpt::BrushSettings resolved;
                if (!sculpt::setBrush(resolved, a.get<std::string>("brush")))
                    throw CommandError("sculptBrush: no brush '" + a.get<std::string>("brush") + "'");
                selectSculptBrush(resolved.brush, resolved.custom);
            }
            const SculptSettings& s = sculptSettings();
            return {{"brush", s.brush == sculpt::Brush::Custom ? s.custom : std::string(sculpt::brushName(s.brush))}, {"preset", s.preset}};
        },
    });

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
                  .doc = "Sculpt the selected mesh on a high-resolution copy; the mesh keeps its topology (settings in the Sculpt panel).",
                  .python = ops::toPython("sculptEnter", {{"mode", "subdivide"}, {"levels", 2}})});
    auto levelAction = [](std::string id, std::string label, std::string command, json args, std::string doc) {
        actions::add({.id = id,
                      .label = label,
                      .category = "Sculpt",
                      .run = [command, args] { runActionWithOptions(command.c_str(), args); },
                      .enabled = [] { auto s = sculpt::active(); return s && s->stack; },
                      .doc = doc,
                      .python = ops::toPython(command, args)});
    };
    levelAction("sculpt.levelUp", "Sculpt Level Up", "sculptLevel", {{"step", 1}}, "Sculpt the next finer subdivision level (D while sculpting).");
    levelAction("sculpt.levelDown", "Sculpt Level Down", "sculptLevel", {{"step", -1}},
                "Sculpt the next coarser level; finer detail follows what you change there (Shift+D while sculpting).");
    levelAction("sculpt.addLevel", "Add Sculpt Level", "sculptAddLevel", json::object(), "Subdivide the finest level once more (Ctrl+D while sculpting).");
    levelAction("sculpt.deleteHigher", "Delete Higher Levels", "sculptDeleteHigher", json::object(),
                "Delete the levels above this one and their detail.");
    actions::add({.id = "sculpt.previewLowPoly",
                  .label = "Preview Low Poly",
                  .category = "Sculpt",
                  .run = [] { setSculptPreview(!sculptPreviewing()); },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Show the mesh as Exit Sculpt would leave it, with the Low poly choice, isolated in the view; again to sculpt on (Shift+V while sculpting).",
                  .python = "lila.ui.run_action(\"sculpt.previewLowPoly\")",
                  .checked = [] { return sculptPreviewing(); }});
    actions::add({.id = "sculpt.duplicateLowPoly",
                  .label = "Duplicate Low Poly",
                  .category = "Sculpt",
                  .run = [] {
                      const SculptSettings& st = sculptSettings();
                      runActionWithOptions("sculptDuplicateLowPoly", {{"method", st.exitMethod}, {"level", st.lowLevel}});
                  },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Copy the low poly Exit would leave, at the Low poly level, beside the mesh, and keep sculpting.",
                  .python = ops::toPython("sculptDuplicateLowPoly", {{"method", "closest"}, {"level", 0}})});
    actions::add({.id = "sculpt.exit",
                  .label = "Exit Sculpt",
                  .category = "Sculpt",
                  .run = exitSculpt,
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Move the mesh onto the sculpt and leave sculpt mode.",
                  .python = ops::toPython("sculptExit", {{"method", "fit"}})});
    actions::add({.id = "sculpt.cancel",
                  .label = "Cancel Sculpt",
                  .category = "Sculpt",
                  .run = [] {
                      if (runActionWithOptions("sculptCancel", json::object()) && sculptToolActive())
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
                          selectSculptBrush(brush);
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
                      .run = [call] { runActionWithOptions("sculptMask", call); },
                      .enabled = [] { return sculpt::active() != nullptr; },
                      .doc = doc,
                      .python = ops::toPython("sculptMask", call)});
    };
    maskAction("sculpt.maskInvert", "Invert Mask", "invert", "mask", "Masked points become free and free ones masked.");
    maskAction("sculpt.maskClear", "Clear Mask", "clear", "mask", "Unmask everything.");
    maskAction("sculpt.maskFill", "Mask All", "fill", "mask", "Mask everything (then paint with Ctrl to free areas).");
    maskAction("sculpt.maskBlur", "Blur Mask", "blur", "mask", "Soften the mask's edges.");
    maskAction("sculpt.freezeClear", "Unfreeze All", "clear", "freeze", "Free every frozen point.");
    actions::add({.id = "sculpt.colorFill",
                  .label = "Fill Polypaint",
                  .category = "Sculpt",
                  .run = [] {
                      const glm::vec3 c = sculptSettings().color;
                      runActionWithOptions("sculptColor", {{"op", "fill"}, {"color", json::array({c.r, c.g, c.b})}});
                  },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Fill the sculpt's polypaint with the Polypaint color; masked points keep theirs.",
                  .python = ops::toPython("sculptColor", {{"op", "fill"}, {"color", json::array({1.0, 0.5, 0.2})}})});
    actions::add({.id = "sculpt.colorClear",
                  .label = "Clear Polypaint",
                  .category = "Sculpt",
                  .run = [] { runActionWithOptions("sculptColor", {{"op", "clear"}}); },
                  .enabled = [] { return sculpt::active() != nullptr; },
                  .doc = "Clear the sculpt's polypaint back to white.",
                  .python = ops::toPython("sculptColor", {{"op", "clear"}})});

    actions::add({.id = "mesh.relaxKeepForm",
                  .label = "Relax (Keep Form)",
                  .category = "Mesh",
                  .run = [] { runActionWithOptions("relaxMesh", {{"pinCreases", true}, {"pinSeams", true}}); },
                  .doc = "Even out the point spacing of the selected components (or whole meshes), sliding points along the "
                         "surface so the shape stays. Borders, creases and UV seams stay put.",
                  .python = ops::toPython("relaxMesh", {{"iterations", 10}, {"pinCreases", true}, {"pinSeams", true}}),
                  .command = "relaxMesh",
                  .call = {{"pinCreases", true}, {"pinSeams", true}}});
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
                      runActionWithOptions("shrinkwrap", {{"args", std::vector<std::string>(meshes.begin() + 1, meshes.end())}, {"target", meshes.front()}});
                  },
                  .doc = "Shrinkwrap meshes onto a surface: select the target first, then the meshes to move. The shrinkwrap "
                         "command also projects along normals or a direction, with an offset.",
                  .python = ops::toPython("shrinkwrap", {{"args", {"low"}}, {"target", "high"}, {"method", "closest"}, {"offset", 0.0}}),
                  .command = "shrinkwrap"});
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
                      runActionWithOptions("transferShape", {{"args", meshes}});
                  },
                  .doc = "Fit low-poly cages to a high poly (an imported sculpt) so their subdivision matches it, keeping UVs. "
                         "Select the high poly first.",
                  .python = ops::toPython("transferShape", {{"args", {"high", "low"}}, {"method", "fit"}, {"levels", 2}}),
                  .command = "transferShape"});
}
} // namespace lila::ui
