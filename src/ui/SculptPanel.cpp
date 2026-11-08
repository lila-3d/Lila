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
#include "ui/Controls.h"
#include "ui/ReferenceOverlay.h"
#include "ui/Actions.h"
#include "ui/LocateFlash.h"
#include "ui/DockSidebar.h"
#include "ui/FileDialog.h"
#include "ui/FixedRowLayout.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/LayoutsUi.h"
#include "ui/Outliner.h"
#include "ui/ModelingToolkit.h"
#include "ui/SculptTool.h"
#include "ui/SpinScrub.h"
#include "ui/UiCommon.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToast.h"

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
#include <QMouseEvent>
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

/** In the panel's order: shape brushes, then the mask, freeze and paint layers. */
constexpr std::array<BrushInfo, std::size_t(sculpt::Brush::Count)> kBrushes = {{
    {sculpt::Brush::Draw, "brush", "Draw", "Push the surface out along its normal; Ctrl digs in."},
    {sculpt::Brush::ClayStrips, "layers-subtract", "Clay Strips", "Build up flat square strips of clay; Ctrl carves them."},
    {sculpt::Brush::Clay, "circle-dot", "Clay", "Build up rounded layers of clay; Ctrl carves them."},
    {sculpt::Brush::Inflate, "balloon", "Inflate", "Swell each point along its own normal; Ctrl deflates."},
    {sculpt::Brush::Blob, "circles", "Blob", "Swell a rounded ball; Ctrl sucks one in."},
    {sculpt::Brush::Layer, "stack", "Layer", "Raise a plateau to one height however often you pass; Ctrl sinks one."},
    {sculpt::Brush::Grab, "hand-grab", "Grab", "Drag a patch of surface with the cursor."},
    {sculpt::Brush::ElasticGrab, "hand-move", "Elastic Grab", "Grab with a soft, wide reach that keeps the form's volume."},
    {sculpt::Brush::SnakeHook, "route", "Snake Hook", "Pull the surface out after the cursor, thinning as it goes: horns, spikes, tendrils."},
    {sculpt::Brush::Smooth, "droplet", "Smooth", "Average out bumps and noise. Shift gives this brush for one stroke."},
    {sculpt::Brush::Relax, "wave-sine", "Relax", "Even out the point spacing, sliding along the surface so the form stays."},
    {sculpt::Brush::Flatten, "ironing", "Flatten", "Press toward the average plane under the brush; Ctrl raises contrast."},
    {sculpt::Brush::Planar, "box-align-bottom", "Planar", "Flatten onto the plane where the stroke began, for hard planes."},
    {sculpt::Brush::Fill, "bucket", "Fill", "Raise dips up to the average plane; Ctrl scrapes."},
    {sculpt::Brush::Scrape, "shovel", "Scrape", "Shave off what sticks out of the average plane; Ctrl fills dips."},
    {sculpt::Brush::Pinch, "arrows-minimize", "Pinch", "Pull points toward the brush centre to sharpen edges; Ctrl spreads them."},
    {sculpt::Brush::Magnify, "arrows-maximize", "Magnify", "Push points away from the brush centre; Ctrl pinches."},
    {sculpt::Brush::Crease, "chisel", "Crease", "Cut a sharp groove; Ctrl raises a sharp ridge."},
    {sculpt::Brush::DamStandard, "slash", "Dam Standard", "Cut a narrow V groove that pinches its edges in, for wrinkles and seams; Ctrl raises a sharp ridge."},
    {sculpt::Brush::TrimDynamic, "cut", "Trim Dynamic", "Cut flat to a plane just under the brush, for hard planes; Ctrl fills up to it."},
    {sculpt::Brush::Polish, "sparkles", "Polish", "Smooth along the surface's normal only and ease onto its plane: crisp, flat forms."},
    {sculpt::Brush::Fold, "ripple", "Fold", "Raise a cloth fold along the stroke, pinched from the sides; Ctrl presses a crease in."},
    {sculpt::Brush::Nudge, "hand-finger", "Nudge", "Push the surface along the stroke."},
    {sculpt::Brush::Thumb, "hand-click", "Thumb", "Push along the stroke and press flat, like a thumb through clay."},
    {sculpt::Brush::Rotate, "rotate-clockwise", "Rotate", "Twist the patch under the brush: drag out, then circle around where you clicked."},
    {sculpt::Brush::Pose, "walk", "Pose", "Click on a part with the brush covering it and drag: it bends at the ring where the brush meets the rest."},
    {sculpt::Brush::Boundary, "border-outer", "Boundary", "Drag the open borders under the brush and the points just inside them."},
    {sculpt::Brush::Mask, "mask", "Mask", "Paint the mask, which shields points from every brush; Ctrl erases it."},
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
    keepOwnColumn(dock);
    mw->addDockWidget(Qt::RightDockWidgetArea, dock);
    // Wide enough for five brush columns with their labels and the session rows; its own column opens at this width.
    dock->setProperty(kDefaultTabExtent, std::max(dock->sizeHint().width(), dock->fontMetrics().averageCharWidth() * 88));
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
    const scene::NodeId node = sculpt::active()->node;
    if (!runActionWithOptions("sculptExit", {{"method", st.exitMethod}, {"lowLevel", st.lowLevel}, {"high", st.highPoly}, {"highLevel", st.highLevel}}))
        return;
    if (sculptToolActive())
        viewportTools::set(nullptr);
    // The low poly alone can look like the sculpt was lost: say where the detail went.
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.node(node);
    if (const std::optional<sculpt::KeptSculpt> kept = n ? sculpt::keptSculpt(*n) : std::nullopt)
        if (scene::Node* k = g.node(kept->node))
        {
            viewportToast::showActive(QString("Sculpt kept in hidden %1. Sculpt %2 again to resume it.")
                                          .arg(QString::fromStdString(k->name()), QString::fromStdString(n->name())), 5000);
            // After the outliners have picked up the new node.
            QTimer::singleShot(0, [id = kept->node] { revealInOutliners({id}); });
        }
}

QToolButton* sculptActionButton(const std::string& id, const QString& iconName, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setAutoRaise(true);
    if (actions::find(id))
        b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    QObject::connect(b, &QToolButton::clicked, b, [id] { actions::run(json(id)); });
    return b;
}

controls::SliderField* sculptSlider(QWidget* parent, double lo, double hi, int decimals, const QString& suffix = {})
{
    auto* s = new controls::SliderField(parent);
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    if (!suffix.isEmpty())
        s->setSuffix(suffix);
    return s;
}

QFormLayout* sculptForm(QWidget* w)
{
    auto* form = new QFormLayout(w);
    form->setContentsMargins(0, 0, 0, 0);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    return form;
}

/** Proxy modes in segment order; a segment's id is its index. */
constexpr std::array<const char*, 3> kProxyModes = {"subdivide", "remesh", "direct"};

int proxyModeId(const std::string& mode)
{
    auto it = std::ranges::find_if(kProxyModes, [&](const char* m) { return mode == m; });
    return it == kProxyModes.end() ? 0 : int(it - kProxyModes.begin());
}

/** A middle click (no drag) on any segment picks segment `id`, the way a combo's default comes back. */
void setSegmentedDefault(controls::Segmented* seg, int id, const QString& defaultText)
{
    for (auto* b : seg->findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        onMiddleClickReset(
            b,
            [seg, id] {
                seg->setCurrent(id);
                Q_EMIT seg->picked(id);
            },
            defaultText, seg);
}
/** A label that runs a function when clicked. */
class SculptClickLabel : public QLabel
{
public:
    using QLabel::QLabel;
    std::function<void()> clicked;

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && clicked)
            clicked();
        QLabel::mousePressEvent(e);
    }
};

