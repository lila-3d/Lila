#include "ui/ViewportToolbar.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "scene/ComponentPick.h"
#include "scene/Graph.h"
#include "scene/Components.h"
#include "scene/MeshNodes.h"
#include "scene/RenderSettings.h"
#include "ui/Actions.h"
#include "ui/DisplayAids.h"
#include "ui/Environments.h"
#include "ui/GizmoMath.h"
#include "ui/Icons.h"
#include "ui/ImagePlaneUi.h"
#include "ui/LodDisplay.h"
#include "ui/MaterialDraw.h"
#include "ui/OptionBox.h"
#include "ui/PaintSelect.h"
#include "ui/RetopoDrawTool.h"
#include "ui/SpinScrub.h"
#include "ui/Tools.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHud.h"
#include "ui/ViewportWidget.h"

#include <QActionGroup>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <utility>
#include <string>

namespace lila::ui
{
namespace
{
namespace vd = viewportDisplay;

constexpr int kHeight = 24;
constexpr int kIcon = 16;
constexpr int kSpacing = 1;
constexpr const char* kViews[] = {"persp", "top", "front", "right", "left", "back", "bottom"};

bool pref(const char* key, bool fallback)
{
    return PrefStore::get().value<bool>("general", key, fallback);
}

QString labelOf(const std::string& id)
{
    const Action* a = actions::find(id);
    return QString::fromStdString(a ? a->label : id);
}

QString tipFor(const std::string& id)
{
    return QString::fromStdString(actions::tooltip(id));
}

std::string snapPref(Tool t, const char* leaf)
{
    return std::string("/manipulator/snap/") + tools::snapKey(t) + "/" + leaf;
}

QString stepText(double v)
{
    return QString::number(v, 'g', 6);
}

/**
 * The rotate increments as two columns in one menu entry: common angles, and divisions of a turn (360/n). Checkable
 * flat buttons stand in for menu items, and a click closes the menu.
 */
QWidgetAction* rotateStepColumns(QMenu* menu, double current, const std::string& stepKey, bool& matched)
{
    auto* holder = new QWidget(menu);
    auto* grid = new QGridLayout(holder);
    grid->setContentsMargins(4, 2, 4, 2);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(0);
    auto header = [&](const QString& text, int col) {
        auto* l = new QLabel(text, holder);
        l->setStyleSheet("color: #8c8c8c; padding: 2px 6px;");
        grid->addWidget(l, 0, col);
    };
    header("Angle", 0);
    header("Divisions of 360", 1);
    auto add = [&](double v, const QString& text, int row, int col) {
        auto* b = new QToolButton(holder);
        b->setText(text);
        b->setCheckable(true);
        bool on = std::abs(v - current) < 1e-9;
        matched = matched || on;
        b->setChecked(on);
        b->setAutoRaise(true);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        b->setStyleSheet("QToolButton { text-align: left; padding: 3px 6px; border: none; }"
                         "QToolButton:hover { background: #4a4a4a; } QToolButton:checked { background: #5c5c5c; }");
        QObject::connect(b, &QToolButton::clicked, menu, [menu, stepKey, v] {
            PrefStore::get().set("general", stepKey, v);
            menu->close();
        });
        grid->addWidget(b, row, col);
    };
    const QString deg(QChar(0x00B0));
    int row = 1;
    for (double v : gizmo::kRotateSteps)
        add(v, stepText(v) + deg, row++, 0);
    row = 1;
    for (int n : gizmo::kTurnDivisions)
    {
        double v = gizmo::turnDivision(n);
        add(v, stepText(v) + deg + "  (360/" + QString::number(n) + ")", row++, 1);
    }
    grid->setRowStretch(row, 1);
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(holder);
    return action;
}
/** The select, transform and selection tools, and the component modes: which one is in use. */
std::optional<bool> toolActionState(const std::string& id)
{
    const bool viewportTool = viewportTools::active() != nullptr;
    for (Tool t : {Tool::Select, Tool::Move, Tool::Rotate, Tool::Scale})
        if (id == std::string("tool.") + tools::name(t))
            return !viewportTool && tools::current() == t;
    if (id == "select.marqueeTool" || id == "select.lassoTool")
        return !viewportTool && tools::current() == Tool::Select && pref(paintSelect::kLassoPref, false) == (id == "select.lassoTool");
    if (id == "select.paintTool")
        return viewportTools::activeId() == paintSelect::kToolId;
    using scene::ComponentMode;
    for (auto [mode, action] : {std::pair{ComponentMode::Object, "select.objectMode"}, {ComponentMode::Vertex, "select.vertexMode"},
                                {ComponentMode::Edge, "select.edgeMode"}, {ComponentMode::Face, "select.faceMode"}})
        if (id == action)
            return scene::components::mode() == mode;
    return std::nullopt;
}
} // namespace

std::optional<bool> viewportActionState(const std::string& id, const ViewportWidget* view)
{
    // Wireframe, Shaded and Textured are one choice, as in Maya.
    const bool wire = PrefStore::get().value<std::string>("general", "/viewport/displayMode", "shaded") == "wireframe";
    if (id == "viewport.wireframe")
        return wire;
    if (id == "viewport.shaded")
        return !wire && !materialsShown();
    if (id == "viewport.textured")
        return !wire && materialsShown();
    if (id == "rig.xrayJoints")
        return pref("/rig/xrayJoints", false);
    if (id == "retopo.toggleSnapSpace")
        return RetopoDrawTool::snapObjectSpace();
    if (std::optional<bool> tool = toolActionState(id))
        return tool;
    struct Toggle
    {
        const char* id;
        const char* key;
        bool fallback;
    };
    static const Toggle toggles[] = {
        {"viewport.wireOnShaded", "/viewport/wireOnShaded", false},
        {"viewport.toggleAO", "/viewport/ao/enabled", true},
        {"viewport.toggleAA", vd::kAaEnabled, true},
        {"viewport.toggleGrid", vd::kGrid, true},
        {"viewport.toggleXray", vd::kXray, false},
        {"viewport.toggleBackfaceCulling", vd::kBackfaceCulling, false},
        {"viewport.toggleKuwahara", vd::kKuwahara, false},
        {"viewport.toggleOutline", vd::kOutline, false},
        {"viewport.toggleShadows", vd::kShadows, true},
        {"viewport.defaultMaterial", vd::kDefaultMaterial, false},
        {"viewport.toggleEnvBackground", "/viewport/env/background", true},
        {"viewport.toggleToolbar", vd::kToolbar, true},
        {"viewport.grayscaleWeights", vd::kGrayscaleWeights, false},
        {"viewport.monotone", vd::kMonotone, false},
        {"viewport.togglePreselectObjects", "/viewport/preselect/objects", false},
        {"window.toggleToolBox", "/ui/showToolBox", true},
        {"softSelect.toggle", "/softSelect/enabled", false},
        {"measure.toggleShow", "/measure/show", true},
        {"select.toggleCameraBased", "/select/cameraBased", false},
        {"select.toggleBackfaceCulling", "/select/backfaceCulling", false},
        {"select.toggleLasso", "/select/lasso", false},
    };
    for (const Toggle& t : toggles)
        if (id == t.id)
            return pref(t.key, t.fallback);
    if (id.starts_with("select.mask."))
        for (scene::selectionMask::Kind k : scene::selectionMask::kAll)
            if (id.substr(12) == scene::selectionMask::name(k))
                return scene::selectionMask::enabled(k);
    if (id.starts_with("viewport.show."))
        for (scene::selectionMask::Kind k : scene::selectionMask::kAll)
            if (id.substr(14) == scene::selectionMask::name(k))
                return scene::selectionMask::shown(k);
    if (id.starts_with("viewport.aaSamples"))
        return pref(vd::kAaEnabled, true) && PrefStore::get().value<int>("general", vd::kAaSamples, vd::kDefaultSamples) == std::stoi(id.substr(18));
    if (id == "viewport.toggleVertexColors")
        return PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") != "off";
    if (id.starts_with("viewport.vertexColors."))
        return PrefStore::get().value<std::string>("general", vd::kVertexColors, "off") == id.substr(22);
    if (id.starts_with("viewport.colorChannel."))
        return PrefStore::get().value<std::string>("general", vd::kColorChannel, "rgb") == id.substr(22);
    if (id.starts_with("viewport.lighting."))
        return PrefStore::get().value<std::string>("general", vd::kLighting, "environment") == id.substr(18);
    if (view && id.starts_with("viewport.aid."))
        return displayAids::actionState(id, view);
    if (const Action* a = actions::find(id); a && a->checked)
        return a->checked();
    if (!view)
        return std::nullopt;
    if (std::optional<bool> hudState = hud::actionState(id, view))
        return hudState;
    if (id == "viewport.isolate")
        return view->isolating();
    if (id == "viewport.toggleImagePlanes")
        return imagePlanes::shownIn(view->viewName());
    if (id.starts_with("viewport.view."))
        return view->viewName() == id.substr(14);
    if (id == "viewport.toggleResolutionGate" || id == "viewport.toggleFilmGate")
    {
        scene::CameraSettings c = scene::renderSettings().camera(view->viewName());
        return id == "viewport.toggleFilmGate" ? c.filmGate : c.resolutionGate;
    }
    return std::nullopt;
}

namespace
{
/** Intensity of the current HDRI as a slider (0..4) and a field (0..10), remembered per HDRI; Reset follows the global pref. */
QWidgetAction* environmentIntensityRow(QMenu* menu)
{
    constexpr double kSliderMax = 4.0;
    constexpr int kSliderSteps = 400;
    auto* holder = new QWidget(menu);
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);
    row->addWidget(new QLabel("Intensity", holder));
    auto* slider = new QSlider(Qt::Horizontal, holder);
    slider->setRange(0, kSliderSteps);
    slider->setMinimumWidth(120);
    auto* spin = new QDoubleSpinBox(holder);
    spin->setRange(0.0, 10.0);
    spin->setSingleStep(0.05);
    spin->setDecimals(2);
    spin->setKeyboardTracking(false);
    spin->setFixedWidth(52);
    setSpinDefault(spin, 1.0);
    auto* reset = new QToolButton(holder);
    reset->setText("Reset");
    reset->setAutoRaise(true);
    reset->setToolTip("Use the default intensity from Preferences for this HDRI");
    row->addWidget(slider, 1);
    row->addWidget(spin);
    row->addWidget(reset);

