#include "ui/CurveToolkit.h"

#include "core/PrefStore.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/DockSidebar.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/OptionBox.h"
#include "ui/SpinScrub.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <tuple>

namespace lila::ui
{
namespace
{
constexpr const char* kCurveCollapsedPref = "/curveToolkit/collapsed";

QWidget* curveSection(const QString& title, QWidget* content, QWidget* parent)
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
    std::string key = std::string(kCurveCollapsedPref) + "/" + title.toStdString();
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
    return box;
}

QWidget* curveFlowBox(QWidget* parent, FlowLayout** layout)
{
    auto* w = new QWidget(parent);
    *layout = new FlowLayout(3);
    (*layout)->setContentsMargins(2, 0, 2, 2);
    w->setLayout(*layout);
    return w;
}

QDockWidget* findCurveDock()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = w->findChild<QDockWidget*>(kCurveToolkitDock))
            return d;
    return nullptr;
}

using Item = std::tuple<const char*, const char*, const char*>;

/** The toolkit's sections: action, icon, short text (empty for the action's own label). */
const std::vector<std::pair<const char*, std::vector<Item>>>& curveSections()
{
    static const std::vector<std::pair<const char*, std::vector<Item>>> sections = {
        {"Draw",
         {{"curve.ezCurve", "line", "EZ"}, {"curve.bezierPen", "vector-bezier", "Pen"}, {"curve.cvTool", "vector-bezier-2", "CV"},
          {"curve.epTool", "vector-spline", "EP"}, {"curve.pencil", "scribble", "Pencil"}, {"curve.catmullRom", "route", "Catmull-Rom"},
          {"curve.kappa", "ease-in-out", "Kappa"}, {"curve.circle", "circle", "Circle"}, {"curve.fromEdges", "polygon", "Edges"},
          {"curve.sketchTool", "ruler-2", "Sketch"}}},
        {"Edit",
         {{"curve.curvePointMode", "point", "Curve Point"}, {"curve.insertAtCurvePoints", "plus", "Insert Point"}, {"curve.fillet", "vector-bezier-arc", "Fillet"}, {"curve.offset", "ripple", "Offset"}, {"curve.rebuild", "refresh", "Rebuild"},
          {"curve.evenSpacing", "dots", "Even"}, {"curve.fit", "wave-sine", "Fit"}, {"curve.project", "arrow-bar-to-down", "Project"},
          {"curve.attach", "link", "Attach"}, {"curve.detach", "unlink", "Detach"}, {"curve.reverse", "arrows-exchange", "Reverse"},
          {"curve.close", "circle-dashed", "Close"}, {"curve.open", "circle-half", "Open"}, {"curve.deletePoints", "trash", "Delete"}}},
        {"Symmetry",
         {{"curve.symmetryX", "flip-horizontal", "X"}, {"curve.symmetryY", "flip-vertical", "Y"}, {"curve.symmetryZ", "flip-horizontal", "Z"},
          {"curve.symmetryOff", "x", "Off"}, {"curve.bakeSymmetry", "copy", "Bake"}}},
        {"Deform",
         {{"curve.bend", "chart-arcs-3", "Bend"}, {"curve.curl", "spiral", "Curl"}, {"curve.scaleCurvature", "chart-arcs", "Curvature"},
          {"curve.smooth", "wave-sine", "Smooth"}}},
        {"Blend and Trim",
         {{"curve.blendTool", "arrows-join", "Blend Tool"}, {"curve.blend", "arrows-join-2", "Blend"}, {"curve.match", "arrow-merge", "Match"},
          {"curve.trimTool", "scissors", "Trim"}, {"curve.extend", "arrows-horizontal", "Extend"}, {"curve.split", "arrows-split", "Split"},
          {"curve.fair", "ease-in-out", "Fair"}}},
        {"Surfaces",
         {{"curve.extrude", "route", "Sweep"}, {"curve.revolve", "rotate-360", "Revolve"}, {"curve.loft", "stack-3", "Loft"},
          {"curve.patchFill", "square-plus", "Patch"}, {"curve.network", "grid-pattern", "Network"}, {"curve.borderBlend", "border-outer", "Border"}}},
        {"Pipes and Cables",
         {{"curve.pipeTool", "route-2", "Pipe Tool"}, {"curve.pipe", "route-2", "Pipe"}, {"curve.pipePresetPipe", "cylinder", "Pipe Preset"},
          {"curve.pipePresetHose", "wave-saw-tool", "Hose Preset"}, {"curve.pipePresetCable", "bolt", "Cable Preset"},
          {"curve.pipePresetConduit", "line-dashed", "Conduit Preset"}, {"curve.cableTool", "plug", "Cable Tool"},
          {"curve.cable", "plug-connected", "Cable"}}},
        {"Along and On Surface",
         {{"curve.deformAlongCurve", "route", "Deform Along"}, {"curve.arrayAlongCurve", "dots", "Array Along"},
          {"curve.instanceAlongCurve", "copy", "Instance Along"}, {"curve.flowAlongSurface", "wave-square", "Flow"},
          {"curve.keepOnSurface", "magnet", "Keep On"}, {"curve.releaseFromSurface", "magnet-off", "Release"}}},
        {"Planar",
         {{"curve.text", "typography", "Text"}, {"curve.bevelPlus", "text-size", "Bevel+"}, {"curve.booleanUnion", "layers-union", "Union"},
          {"curve.booleanSubtract", "layers-difference", "Subtract"}, {"curve.booleanIntersect", "layers-intersect", "Intersect"},
          {"curve.booleanXor", "layers-subtract", "Xor"}, {"curve.offsetRegion", "ripple", "Offset Region"},
          {"curve.insetSkeleton", "box-model-2", "Skeleton"}, {"curve.centerLine", "line-dashed", "Center Line"}, {"curve.roof", "home", "Roof"}}},
        {"Sections and Groom",
         {{"curve.contour", "topology-ring", "Contour"}, {"curve.sectionTool", "slice", "Section"}, {"curve.sculptCurves", "brush", "Sculpt"},
          {"curve.groomSurface", "feather", "Groom"}, {"curve.groomGuides", "git-branch", "Guides"}, {"curve.hairCards", "cards", "Hair Cards"}}},
        {"Handles and Convert",
         {{"curve.handleAuto", "", "Auto"}, {"curve.handleVector", "", "Vector"}, {"curve.handleAligned", "", "Aligned"},
          {"curve.handleFree", "", "Free"}, {"curve.toBezier", "vector-bezier", "To Bezier"}, {"curve.toNurbs", "vector-spline", "To NURBS"},
          {"curve.toPoly", "polygon", "To Poly"}, {"curve.toggleComb", "chart-line", "Comb"}, {"curve.sendToUnreal", "send", "Send to Unreal"}}},
    };
    return sections;
}
} // namespace