/** What Exit leaves of mesh `name`: the low poly and the high poly, as a noun phrase. */
QString exitSummary(const QString& name, sculpt::ProxyMode mode, int top, const SculptSettings& st)
{
    if (mode == sculpt::ProxyMode::Direct)
        return name + ", sculpted in place";
    if (mode == sculpt::ProxyMode::Subdivide && top == 0)
        return name + " as sculpted";
    QString low = st.exitMethod == "fit" ? "subdivision cage" : "game mesh";
    if (st.lowLevel > 0 && mode == sculpt::ProxyMode::Subdivide)
        low += QString(", level %1").arg(std::min(st.lowLevel, top));
    if (st.highPoly == "none")
        return QString("%1 (%2), sculpt discarded").arg(name, low);
    QString high = "voxel sculpt";
    if (mode == sculpt::ProxyMode::Subdivide)
    {
        const int level = st.highPoly == "belowHighest" ? top - st.highLevel : st.highPoly == "level" ? st.highLevel : top;
        high = QString("level %1").arg(std::clamp(level, 1, top));
    }
    return QString("%1 (%2) + hidden %1_high (%3)").arg(name, low, high);
}
} // namespace

SculptPanel::SculptPanel(QWidget* parent) : QWidget(parent)
{
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(12, 8, 12, 8);
    col->setSpacing(4);
    constexpr const char* kSections = "sculptPanel";

    const SculptSettings defaults;

    // Session: what to sculpt and the enter / exit buttons, always on top.
    auto* session = new QWidget(this);
    auto* sessionCol = new QVBoxLayout(session);
    sessionCol->setContentsMargins(0, 0, 0, 0);
    sessionCol->setSpacing(6);
    mStatus = new QLabel(session);
    mStatus->setWordWrap(true);
    mStatus->setObjectName("sculptStatus");
    sessionCol->addWidget(mStatus);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(2);
    mEnter = new QPushButton(icon("door-enter"), "Enter", session);
    mExit = new QPushButton(icon("door-exit"), "Exit", session);
    mPreview = new QPushButton(icon("eye"), "Preview", session);
    mPreview->setObjectName("sculptPreview");
    mPreview->setCheckable(true);
    mPreview->setToolTip("Preview Low Poly: show the low poly Exit would leave, isolated, before you commit. Again to sculpt on. Shift+V");
    mCancel = new QPushButton("Cancel", session);
    mEnter->setToolTip("Enter: sculpt the selected mesh.");
    mExit->setToolTip("Exit: move the mesh onto the sculpt and leave sculpt mode.");
    mCancel->setToolTip("Cancel: leave sculpt mode; the mesh stays as it was.");
    buttons->addWidget(mEnter);
    buttons->addWidget(mPreview);
    buttons->addWidget(mExit);
    buttons->addWidget(mCancel);
    sessionCol->addLayout(buttons);
    auto* sessionFields = new QWidget(session);
    auto* sform = sculptForm(sessionFields);
    mMode = new controls::Segmented(sessionFields);
    mMode->add("Levels", {}, "Subdivision Levels: a subdivided copy with levels to step between. Exit fits the mesh back to it exactly.", 0);
    mMode->add("Voxels", {}, "Voxel Remesh: an even remesh, free of the mesh's topology. Exit projects the mesh onto it.", 1);
    mMode->add("Mesh", {}, "Mesh Itself: the mesh's own points, no copy.", 2);
    mMode->setToolTip("What you sculpt on. The mesh keeps its topology and UVs whichever you pick.");
    setSegmentedDefault(mMode, proxyModeId(defaults.proxyMode), QStringList{"Levels", "Voxels", "Mesh"}[proxyModeId(defaults.proxyMode)]);
    sform->addRow("Sculpt On", mMode);
    mLevels = sculptSlider(sessionFields, 0, 6, 0);
    mLevels->setToolTip("Levels: subdivision levels to start with; each has four times the faces. 0 sculpts the mesh's own faces until you Add a level.");
    setSpinDefault(mLevels, defaults.levels);
    sform->addRow("Levels", mLevels);
    mLevelRow = new QWidget(sessionFields);
    mLevelRow->setObjectName("sculptLevelRow");
    {
        auto* row = new QHBoxLayout(mLevelRow);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        mLevelDown = sculptActionButton("sculpt.levelDown", "chevron-down", mLevelRow);
        mLevelUp = sculptActionButton("sculpt.levelUp", "chevron-up", mLevelRow);
        mLevelLabel = new QLabel(mLevelRow);
        mLevelLabel->setMinimumWidth(QFontMetrics(mLevelLabel->font()).horizontalAdvance("0 / 0  "));
        mLevelLabel->setAlignment(Qt::AlignCenter);
        mLevelLabel->setObjectName("sculptLevel");
        mAddLevel = controls::push("sculpt.addLevel", mLevelRow, "Add");
        mDeleteHigher = controls::push("sculpt.deleteHigher", mLevelRow, "Delete Higher");
        row->addWidget(mLevelDown);
        row->addWidget(mLevelLabel);
        row->addWidget(mLevelUp);
        row->addSpacing(6);
        row->addWidget(mAddLevel);
        row->addWidget(mDeleteHigher);
    }
    sform->addRow("Level", mLevelRow);
    mResolution = sculptSlider(sessionFields, 16, 2048, 0, " vox");
    mResolution->setToolTip("Resolution: voxels along the longest side of the remesh.");
    setSpinDefault(mResolution, defaults.resolution);
    sform->addRow("Resolution", mResolution);
    auto* summary = new SculptClickLabel(sessionFields);
    summary->setObjectName("sculptExitSummary");
    summary->setWordWrap(true);
    summary->setCursor(Qt::PointingHandCursor);
    summary->setToolTip("Exit Makes: the low poly Exit leaves, and the high poly it keeps hidden under it to bake from. Click to change.");
    summary->clicked = [this] {
        if (mExitHeader && !mExitHeader->isChecked())
            mExitHeader->click();
        flashLocate(mExitHeader);
    };
    mExitSummary = summary;
    sform->addRow("Exit Makes", mExitSummary);
    sessionCol->addWidget(sessionFields);
    col->addWidget(session);

    // Brushes
    mBrushBox = new QWidget(this);
    mBrushFlow = new FixedRowLayout(5, 2);
    mBrushFlow->setContentsMargins(0, 0, 0, 0);
    mBrushBox->setLayout(mBrushFlow);
    mBrushes = new QButtonGroup(this);
    mBrushes->setExclusive(true);
    for (const BrushInfo& b : kBrushes)
        mBrushes->addButton(brushButton(icon(b.icon), b.label, b.doc), int(b.brush));
    rebuildCustomBrushes();
    col->addWidget(controls::section("Brushes", mBrushBox, this, kSections));

    // Brush settings
    auto* settingsBox = new QWidget(this);
    auto* bform = sculptForm(settingsBox);
    mRadius = sculptSlider(settingsBox, 2.0, 2000.0, 0, " px");
    mRadius->setToolTip("Radius: brush radius on screen. Hold F or B and drag in the viewport to resize.");
    setSpinDefault(mRadius, defaults.stroke.radius);
    bform->addRow("Radius", mRadius);
    mStrength = sculptSlider(settingsBox, 0.0, 1.0, 2);
    mStrength->setSingleStep(0.05);
    mStrength->setToolTip("Strength: hold M (or Shift+F) and drag in the viewport, or hold B and drag with the right button, to change.");
    setSpinDefault(mStrength, defaults.stroke.strength);
    bform->addRow("Strength", mStrength);
    mFalloff = new QComboBox(settingsBox);
    for (const char* f : {"Smooth", "Sphere", "Root", "Sharp", "Linear", "Constant"})
        mFalloff->addItem(f);
    setComboDefault(mFalloff, int(sculpt::falloffFromName(defaults.stroke.falloff).value_or(sculpt::Falloff::Smooth)));
    bform->addRow("Falloff", mFalloff);
    mSpacing = sculptSlider(settingsBox, 0.02, 2.0, 2, " r");
    mSpacing->setSingleStep(0.05);
    mSpacing->setToolTip("Spacing: distance between dabs along a stroke, in brush radii.");
    setSpinDefault(mSpacing, defaults.stroke.spacing);
    bform->addRow("Spacing", mSpacing);
    mSymmetry = new controls::Segmented(settingsBox);
    // Segment ids are the mesh's symmetry values: off, x, y, z, radial.
    mSymmetry->add("Off", {}, "No symmetry", 0);
    mSymmetry->add("X", {}, "Mirror strokes across X", 1);
    mSymmetry->add("Y", {}, "Mirror strokes across Y", 2);
    mSymmetry->add("Z", {}, "Mirror strokes across Z", 3);
    mSymmetry->add("Radial", {}, "Repeat strokes round the mesh's radial axis", 4);
    mSymmetry->setToolTip("Symmetry: the mesh's symmetry setting. Strokes mirror exactly across it, or repeat exactly round its radial axis.");
    setSegmentedDefault(mSymmetry, 0, "Off");
    bform->addRow("Symmetry", mSymmetry);
    mFrontFaces = new QCheckBox("Front Faces Only", settingsBox);
    mFrontFaces->setToolTip("Front Faces Only: leave surfaces facing away from the view, so strokes don't reach through thin parts.");
    setCheckDefault(mFrontFaces, defaults.frontFacesOnly);
    bform->addRow("", mFrontFaces);
    auto* shareRow = new QHBoxLayout;
    const SculptSettings shareDefaults;
    mShareRadius = new QCheckBox("Radius", settingsBox);
    mShareStrength = new QCheckBox("Strength", settingsBox);
    mShareStroke = new QCheckBox("Stroke", settingsBox);
    mShareRadius->setToolTip("Share Radius: every brush uses the same radius; off, each brush keeps its own");
    mShareStrength->setToolTip("Share Strength: every brush uses the same strength; off, each brush keeps its own");
    mShareStroke->setToolTip("Share Stroke: every brush uses the same falloff, spacing, alpha, stabiliser and the rest of the Stroke group; off, "
                             "each brush keeps its own");
    setCheckDefault(mShareRadius, shareDefaults.shareRadius);
    setCheckDefault(mShareStrength, shareDefaults.shareStrength);
    setCheckDefault(mShareStroke, shareDefaults.shareStroke);
    shareRow->addWidget(mShareRadius);
    shareRow->addWidget(mShareStrength);
    shareRow->addWidget(mShareStroke);
    shareRow->addStretch(1);
    bform->addRow("Shared", shareRow);
    col->addWidget(controls::section("Brush", settingsBox, this, kSections));
    mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return sculptSettings().stroke; },
                                                  [] { sculptSettingsChanged(); });
    col->addWidget(mStrokeOptions);

    // Mask and freeze
    std::vector<QWidget*> maskButtons = {controls::push("sculpt.maskInvert", this, "Invert"), controls::push("sculpt.maskBlur", this, "Blur"),
                                         controls::push("sculpt.maskFill", this, "Fill"), controls::push("sculpt.maskClear", this, "Clear"),
                                         controls::push("sculpt.freezeClear", this, "Unfreeze All")};
    col->addWidget(controls::section("Mask and Freeze", controls::grid(maskButtons, 3, this), this, kSections));

    // Presets
    mPresetBox = new QWidget(this);
    auto* presetCol = new QVBoxLayout(mPresetBox);
    presetCol->setContentsMargins(0, 0, 0, 0);
    presetCol->setSpacing(4);
    mPresetFlow = new FlowLayout(2);
    presetCol->addLayout(mPresetFlow);
    mPresetEmpty = new QLabel("No presets. Click Save Preset, or drop .lilabrush files here.", mPresetBox);
    mPresetEmpty->setWordWrap(true);
    mPresetEmpty->setEnabled(false);
    presetCol->addWidget(mPresetEmpty);
    auto* presetRow = new QHBoxLayout;
    presetRow->setSpacing(2);
    auto* savePresetButton = new QToolButton(mPresetBox);
    savePresetButton->setText("Save Preset");
    savePresetButton->setToolTip("Save Preset: save the current brush and its settings, alpha included, as a preset");
    auto* importButton = new QToolButton(mPresetBox);
    importButton->setText("Import");
    importButton->setToolTip("Import Presets: install .lilabrush files (or drop them on this panel)");
    for (QToolButton* b : {savePresetButton, importButton})
    {
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        controls::setRole(b, "push");
        presetRow->addWidget(b);
    }
    presetCol->addLayout(presetRow);
    col->addWidget(controls::section("Presets", mPresetBox, this, kSections));
    connect(savePresetButton, &QToolButton::clicked, this, [this] { savePreset(); });
    connect(importButton, &QToolButton::clicked, this, [this] { importPresets(); });
    setAcceptDrops(true);
    mPresetWatcher = new QFileSystemWatcher(this);
    connect(mPresetWatcher, &QFileSystemWatcher::directoryChanged, this, [] { brush::presets::notifyChanged(); });
    rebuildPresets();

    // Polypaint
    auto* paintBox = new QWidget(this);
    auto* pform = sculptForm(paintBox);
    mColor = new QToolButton(paintBox);
    mColor->setMinimumWidth(60);
    mColor->setProperty("lilaRawColors", true);
    mColor->setToolTip("Polypaint Color: the color the Paint brush, and shape brushes with Paint While Sculpting, lay down");
    auto* colorRow = new QHBoxLayout;
    colorRow->setSpacing(2);
    colorRow->addWidget(mColor);
    pform->addRow("Color", colorRow);
    mPaint = new QCheckBox("Paint While Sculpting", paintBox);
    mPaint->setToolTip("Paint While Sculpting: shape brushes also paint the color wherever they move the surface");
    setCheckDefault(mPaint, SculptSettings{}.paint);
    pform->addRow("", mPaint);
    pform->addRow("", controls::grid({controls::push("sculpt.colorFill", paintBox, "Fill"), controls::push("sculpt.colorClear", paintBox, "Clear")}, 2,
                                     paintBox));
    col->addWidget(controls::section("Polypaint", paintBox, this, kSections));

    // Display
    auto* displayBox = new QWidget(this);
    auto* dform = sculptForm(displayBox);
    mMaterial = new QComboBox(displayBox);
    for (const auto& [id, label] : sculptMaterials())
        mMaterial->addItem(QString::fromStdString(label), QString::fromStdString(id));
    mMaterial->setToolTip("Material: what the sculpt is drawn with while sculpting; the mesh's own comes back on exit");
    setComboDefault(mMaterial, std::max(0, mMaterial->findData(QString::fromStdString(SculptSettings{}.material))));
    dform->addRow("Material", mMaterial);
    mWireframe = new QCheckBox("Wireframe", displayBox);
    mWireframe->setToolTip("Wireframe: the sculpt mesh's wireframe (Shift+F in the viewport)");
    setCheckDefault(mWireframe, SculptSettings{}.wireframe);
    dform->addRow("", mWireframe);
    mWireOpacity = sculptSlider(displayBox, 5, 100, 0, " %");
    mWireOpacity->setToolTip("Wire Opacity: how strongly the wireframe draws over the sculpt");
    setSpinDefault(mWireOpacity, SculptSettings{}.wireOpacity * 100.0);
    dform->addRow("Wire Opacity", mWireOpacity);
    mSeeThrough = new QCheckBox("See-Through", displayBox);
    mSeeThrough->setToolTip("See-Through: image planes behind the sculpt show through (Alt+X in the viewport)");
    setCheckDefault(mSeeThrough, SculptSettings{}.seeThrough);
    dform->addRow("", mSeeThrough);
    mOpacity = sculptSlider(displayBox, 0, 100, 0, " %");
    mOpacity->setToolTip("Opacity: the sculpt surface's opacity while see-through");
    setSpinDefault(mOpacity, SculptSettings{}.seeThroughOpacity * 100.0);
    dform->addRow("Opacity", mOpacity);
    mShowCage = new QCheckBox("Show Cage", displayBox);
    mShowCage->setToolTip("Show Cage: draw the low poly over the sculpt as Exit will leave it, refitted as you sculpt.");
    setCheckDefault(mShowCage, defaults.showCage);
    dform->addRow("", mShowCage);
    mSymmetryLine = new QCheckBox("Symmetry Line", displayBox);
    mSymmetryLine->setToolTip("Symmetry Line: the red line along the mesh's mirror plane.");
    setCheckDefault(mSymmetryLine, defaults.symmetryLine);
    dform->addRow("", mSymmetryLine);
    col->addWidget(controls::section("Display", displayBox, this, kSections));

    // What Exit leaves: set once per asset, so closed by default.
    auto* exitBox = new QWidget(this);
    auto* eform = sculptForm(exitBox);
    mExitMethod = new QComboBox(exitBox);
    mExitMethod->addItem("Game Mesh", "closest");
    mExitMethod->addItem("Game Mesh Along Normals", "normal");
    mExitMethod->addItem("Subdivision Cage", "fit");
    mExitMethod->setItemData(0, "Each point moves to the nearest spot on the sculpt, so the low poly lies on it. Bake normals from the "
                                "high poly onto it.", Qt::ToolTipRole);
    mExitMethod->setItemData(1, "Each point moves straight in or out along its normal onto the sculpt. Try it when Game Mesh slides "
                                "points sideways into a crease.", Qt::ToolTipRole);
    mExitMethod->setItemData(2, "Points float off the sculpt so that subdividing the low poly gives the sculpt back. For a mesh you "
                                "subdivide afterwards, not one you bake.", Qt::ToolTipRole);
    mExitMethod->setToolTip("Low Poly: what Exit makes of the mesh you entered with. It keeps its topology and UVs either way. Show Cage previews it.");
    setComboDefault(mExitMethod, mExitMethod->findData(QString::fromStdString(defaults.exitMethod)));
    eform->addRow("Low Poly", mExitMethod);
    mLowLevel = sculptSlider(exitBox, 0, 6, 0);
    mLowLevel->setToolTip("Low Poly Level: the level the low poly comes out at. Level 0 keeps the mesh's topology; each level above subdivides "
                          "it once more, UVs included.");
    setSpinDefault(mLowLevel, defaults.lowLevel);
    eform->addRow("Low Level", mLowLevel);
    mHighPoly = new QComboBox(exitBox);
    mHighPoly->addItem("Highest Level", "highest");
    mHighPoly->addItem("Below Highest", "belowHighest");
    mHighPoly->addItem("Level", "level");
    mHighPoly->addItem("Don't Keep", "none");
    mHighPoly->setItemData(1, "Keeps a level some steps below the highest: a lighter high poly to bake from.", Qt::ToolTipRole);
    mHighPoly->setItemData(3, "Keeps nothing: the sculpt and its levels are gone after Exit.", Qt::ToolTipRole);
    mHighPoly->setToolTip("High Poly: Exit keeps the sculpt as the hidden child <mesh>_high, at this level, to bake from. Every level "
                          "comes back the next time you sculpt the mesh.");
    setComboDefault(mHighPoly, mHighPoly->findData(QString::fromStdString(defaults.highPoly)));
    eform->addRow("High Poly", mHighPoly);
    mHighLevel = sculptSlider(exitBox, 0, 6, 0);
    setSpinDefault(mHighLevel, defaults.highLevel);
    eform->addRow("High Level", mHighLevel);
    // Right under the session, since it decides what the sculpt turns into.
    col->insertWidget(col->indexOf(session) + 1, controls::section("On Exit", exitBox, this, kSections, false, &mExitHeader));

    col->addWidget(referenceOverlays::createSculptSection(this));
    col->addWidget(controls::overflow({"sculpt.duplicateLowPoly", "mesh.relaxKeepForm", "mesh.reproject", "mesh.transferShape"}, this));
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
    connect(mWireOpacity, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().wireOpacity = v / 100.0; changed(); });
    connect(mSeeThrough, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().seeThrough = on; changed(); });
    connect(mOpacity, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().seeThroughOpacity = v / 100.0; changed(); });
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
    connect(mMode, &controls::Segmented::picked, this, [changed](int id) {
        sculptSettings().proxyMode = kProxyModes[size_t(std::clamp(id, 0, int(kProxyModes.size()) - 1))];
        changed();
    });
    connect(mLevels, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().levels = int(std::lround(v)); changed(); });
    connect(mResolution, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().resolution = int(std::lround(v)); changed(); });
    connect(mExitMethod, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        sculptSettings().exitMethod = mExitMethod->currentData().toString().toStdString();
        changed();
    });
    connect(mHighPoly, &QComboBox::currentIndexChanged, this, [this, changed](int) {
        sculptSettings().highPoly = mHighPoly->currentData().toString().toStdString();
        changed();
    });
    connect(mHighLevel, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().highLevel = int(std::lround(v)); changed(); });
    connect(mLowLevel, &QDoubleSpinBox::valueChanged, this, [changed](double v) { sculptSettings().lowLevel = int(std::lround(v)); changed(); });
    connect(mSymmetryLine, &QCheckBox::toggled, this, [changed](bool on) { sculptSettings().symmetryLine = on; changed(); });
    connect(mSymmetry, &controls::Segmented::picked, this, [](int id) {
        static constexpr std::array<const char*, 5> kAxes = {"off", "x", "y", "z", "radial"};
        std::vector<std::string> targets;
        if (auto s = sculpt::active(); s && scene::Graph::current().node(s->node))
            targets.push_back(scene::Graph::current().node(s->node)->path());
        else
            targets = selectedMeshes();
        if (!targets.empty())
            runActionWithOptions("setSymmetry", {{"args", targets}, {"axis", kAxes[size_t(std::clamp(id, 0, 4))]}});
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
    btn->setIconSize(QSize(24, 24));
    btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    btn->setFocusPolicy(Qt::NoFocus);
    controls::setRole(btn, "tile");
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
    mPresetEmpty->setVisible(mPresets.empty());

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
                                                           : QString(std::ranges::find(kBrushes, s.brush, &BrushInfo::brush)->label);
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
        b16(mWireframe), b17(mMaterial), b18(mWireOpacity), b22(mSeeThrough), b23(mOpacity);
    mShareRadius->setChecked(s.shareRadius);
    mShareStrength->setChecked(s.shareStrength);
    mShareStroke->setChecked(s.shareStroke);
    mWireframe->setChecked(s.wireframe);
    mWireOpacity->setValue(std::round(s.wireOpacity * 100.0));
    mWireOpacity->setEnabled(s.wireframe);
    mSeeThrough->setChecked(s.seeThrough);
    mOpacity->setValue(std::round(s.seeThroughOpacity * 100.0));
    mOpacity->setEnabled(s.seeThrough);
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
    mColor->setStyleSheet(QString("QToolButton { background: %1; border: none; border-radius: 3px; min-height: 18px; }").arg(swatchColor(s.color).name()));
    mRadius->setValue(s.stroke.radius);
    mStrength->setValue(s.stroke.strength);
    mSpacing->setValue(s.stroke.spacing);
    mFalloff->setCurrentIndex(int(sculpt::falloffFromName(s.stroke.falloff).value_or(sculpt::Falloff::Smooth)));
    if (mStrokeOptions)
        mStrokeOptions->sync();
    mFrontFaces->setChecked(s.frontFacesOnly);
    mShowCage->setChecked(s.showCage);
    mMode->setCurrent(proxyModeId(s.proxyMode));
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
    const std::string& mode = sculptSettings().proxyMode;
    const std::string& high = sculptSettings().highPoly;
    if (auto* form = qobject_cast<QFormLayout*>(mHighLevel->parentWidget()->layout()))
        form->setRowVisible(mHighLevel, high == "belowHighest" || high == "level");
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
    // How a sculpt starts is fixed once it has: the start rows give way to the level row while it runs.
    if (auto* form = qobject_cast<QFormLayout*>(mLevelRow->parentWidget()->layout()))
    {
        form->setRowVisible(mMode, !n);
        form->setRowVisible(mLevels, !n && !resuming && mode == "subdivide");
        form->setRowVisible(mResolution, !n && mode == "remesh");
        form->setRowVisible(mLevelRow, levels);
        scene::Node* target = n ? n : [&]() -> scene::Node* {
            auto meshes = selectedMeshes();
            return meshes.empty() ? nullptr : g.find(meshes.back());
        }();
        form->setRowVisible(mExitSummary, target != nullptr);
        if (target)
        {
            const sculpt::ProxyMode pm = n ? s->mode : resuming ? (resumingLevels > 0 ? sculpt::ProxyMode::Subdivide : sculpt::ProxyMode::Remesh)
                                         : mode == "remesh" ? sculpt::ProxyMode::Remesh : mode == "direct" ? sculpt::ProxyMode::Direct
                                                            : sculpt::ProxyMode::Subdivide;
            mExitSummary->setText(exitSummary(QString::fromStdString(target->name()), pm, top, sculptSettings()));
        }
    }
    if (levels)
    {
        mLevelLabel->setText(QString("%1 / %2").arg(s->level).arg(s->stack->top()));
        mLevelDown->setEnabled(s->level > 0);
        mLevelUp->setEnabled(s->level < s->stack->top());
        mAddLevel->setEnabled(s->stack->top() < 6);
        mDeleteHigher->setEnabled(s->level < s->stack->top());
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
    mSymmetry->setCurrent(sym ? int(std::clamp<int64_t>(sym->as<int64_t>(scene::MeshNode::kSymmetry), 0, scene::kSymmetryRadial)) : 0);
}

void registerSculptUi()
{
    installSculptDrawer();
    addLazyDock(kSculptDock, [] { return sculptDock(true); });
    // Leaving sculpt mode, however it is left, closes the panel entering opened.
    sculpt::addListener([] {
        QTimer::singleShot(0, QCoreApplication::instance(), [] {
            static bool wasActive = false;
            const bool active = sculpt::active() != nullptr;
            if (QDockWidget* d = sculptDock(false); wasActive && !active && d && d->isVisible())
                d->close();
            wasActive = active;
        });
    });

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
                  .python = ops::toPython("sculptEnter", {{"mode", "subdivide"}})});
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
    actions::add({.id = "sculpt.toggleSeeThrough",
                  .label = "See-Through Sculpt",
                  .category = "Sculpt",
                  .run =
                      [] {
                          bool& on = sculptSettings().seeThrough;
                          on = !on;
                          sculptSettingsChanged();
                          viewportToast::showActive(on ? "See-through on" : "See-through off", 1000);
                      },
                  .doc = "Draw the sculpt at the Display opacity so image planes behind it show through (Alt+X while sculpting).",
                  .python = "lila.ui.run_action(\"sculpt.toggleSeeThrough\")",
                  .checked = [] { return sculptSettings().seeThrough; }});
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