    auto show = [slider, spin, reset] {
        double v = environments::intensity();
        QSignalBlocker a(slider), b(spin);
        slider->setValue(int(std::lround(std::min(v, kSliderMax) / kSliderMax * kSliderSteps)));
        spin->setValue(v);
        reset->setEnabled(environments::hasOwnIntensity());
    };
    show();
    QObject::connect(slider, &QSlider::valueChanged, holder, [show](int i) {
        environments::setIntensity(float(i * kSliderMax / kSliderSteps));
        show();
    });
    QObject::connect(spin, &QDoubleSpinBox::valueChanged, holder, [show](double v) {
        environments::setIntensity(float(v));
        show();
    });
    QObject::connect(reset, &QToolButton::clicked, holder, [show] {
        environments::setIntensity(std::nullopt);
        show();
    });
    auto* action = new QWidgetAction(menu);
    action->setDefaultWidget(holder);
    return action;
}
} // namespace

void fillCameraMenu(QMenu* menu, ViewportWidget* view, std::function<void()> activate)
{
    menu->clear();
    if (!view)
        return;
    QPointer<ViewportWidget> target(view);
    auto pick = [menu, activate](QAction* a, std::function<void()> fn) {
        QObject::connect(a, &QAction::triggered, menu, [activate, fn] {
            if (activate)
                activate();
            fn();
        });
    };
    for (const char* v : kViews)
    {
        std::string id = std::string("viewport.view.") + v;
        QAction* a = menu->addAction(labelOf(id));
        a->setCheckable(true);
        a->setChecked(view->viewName() == v && view->lookThroughCamera() == scene::kNoNode);
        pick(a, [id] { actions::run(id); });
    }
    scene::Graph& g = scene::Graph::current();
    bool first = true;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n->type().isA("camera"))
            continue;
        if (std::exchange(first, false))
            menu->addSeparator();
        QAction* a = menu->addAction(icon("camera"), QString::fromStdString(n->name()));
        a->setToolTip(QString::fromStdString(n->fullPath()));
        a->setCheckable(true);
        a->setChecked(view->lookThroughCamera() == id);
        pick(a, [target, id] {
            if (target)
                target->lookThrough(id);
        });
    }
    menu->addSeparator();
    for (const char* id : {"viewport.lookThroughSelected", "create.cameraFromView"})
    {
        QAction* a = menu->addAction(labelOf(id));
        a->setEnabled(actions::isEnabled(nlohmann::json(id)));
        pick(a, [id = std::string(id)] { actions::run(id); });
    }
}