CurveToolkit::CurveToolkit(QWidget* parent) : QWidget(parent)
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

    FlowLayout* head = nullptr;
    QWidget* headBox = curveFlowBox(body, &head);
    mModes = new QButtonGroup(this);
    mModes->setExclusive(true);
    int modeIndex = 0;
    for (auto [id, ic, text] : {Item{"select.objectMode", "cube", "Object"}, Item{"select.vertexMode", "point", "Points"}})
    {
        if (QToolButton* b = actionButton(id, ic, text, headBox))
        {
            b->setCheckable(true);
            mModes->addButton(b, modeIndex);
            head->addWidget(b);
        }
        ++modeIndex;
    }
    for (auto [member, id, ic, text] : {std::tuple{&mOnSurface, "curve.drawOnSurfaces", "magnet", "On Surfaces"},
                                        std::tuple{&mKeepOnSurface, "curve.keepDrawnOnSurface", "pin", "Keep On"}})
        if ((*member = actionButton(id, ic, text, headBox)))
        {
            (*member)->setCheckable(true);
            head->addWidget(*member);
        }
    mOffset = new QDoubleSpinBox(headBox);
    mOffset->setRange(0.0, 10000.0);
    mOffset->setDecimals(2);
    mOffset->setPrefix("Offset ");
    mOffset->setSuffix(" cm");
    mOffset->setKeyboardTracking(false);
    setSpinDefault(mOffset, 0.0);
    mOffset->setToolTip("How far drawn curves sit off the surface they are drawn on (Ctrl+scroll in a draw tool)");
    connect(mOffset, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", "/curveTools/surfaceOffset", v); });
    head->addWidget(mOffset);
    mDegree = new QSpinBox(headBox);
    mDegree->setRange(1, 7);
    mDegree->setPrefix("Degree ");
    mDegree->setKeyboardTracking(false);
    mDegree->setToolTip("Degree of curves the CV and EP tools draw");
    connect(mDegree, &QSpinBox::valueChanged, this, [](int v) { PrefStore::get().set("general", "/curveTools/degree", v); });
    head->addWidget(mDegree);
    mDrawSymmetry = new QComboBox(headBox);
    mDrawSymmetry->addItems({"Draw Symmetry Off", "Draw Symmetry X", "Draw Symmetry Y", "Draw Symmetry Z"});
    mDrawSymmetry->setToolTip("Curves the draw tools make are mirrored across this world plane (Y in a draw tool cycles it)");
    connect(mDrawSymmetry, &QComboBox::activated, this, [](int i) { PrefStore::get().set("general", "/curveTools/symmetry", i); });
    head->addWidget(mDrawSymmetry);
    root->addWidget(headBox);

    mInfo = new QLabel(body);
    mInfo->setContentsMargins(4, 0, 4, 2);
    root->addWidget(mInfo);

    for (const auto& [title, items] : curveSections())
    {
        FlowLayout* flow = nullptr;
        QWidget* box = curveFlowBox(body, &flow);
        for (auto [id, ic, text] : items)
            if (QToolButton* b = actionButton(id, ic, text, box))
                flow->addWidget(optionBox::withOptionBox(b, id, box));
        if (flow->count())
            root->addWidget(curveSection(title, box, body));
        else
            delete box;
    }

    auto* optionsBox = new QWidget(body);
    auto* optionsLayout = new QVBoxLayout(optionsBox);
    optionsLayout->setContentsMargins(4, 0, 4, 4);
    optionsLayout->addWidget(new LastOpEditor(optionsBox));
    root->addWidget(curveSection("Tool Options", optionsBox, body));
    root->addStretch(1);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(30);
    connect(mRefresh, &QTimer::timeout, this, [this] { refreshState(); });
    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) { mRefresh->start(); });
    mComponentListener = scene::components::addListener([this] { mRefresh->start(); });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
    refreshState();
}