void fillEnvironmentMenu(QMenu* menu)
{
    menu->clear();
    std::string resolved = environments::resolve(PrefStore::get().value<std::string>("general", "/viewport/env/path", ""));
    auto item = [&](const QString& label, const std::string& value, bool checked) {
        QAction* a = menu->addAction(label);
        a->setCheckable(true);
        a->setChecked(checked);
        QObject::connect(a, &QAction::triggered, menu, [value] { PrefStore::get().set("general", "/viewport/env/path", value); });
    };
    for (const std::filesystem::path& hdri : environments::builtIn())
    {
        std::u8string name = hdri.filename().u8string();
        std::string value(name.begin(), name.end());
        item(QString::fromStdWString(hdri.stem().wstring()), value, !resolved.empty() && environments::resolve(value) == resolved);
    }
    item("Procedural Studio", environments::kProcedural, resolved.empty());
    menu->addSeparator();
    QObject::connect(menu->addAction("Browse..."), &QAction::triggered, menu, [menu] {
        QString f = QFileDialog::getOpenFileName(menu->parentWidget(), "Choose Environment", QString(), "Radiance HDR (*.hdr)");
        if (!f.isEmpty())
            PrefStore::get().set("general", "/viewport/env/path", f.toStdString());
    });
    menu->addSeparator();
    menu->addAction(environmentIntensityRow(menu));
}

ViewportToolbar::ViewportToolbar(ViewportWidget* view, QWidget* parent) : QWidget(parent), mView(view)
{
    setObjectName("viewportToolbar");
    setAttribute(Qt::WA_StyledBackground);
    setFixedHeight(kHeight);
    setStyleSheet(R"(
        #viewportToolbar { background: #383838; border-bottom: 1px solid #262626; }
        #viewportToolbar QToolButton { border: none; border-radius: 2px; padding: 2px; }
        #viewportToolbar QToolButton:hover { background: #4a4a4a; }
        #viewportToolbar QToolButton:checked { background: #5c5c5c; }
        #viewportToolbar QToolButton[partial="true"] { background: #444444; border: 1px solid #5c5c5c; }
        #viewportToolbar QToolButton::menu-indicator { image: none; width: 0; }
        #viewportToolbar QDoubleSpinBox { background: #2a2a2a; border: 1px solid #262626; padding: 0 2px; }
        #viewportToolbar QFrame#toolbarSeparator { color: #4c4c4c; }
    )");

    mCameraButton = menuButton("camera", "Camera", [this](QMenu* m) { fillCameraMenu(m, mView, [this] { activateView(); }); });
    // Right-click selects the camera node this view looks through, as Maya's panel camera button does.
    mCameraButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mCameraButton, &QWidget::customContextMenuRequested, this, [this] {
        const scene::Node* n = scene::Graph::current().node(mView->lookThroughCamera());
        if (!n)
        {
            log::info("{} is a built-in view; there is no camera node to select.", mView->viewName());
            return;
        }
        try
        {
            UndoChunk chunk("Select Camera");
            ops::execute("selectMode", {{"args", {"object"}}}, CallSource::UI);
            ops::execute("select", {{"args", {n->path()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    });
    actionButton("viewport.cameraSettings", "camera-cog");
    separator();
    actionButton("viewport.toggleGrid", "grid-4x4");
    actionButton("viewport.toggleFilmGate", "frame");
    actionButton("viewport.toggleResolutionGate", "aspect-ratio");
    // Click toggles this view's image planes; a long press opens the camera's image plane menu.
    QToolButton* planes = actionButton("viewport.toggleImagePlanes", "photo");
    auto* planeMenu = new QMenu(planes);
    connect(planeMenu, &QMenu::aboutToShow, this, [this, planeMenu] {
        activateView();
        planeMenu->clear();
        imagePlanes::fillCameraMenu(planeMenu, mView->viewName());
    });
    planes->setMenu(planeMenu);
    planes->setPopupMode(QToolButton::DelayedPopup);
    QToolButton* hudButton = menuButton("device-desktop-analytics", "Heads Up Display", [this](QMenu* m) { hud::fillMenu(m, mView); });
    // Decorated after the fill above is connected, so the option box squares go onto the rows it just made.
    optionBox::decorateMenu(hudButton->menu());
    separator();
    actionButton("viewport.wireframe", "cube");
    actionButton("viewport.shaded", "sphere");
    actionButton("viewport.textured", "texture");
    actionButton("viewport.wireOnShaded", "box-model");
    actionButton("viewport.defaultMaterial", "ball-bowling");
    menuButton("palette", "Vertex Colors", [this](QMenu* m) {
        for (const char* group : {"vertexColors", "colorChannel"})
        {
            auto* exclusive = new QActionGroup(m);
            for (const char* v : std::string(group) == "vertexColors" ? std::initializer_list<const char*>{"off", "shaded", "unlit"}
                                                                       : std::initializer_list<const char*>{"rgb", "r", "g", "b", "a"})
            {
                std::string id = std::string("viewport.") + group + "." + v;
                QAction* a = m->addAction(labelOf(id));
                a->setCheckable(true);
                a->setChecked(viewportActionState(id, mView).value_or(false));
                exclusive->addAction(a);
                connect(a, &QAction::triggered, this, [id] { actions::run(id); });
            }
            m->addSeparator();
        }
        QAction* gray = m->addAction(labelOf("viewport.grayscaleWeights"));
        gray->setCheckable(true);
        gray->setChecked(viewportActionState("viewport.grayscaleWeights", mView).value_or(false));
        connect(gray, &QAction::triggered, this, [] { actions::run("viewport.grayscaleWeights"); });
    });
    actionButton("viewport.monotone", "palette");
    separator();
    menuButton("bulb", "Lighting", [this](QMenu* m) {
        auto* group = new QActionGroup(m);
        for (const char* mode : {"environment", "studio", "flat"})
        {
            std::string id = std::string("viewport.lighting.") + mode;
            QAction* a = m->addAction(labelOf(id));
            a->setCheckable(true);
            a->setChecked(viewportActionState(id, mView).value_or(false));
            group->addAction(a);
            connect(a, &QAction::triggered, this, [id] { actions::run(id); });
        }
    });
    actionButton("viewport.toggleShadows", "shadow");
    actionButton("viewport.toggleAO", "blur");
    actionButton("viewport.toggleAA", "wave-saw-tool");
    mSamplesButton = menuButton({}, "Antialiasing Samples", [this](QMenu* m) {
        auto* group = new QActionGroup(m);
        for (const char* n : {"2", "4", "8"})
        {
            std::string id = std::string("viewport.aaSamples") + n;
            QAction* a = m->addAction(QString(n) + "x");
            a->setCheckable(true);
            a->setChecked(PrefStore::get().value<int>("general", vd::kAaSamples, vd::kDefaultSamples) == std::stoi(n));
            group->addAction(a);
            connect(a, &QAction::triggered, this, [id] { actions::run(id); });
        }
    });
    mSamplesButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    separator();
    actionButton("viewport.toggleEnvBackground", "background");
    menuButton("world", "Environment", [](QMenu* m) { fillEnvironmentMenu(m); });
    separator();
    actionButton("viewport.isolate", "focus-2");
    actionButton("viewport.show.mesh", "box");
    actionButton("viewport.show.joint", "bone");
    actionButton("viewport.hud.originAxis", "axis-y");
    actionButton("viewport.toggleXray", "ghost");
    actionButton("rig.xrayJoints", "skull");
    actionButton("viewport.toggleBackfaceCulling", "square-toggle");
    actionButton("viewport.toggleKuwahara", "brush");
    actionButton("viewport.toggleOutline", "border-outer");
    menuButton("eye", "Mesh Display", [this](QMenu* m) { displayAids::fillMenu(m, mView); });
    menuButton("stack-2", "Level of Detail", [this](QMenu* m) { lodDisplay::fillMenu(m, mView); });
    separator();
    actionButton("viewport.frameSelected", "focus-centered");
    actionButton("viewport.frameAll", "arrows-maximize");
    separator();
    actionButton("viewport.resetExposure", "exposure");
    field(vd::kExposure, 0.0, -10.0, 10.0, 0.1, "Exposure (stops)");
    field(vd::kGamma, 1.0, 0.1, 4.0, 0.05, "Gamma");

    mSnapRow = new QWidget(this);
    auto* snapLayout = new QHBoxLayout(mSnapRow);
    snapLayout->setContentsMargins(0, 0, 0, 0);
    snapLayout->setSpacing(kSpacing);
    snapGroup(Tool::Move, "grid-dots", {0.1, 0.5, 1, 5, 10, 50, 100, 500, 1000}, " cm");
    snapGroup(Tool::Rotate, "angle", {}, QString(QChar(0x00B0)));
    snapGroup(Tool::Scale, "resize", {0.0625, 0.1, 0.125, 0.25, 0.5, 1}, "");
    buildSymmetryRow();
    buildMaskRow();
    buildRetopoRow();

    mChevron = new QToolButton(this);
    mChevron->setIcon(icon("chevrons-right"));
    mChevron->setIconSize(QSize(kIcon, kIcon));
    mChevron->setToolTip("More viewport options");
    mChevron->setPopupMode(QToolButton::InstantPopup);
    auto* overflow = new QMenu(mChevron);
    mChevron->setMenu(overflow);
    optionBox::decorateMenu(overflow);
    connect(overflow, &QMenu::aboutToShow, this, [this, overflow] {
        overflow->clear();
        for (const Item& item : mItems)
            if (item.overflow && item.widget->isHidden())
                item.overflow(overflow);
        if (mMaskRow->isHidden() && !activeRetopoDraw())
        {
            overflow->addSeparator();
            for (QAction* a : mMaskActions)
                overflow->addAction(a);
        }
        if (mRetopoRow->isHidden() && activeRetopoDraw())
        {
            overflow->addSeparator();
            overflow->addAction(mRetopoSpace->defaultAction());
            overflow->addAction(mRetopoSurface->defaultAction());
        }
        if (mSymmetryRow->isHidden())
        {
            overflow->addSeparator();
            overflow->addAction(mSymmetryToggle->defaultAction());
        }
        if (mSnapRow->isHidden())
        {
            overflow->addSeparator();
            for (const SnapGroup& g : mSnapGroups)
                overflow->addAction(g.toggle->defaultAction());
        }
    });

    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            refresh();
    });
    mRenderSettingsListener = scene::addRenderSettingsListener([this] { refresh(); });
    mToolsListener = tools::addListener([this] { refresh(); });
    mViewportToolListener = viewportTools::addListener([this] { refreshRetopo(); });
    connect(mView, &ViewportWidget::viewStateChanged, this, &ViewportToolbar::refresh);
    refresh();
    refreshTooltips();
}

ViewportToolbar::~ViewportToolbar()
{
    PrefStore::get().removeOnChanged(mPrefListener);
    scene::removeRenderSettingsListener(mRenderSettingsListener);
    tools::removeListener(mToolsListener);
    viewportTools::removeListener(mViewportToolListener);
    scene::Graph::current().removeListener(mGraphListener);
}

QToolButton* ViewportToolbar::actionButton(const char* id, const char* iconName)
{
    auto* b = new QToolButton(this);
    std::string action = id;
    QAction* a = new QAction(icon(iconName), labelOf(action), b);
    a->setCheckable(viewportActionState(action, mView).has_value());
    b->setDefaultAction(a);
    b->setIconSize(QSize(kIcon, kIcon));
    // The action's own checked state comes from refresh(); the click only runs the registered action.
    connect(a, &QAction::triggered, this, [this, action] {
        activateView();
        actions::run(action);
        refresh();
    });
    mActionButtons.emplace_back(b, action);
    mItems.push_back({b, [a](QMenu* m) { m->addAction(a); }});
    return b;
}

QToolButton* ViewportToolbar::menuButton(const char* iconName, const QString& tip, std::function<void(QMenu*)> fill)
{
    auto* b = new QToolButton(this);
    if (iconName)
        b->setIcon(icon(iconName));
    b->setIconSize(QSize(kIcon, kIcon));
    b->setToolTip(tip);
    b->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(b);
    b->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu, fill] {
        activateView();
        menu->clear();
        fill(menu);
    });
    mItems.push_back({b, [this, tip, iconName, fill](QMenu* m) {
                          QMenu* sub = m->addMenu(iconName ? icon(iconName) : QIcon(), tip);
                          connect(sub, &QMenu::aboutToShow, this, [sub, fill] {
                              sub->clear();
                              fill(sub);
                          });
                      }});
    return b;
}