CurveToolkit::~CurveToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* CurveToolkit::actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent)
{
    const Action* action = actions::find(id);
    if (!action)
        return nullptr;
    auto* b = new QToolButton(parent);
    if (!iconName.isEmpty())
        b->setIcon(icon(iconName));
    b->setText(text.isEmpty() ? QString::fromStdString(action->label) : text);
    b->setToolButtonStyle(iconName.isEmpty() ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
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

void CurveToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    // Hotkeys are live-editable, so tooltips pick up the current keys whenever the panel shows.
    for (QToolButton* b : mButtons)
        if (std::string id = b->property("lilaAction").toString().toStdString(); actions::find(id))
            b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    refreshState();
}

void CurveToolkit::refreshState()
{
    const bool points = scene::components::mode() != scene::ComponentMode::Object;
    if (QAbstractButton* b = mModes->button(points ? 1 : 0); b && !b->isChecked())
        b->setChecked(true);
    PrefStore& p = PrefStore::get();
    if (mOnSurface)
        mOnSurface->setChecked(p.value<bool>("general", "/curveTools/onSurface", false));
    if (mKeepOnSurface)
        mKeepOnSurface->setChecked(p.value<bool>("general", "/curveTools/conform", false));
    if (!mOffset->hasFocus())
    {
        QSignalBlocker block(mOffset);
        mOffset->setValue(p.value<double>("general", "/curveTools/surfaceOffset", 0.0));
    }
    {
        QSignalBlocker block(mDrawSymmetry);
        mDrawSymmetry->setCurrentIndex(std::clamp(p.value<int>("general", "/curveTools/symmetry", 0), 0, 3));
    }
    if (!mDegree->hasFocus())
    {
        QSignalBlocker block(mDegree);
        mDegree->setValue(std::clamp(p.value<int>("general", "/curveTools/degree", 3), 1, 7));
    }

    scene::Graph& g = scene::Graph::current();
    int curves = 0;
    size_t pointCount = 0;
    double length = 0.0;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
        {
            ++curves;
            for (const curve::Spline& s : curve::shapeSplines(g, id))
            {
                pointCount += s.size();
                length += curve::length(s);
            }
        }
    mInfo->setText(curves ? QString("%1 curve%2, %3 points, %4 cm long").arg(curves).arg(curves == 1 ? "" : "s").arg(pointCount).arg(length, 0, 'f', 1)
                          : QString("No curve selected"));
}

void registerCurveToolkitActions()
{
    actions::add({.id = "window.curveToolkit", .label = "Curve Toolkit", .category = "Windows", .run = [] { toggleDock(findCurveDock()); },
                  .doc = "Curve draw tools, their surface options, curve editing, deforming, surfacing and planar commands, and live tool options in one panel.",
                  .python = "lila.ui.run_action(\"window.curveToolkit\")"});
}
} // namespace lila::ui