QDoubleSpinBox* ViewportToolbar::field(const char* key, double fallback, double lo, double hi, double step, const QString& tip)
{
    auto* s = new QDoubleSpinBox(this);
    s->setRange(lo, hi);
    s->setSingleStep(step);
    s->setDecimals(2);
    s->setKeyboardTracking(false);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    s->setAlignment(Qt::AlignRight);
    s->setFixedSize(42, kHeight - 6);
    s->setToolTip(tip);
    setSpinDefault(s, fallback);
    std::string k = key;
    connect(s, &QDoubleSpinBox::valueChanged, this, [k](double v) { PrefStore::get().set("general", k, v); });
    connect(s, &QDoubleSpinBox::editingFinished, this, [this] { mView->setFocus(); });
    mFields.push_back({s, {k, fallback}});
    mItems.push_back({s, nullptr});
    return s;
}

void ViewportToolbar::snapGroup(Tool t, const char* iconName, std::vector<double> steps, const QString& unit)
{
    auto* layout = static_cast<QHBoxLayout*>(mSnapRow->layout());
    if (!mSnapGroups.empty())
    {
        auto* line = new QFrame(mSnapRow);
        line->setFrameShape(QFrame::VLine);
        line->setObjectName("toolbarSeparator");
        line->setFixedSize(7, kHeight - 8);
        layout->addWidget(line);
    }
    std::string action = std::string("snap.") + tools::snapKey(t) + "Increment";
    auto* toggle = new QToolButton(mSnapRow);
    auto* a = new QAction(icon(iconName), labelOf(action), toggle);
    a->setCheckable(true);
    toggle->setDefaultAction(a);
    toggle->setIconSize(QSize(kIcon, kIcon));
    connect(a, &QAction::triggered, this, [this, action] {
        actions::run(action);
        refresh();
    });
    mActionButtons.emplace_back(toggle, action);

    auto* value = new QToolButton(mSnapRow);
    value->setToolButtonStyle(Qt::ToolButtonTextOnly);
    value->setPopupMode(QToolButton::InstantPopup);
    value->setToolTip(labelOf(action) + " increment" + (unit.trimmed().isEmpty() ? QString() : " (" + unit.trimmed() + ")"));
    auto* menu = new QMenu(value);
    value->setMenu(menu);
    std::string stepKey = snapPref(t, "step");
    connect(menu, &QMenu::aboutToShow, this, [this, t, menu, steps, unit, stepKey] {
        menu->clear();
        double current = tools::incrementSnap(t).step;
        bool custom = true;
        if (t == Tool::Rotate)
        {
            bool matched = false;
            menu->addAction(rotateStepColumns(menu, current, stepKey, matched));
            custom = !matched;
        }
        auto* group = new QActionGroup(menu);
        for (double v : steps)
        {
            bool on = std::abs(v - current) < 1e-9;
            custom = custom && !on;
            QAction* item = menu->addAction(t == Tool::Scale && v == 0.1 ? QString("0.1 (10%)") : stepText(v) + unit);
            item->setCheckable(true);
            item->setChecked(on);
            group->addAction(item);
            connect(item, &QAction::triggered, this, [stepKey, v] { PrefStore::get().set("general", stepKey, v); });
        }
        menu->addSeparator();
        QAction* other = menu->addAction(custom ? "Custom (" + stepText(current) + unit + ")..." : QString("Custom..."));
        other->setCheckable(true);
        other->setChecked(custom);
        connect(other, &QAction::triggered, this, [this, t, stepKey, current] {
            bool ok = false;
            double v = QInputDialog::getDouble(this, "Snap Increment", labelOf(std::string("snap.") + tools::snapKey(t) + "Increment"), current,
                                               1e-4, 1e6, 4, &ok);
            if (ok && v > 0.0)
                PrefStore::get().set("general", stepKey, v);
        });
        if (t == Tool::Move)
        {
            std::string absKey = snapPref(t, "absolute");
            QAction* abs = menu->addAction("Snap to World Grid");
            abs->setCheckable(true);
            abs->setChecked(PrefStore::get().value<bool>("general", absKey, false));
            abs->setToolTip("Land on multiples of the increment instead of stepping from where the drag started");
            connect(abs, &QAction::triggered, this, [absKey](bool on) { PrefStore::get().set("general", absKey, on); });
        }
    });

    layout->addWidget(toggle);
    layout->addWidget(value);
    mSnapGroups.push_back({t, toggle, value, unit});
}

void ViewportToolbar::buildSymmetryRow()
{
    mSymmetryRow = new QWidget(this);
    auto* layout = new QHBoxLayout(mSymmetryRow);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kSpacing);
    mSymmetryToggle = new QToolButton(mSymmetryRow);
    auto* a = new QAction(icon("flip-vertical"), labelOf("mesh.toggleSymmetry"), mSymmetryToggle);
    a->setCheckable(true);
    mSymmetryToggle->setDefaultAction(a);
    mSymmetryToggle->setIconSize(QSize(kIcon, kIcon));
    connect(a, &QAction::triggered, this, [this] {
        activateView();
        actions::run("mesh.toggleSymmetry");
        refreshSymmetry();
    });

    mSymmetryAxis = new QToolButton(mSymmetryRow);
    mSymmetryAxis->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mSymmetryAxis->setPopupMode(QToolButton::InstantPopup);
    mSymmetryAxis->setToolTip("Symmetry axis (object space); picking one turns symmetry on with it");
    auto* menu = new QMenu(mSymmetryAxis);
    mSymmetryAxis->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        scene::Graph& g = scene::Graph::current();
        scene::SymmetryState st = scene::symmetryState(g, g.selection());
        auto* group = new QActionGroup(menu);
        for (int i = 0; i < 3; ++i)
        {
            std::string id = std::string("mesh.symmetry") + char('X' + i);
            QAction* item = menu->addAction(labelOf(id));
            item->setCheckable(true);
            item->setChecked(st.on > 0 && st.axis == i);
            group->addAction(item);
            connect(item, &QAction::triggered, this, [this, i, id] {
                PrefStore::get().set("general", "/symmetry/axis", std::string(1, char('x' + i)));
                activateView();
                actions::run(id);
                refreshSymmetry();
            });
        }
        // Radial: the setting, then its count and mirror; picking either turns radial symmetry on with it.
        QAction* radial = menu->addAction(labelOf("mesh.symmetryRadial"));
        radial->setCheckable(true);
        radial->setChecked(st.on > 0 && st.axis == scene::SymmetryState::kRadialState);
        group->addAction(radial);
        connect(radial, &QAction::triggered, this, [this] {
            PrefStore::get().set("general", "/symmetry/axis", std::string("radial"));
            activateView();
            actions::run("mesh.symmetryRadial");
            refreshSymmetry();
        });
        auto setRadial = [this](json flags) {
            scene::Graph& graph = scene::Graph::current();
            json names = json::array();
            for (scene::NodeId id : graph.selection())
                if (const scene::Node* n = graph.node(id); n && n->type().isA("mesh"))
                    names.push_back(n->path());
            if (names.empty())
                return;
            flags["args"] = names;
            flags["axis"] = "radial";
            PrefStore::get().set("general", "/symmetry/axis", std::string("radial"));
            activateView();
            try
            {
                ops::execute("setSymmetry", flags, CallSource::UI);
            }
            catch (const std::exception& e)
            {
                log::warn("{}", e.what());
            }
            refreshSymmetry();
        };
        QMenu* counts = menu->addMenu("Radial Count");
        for (int count : {2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32})
        {
            QAction* item = counts->addAction(QString::number(count));
            item->setCheckable(true);
            item->setChecked(st.axis == scene::SymmetryState::kRadialState && st.radialCount == count);
            connect(item, &QAction::triggered, this, [setRadial, count] { setRadial({{"count", count}}); });
        }
        QAction* mirrored = menu->addAction("Radial Mirror");
        mirrored->setCheckable(true);
        mirrored->setChecked(st.axis == scene::SymmetryState::kRadialState && st.radialCount > 0 && st.radialMirror);
        mirrored->setToolTip("Also mirror each wedge across a plane through the axis (dihedral symmetry: rims, flowers)");
        connect(mirrored, &QAction::triggered, this, [setRadial](bool on) { setRadial({{"mirror", on}}); });
        menu->addSeparator();
        bool custom = false;
        for (scene::NodeId id : g.selection())
            if (const scene::Node* n = g.node(id); n && n->type().isA("mesh") && n->as<int64_t>(scene::MeshNode::kSymmetryPlane) == 1)
                custom = true;
        QAction* fromSel = menu->addAction(labelOf("mesh.symmetryPlaneFromSelection"));
        fromSel->setEnabled(scene::components::mode() == scene::ComponentMode::Vertex || scene::components::mode() == scene::ComponentMode::Edge);
        connect(fromSel, &QAction::triggered, this, [this] {
            activateView();
            actions::run("mesh.symmetryPlaneFromSelection");
            refreshSymmetry();
        });
        QAction* atPivot = menu->addAction(labelOf("mesh.symmetryPlaneFromPivot"));
        atPivot->setCheckable(true);
        atPivot->setChecked(!custom);
        atPivot->setEnabled(custom);
        connect(atPivot, &QAction::triggered, this, [this] {
            activateView();
            actions::run("mesh.symmetryPlaneFromPivot");
            refreshSymmetry();
        });
        menu->addSeparator();
        QAction* off = menu->addAction(labelOf("mesh.symmetryOff"));
        off->setEnabled(st.on > 0);
        connect(off, &QAction::triggered, this, [this] {
            activateView();
            actions::run("mesh.symmetryOff");
            refreshSymmetry();
        });
    });
    layout->addWidget(mSymmetryToggle);
    layout->addWidget(mSymmetryAxis);
    auto* line = new QFrame(mSymmetryRow);
    line->setFrameShape(QFrame::VLine);
    line->setObjectName("toolbarSeparator");
    line->setFixedSize(7, kHeight - 8);
    layout->addWidget(line);

    // Value changes arrive per drag step, so coalesce them; symmetry only needs the latest state.
    mSymmetryRefresh = new QTimer(this);
    mSymmetryRefresh->setSingleShot(true);
    mSymmetryRefresh->setInterval(30);
    connect(mSymmetryRefresh, &QTimer::timeout, this, &ViewportToolbar::refreshSymmetry);
    mGraphListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        if (c != scene::Change::Renamed && c != scene::Change::Reparented)
            mSymmetryRefresh->start();
    });
}

void ViewportToolbar::buildMaskRow()
{
    namespace sm = scene::selectionMask;
    mMaskRow = new QWidget(this);
    auto* layout = new QHBoxLayout(mMaskRow);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kSpacing);
    std::vector<std::pair<std::string, const char*>> buttons = {{"viewport.togglePreselectObjects", "pointer"}};
    for (auto [kind, iconName] : {std::pair{sm::Kind::Mesh, "box"}, {sm::Kind::Joint, "bone"}, {sm::Kind::Locator, "crosshair"}, {sm::Kind::Other, "dots"}})
        buttons.emplace_back(std::string("select.mask.") + sm::name(kind), iconName);
    for (const auto& [action, iconName] : buttons)
    {
        auto* b = new QToolButton(mMaskRow);
        auto* a = new QAction(icon(iconName), labelOf(action), b);
        a->setCheckable(true);
        b->setDefaultAction(a);
        b->setIconSize(QSize(kIcon, kIcon));
        connect(a, &QAction::triggered, this, [this, action] {
            actions::run(action);
            refresh();
        });
        mActionButtons.emplace_back(b, action);
        mMaskActions.push_back(a);
        layout->addWidget(b);
        if (action == "viewport.togglePreselectObjects")
        {
            auto* line = new QFrame(mMaskRow);
            line->setFrameShape(QFrame::VLine);
            line->setObjectName("toolbarSeparator");
            line->setFixedSize(7, kHeight - 8);
            layout->addWidget(line);
        }
    }
}

void ViewportToolbar::buildRetopoRow()
{
    mRetopoRow = new QWidget(this);
    auto* layout = new QHBoxLayout(mRetopoRow);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kSpacing);
    auto* title = new QLabel("Retopo", mRetopoRow);
    title->setStyleSheet("color: #8c8c8c; padding: 0 4px;");
    layout->addWidget(title);

    mRetopoSpace = new QToolButton(mRetopoRow);
    auto* space = new QAction(labelOf("retopo.toggleSnapSpace"), mRetopoSpace);
    space->setCheckable(true);
    mRetopoSpace->setDefaultAction(space);
    mRetopoSpace->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connect(space, &QAction::triggered, this, [this] {
        actions::run("retopo.toggleSnapSpace");
        refreshRetopo();
    });
    layout->addWidget(mRetopoSpace);

    mRetopoAngle = new QToolButton(mRetopoRow);
    mRetopoAngle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    mRetopoAngle->setPopupMode(QToolButton::InstantPopup);
    mRetopoAngle->setToolTip("Retopo Draw snap angle: Shift places the next dot on a line from the last one at a multiple of this");
    auto* menu = new QMenu(mRetopoAngle);
    mRetopoAngle->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        double current = RetopoDrawTool::snapAngle();
        bool matched = false;
        menu->addAction(rotateStepColumns(menu, current, "/retopoDraw/snapAngle", matched));
        menu->addSeparator();
        QAction* other = menu->addAction(!matched ? "Custom (" + stepText(current) + QChar(0x00B0) + ")..." : QString("Custom..."));
        other->setCheckable(true);
        other->setChecked(!matched);
        connect(other, &QAction::triggered, this, [this, current] {
            bool ok = false;
            double v = QInputDialog::getDouble(this, "Retopo Snap Angle", "Snap angle (degrees)", current, 0.1, 180.0, 2, &ok);
            if (ok)
                RetopoDrawTool::setSnapAngle(v);
        });
    });
    layout->addWidget(mRetopoAngle);

    auto* line = new QFrame(mRetopoRow);
    line->setFrameShape(QFrame::VLine);
    line->setObjectName("toolbarSeparator");
    line->setFixedSize(7, kHeight - 8);
    layout->addWidget(line);

    mRetopoSurface = new QToolButton(mRetopoRow);
    auto* surface = new QAction(icon("target"), labelOf("retopo.setReference"), mRetopoSurface);
    mRetopoSurface->setDefaultAction(surface);
    mRetopoSurface->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    mRetopoSurface->setIconSize(QSize(kIcon, kIcon));
    connect(surface, &QAction::triggered, this, [this] {
        activateView();
        actions::run("retopo.setReference");
        refreshRetopo();
    });
    layout->addWidget(mRetopoSurface);
    mRetopoRow->setVisible(false);
}

void ViewportToolbar::refreshRetopo()
{
    RetopoDrawTool* tool = activeRetopoDraw();
    const bool active = tool != nullptr;
    if (mRetopoRow->property("active").toBool() != active)
    {
        mRetopoRow->setProperty("active", active);
        layoutItems();
    }
    if (!tool)
        return;
    const bool object = RetopoDrawTool::snapObjectSpace();
    QAction* space = mRetopoSpace->defaultAction();
    if (QString text = object ? "Object" : "World"; space->text() != text)
    {
        QSignalBlocker block(space);
        space->setChecked(object);
        space->setText(text);
        space->setToolTip(tipFor("retopo.toggleSnapSpace") + "\nNow: " + (object ? "object axes" : "world axes"));
    }
    QString angle = stepText(RetopoDrawTool::snapAngle()) + QChar(0x00B0);
    const scene::Node* ref = scene::Graph::current().node(tool->reference());
    QString name = ref ? QString::fromStdString(ref->name()) : QString("(none)");
    if (mRetopoAngle->text() != angle || mRetopoSurface->defaultAction()->text() != name)
    {
        mRetopoAngle->setText(angle);
        QAction* surface = mRetopoSurface->defaultAction();
        surface->setText(name);
        surface->setToolTip("Retopo surface: " + (ref ? QString::fromStdString(ref->path()) : QString("none")) + "\n" +
                            tipFor("retopo.setReference"));
        mRetopoRow->adjustSize();
        layoutItems();
    }
}

void ViewportToolbar::refreshSymmetry()
{
    scene::Graph& g = scene::Graph::current();
    scene::SymmetryState st = scene::symmetryState(g, g.selection());
    const bool all = st.meshes > 0 && st.on == st.meshes;
    const bool partial = st.on > 0 && !all;
    mSymmetryRow->setEnabled(st.meshes > 0);
    {
        QSignalBlocker block(mSymmetryToggle->defaultAction());
        mSymmetryToggle->defaultAction()->setChecked(all);
    }
    if (mSymmetryToggle->property("partial").toBool() != partial)
    {
        mSymmetryToggle->setProperty("partial", partial);
        mSymmetryToggle->style()->unpolish(mSymmetryToggle);
        mSymmetryToggle->style()->polish(mSymmetryToggle);
    }
    std::string remembered = PrefStore::get().value<std::string>("general", "/symmetry/axis", "x");
    auto radialText = [&st] {
        return st.radialCount > 0 ? QString("%1%2").arg(st.radialMirror ? "D" : "R").arg(st.radialCount) : QString("R");
    };
    QString axis = st.on > 0 && st.axis < 0                      ? QString("Mix")
                   : st.axis == scene::SymmetryState::kRadialState ? radialText()
                   : st.axis >= 0                                   ? QString(QChar('X' + st.axis))
                   : remembered == "radial"                         ? QString("R")
                                                                    : QString::fromStdString(remembered).toUpper();
    mSymmetryAxis->setText(axis);

    QString state = st.meshes == 0 ? QString("No mesh selected")
                    : all          ? "On (" + axis + ")"
                    : partial      ? QString("On for %1 of %2 selected meshes; click to turn on for all").arg(st.on).arg(st.meshes)
                                   : QString("Off");
    QAction* a = mSymmetryToggle->defaultAction();
    a->setText(labelOf("mesh.toggleSymmetry"));
    a->setToolTip(tipFor("mesh.toggleSymmetry") + "\nSymmetry: " + state);
    mSymmetryRow->adjustSize();
}

void ViewportToolbar::separator()
{
    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::VLine);
    line->setObjectName("toolbarSeparator");
    line->setFixedSize(7, kHeight - 8);
    mItems.push_back({line, nullptr});
}

void ViewportToolbar::activateView()
{
    mView->setFocus(Qt::OtherFocusReason);
}

void ViewportToolbar::refresh()
{
    for (auto& [b, id] : mActionButtons)
        if (std::optional<bool> on = viewportActionState(id, mView))
        {
            QSignalBlocker block(b->defaultAction());
            b->defaultAction()->setChecked(*on);
            if (id == "viewport.monotone")
                b->defaultAction()->setIcon(icon(*on ? "palette-off" : "palette"));
        }
    for (auto& [s, kf] : mFields)
    {
        double v = PrefStore::get().value<double>("general", kf.first, kf.second);
        if (std::abs(s->value() - v) > 1e-6 && !s->hasFocus())
        {
            QSignalBlocker block(s);
            s->setValue(v);
        }
    }
    for (const SnapGroup& g : mSnapGroups)
    {
        QSignalBlocker block(g.toggle->defaultAction());
        bool on = PrefStore::get().value<bool>("general", snapPref(g.tool, "enabled"), false);
        // Holding the rotate snap key flips it for the drag without touching the pref.
        if (g.tool == Tool::Rotate && (tools::snaps() & SnapRotateHold))
            on = !on;
        g.toggle->defaultAction()->setChecked(on);
        g.value->setText(stepText(tools::incrementSnap(g.tool).step) + g.unit.trimmed());
    }
    mSnapRow->adjustSize();
    refreshRetopo();
    mCameraButton->setToolTip("Camera: " + QString::fromStdString(mView->viewName()) + "\nRight-click to select it");
    int samples = PrefStore::get().value<int>("general", vd::kAaSamples, vd::kDefaultSamples);
    mSamplesButton->setText(QString::number(samples) + "x");
    mSamplesButton->setEnabled(pref(vd::kAaEnabled, true));
    refreshSymmetry();
}

void ViewportToolbar::refreshTooltips()
{
    for (auto& [b, id] : mActionButtons)
    {
        b->defaultAction()->setText(labelOf(id));
        b->defaultAction()->setToolTip(tipFor(id));
    }
    refreshSymmetry();
}

QSize ViewportToolbar::sizeHint() const
{
    int w = 4 + mSnapRow->sizeHint().width() + 8 + mSymmetryRow->sizeHint().width() + 16 +
            std::max(mMaskRow->sizeHint().width(), mRetopoRow->sizeHint().width());
    for (const Item& item : mItems)
        w += item.widget->sizeHint().width() + kSpacing;
    return {w, kHeight};
}

QSize ViewportToolbar::minimumSizeHint() const
{
    return {0, kHeight};
}

void ViewportToolbar::layoutItems()
{
    int chevronWidth = mChevron->sizeHint().width();
    int x = 2;
    int available = width() - 2;
    // The snap row keeps the far right and the symmetry row sits before it, each while it fits beside the chevron; the
    // left row overflows before either.
    QSize snap = mSnapRow->sizeHint();
    bool snapFits = available - snap.width() > chevronWidth + 2;
    mSnapRow->setVisible(snapFits);
    if (snapFits)
    {
        mSnapRow->setGeometry(available - snap.width(), (kHeight - snap.height()) / 2, snap.width(), snap.height());
        available -= snap.width();
    }
    QSize sym = mSymmetryRow->sizeHint();
    bool symFits = available - sym.width() > chevronWidth + 2;
    mSymmetryRow->setVisible(symFits);
    if (symFits)
    {
        mSymmetryRow->setGeometry(available - sym.width(), (kHeight - sym.height()) / 2, sym.width(), sym.height());
        available -= sym.width() + 8;
    }
    // The masks (Retopo Draw's row while it runs) keep their place while the whole left row fits beside them, and go to
    // the chevron menu first.
    const bool retopo = activeRetopoDraw() != nullptr;
    QWidget* centre = retopo ? mRetopoRow : mMaskRow;
    (retopo ? mMaskRow : mRetopoRow)->setVisible(false);
    QSize mask = centre->sizeHint();
    int leftWidth = 0;
    for (const Item& item : mItems)
    {
        QWidget* w = item.widget;
        leftWidth += (w->maximumSize().width() < QWIDGETSIZE_MAX ? w->maximumSize() : w->sizeHint()).width() + kSpacing;
    }
    const int maskGap = 16;
    bool maskFits = x + leftWidth + maskGap + mask.width() + maskGap <= available;
    centre->setVisible(maskFits);
    if (maskFits)
    {
        int left = x + leftWidth, gap = available - left;
        centre->setGeometry(left + (gap - mask.width()) / 2, (kHeight - mask.height()) / 2, mask.width(), mask.height());
    }
    bool overflowed = false;
    for (size_t i = 0; i < mItems.size(); ++i)
    {
        QWidget* w = mItems[i].widget;
        QSize s = w->sizeHint();
        if (QSize fixed = w->maximumSize(); fixed.width() < QWIDGETSIZE_MAX)
            s = fixed;
        // The last item may use the chevron's space; anything before it has to leave room for the chevron.
        int limit = i + 1 == mItems.size() ? available : available - chevronWidth;
        if (!overflowed && x + s.width() > limit)
            overflowed = true;
        w->setVisible(!overflowed);
        if (!overflowed)
        {
            w->setGeometry(x, (kHeight - s.height()) / 2, s.width(), s.height());
            x += s.width() + kSpacing;
        }
    }
    mChevron->setVisible(overflowed);
    if (overflowed)
    {
        QSize s = mChevron->sizeHint();
        mChevron->setGeometry(available - s.width(), (kHeight - s.height()) / 2, s.width(), s.height());
    }
}

void ViewportToolbar::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutItems();
}

bool ViewportToolbar::event(QEvent* e)
{
    // Hotkeys are live-editable, so tooltips pick up the current binding whenever the pointer comes over the row.
    if (e->type() == QEvent::Enter)
        refreshTooltips();
    else if (e->type() == QEvent::Show || e->type() == QEvent::LayoutRequest)
        layoutItems();
    return QWidget::event(e);
}

ViewportPanel::ViewportPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    mView = new ViewportWidget(this);
    mToolbar = new ViewportToolbar(mView, this);
    v->addWidget(mToolbar);
    v->addWidget(mView, 1);
    imagePlanes::acceptDrops(mView);
    mToolbar->setVisible(pref(vd::kToolbar, true));
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            mToolbar->setVisible(pref(vd::kToolbar, true));
    });
}

ViewportPanel::~ViewportPanel()
{
    PrefStore::get().removeOnChanged(mPrefListener);
}
} // namespace lila::ui
