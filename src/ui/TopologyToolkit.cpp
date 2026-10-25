#include "ui/TopologyToolkit.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "retopo/Retopo.h"
#include "retopo/RetopoCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/NodeLookup.h"
#include "ui/OptionBox.h"
#include "ui/RetopoDrawTool.h"
#include "ui/SpinScrub.h"
#include "ui/TopologyTool.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
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
using retopo::ConstructKind;

constexpr const char* kPrefs = "general";
constexpr const char* kCollapsedPref = "/topologyToolkit/collapsed";

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
    bool open = !PrefStore::get().value<bool>(kPrefs, key, false);
    header->setChecked(open);
    header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    content->setVisible(open);
    QObject::connect(header, &QToolButton::toggled, content, [header, content, key](bool on) {
        content->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        PrefStore::get().set(kPrefs, key, !on);
    });
    v->addWidget(header);
    v->addWidget(content, 1);
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

/** The retopo mesh the toolkit shows: the running tool's, else the one the selection resolves to. */
scene::Node* sessionRetopo(scene::NodeId* reference = nullptr)
{
    std::optional<retopo::RetopoDrawTargets> t;
    if (TopologyTool* tool = activeTopologyTool())
        t = retopo::RetopoDrawTargets{tool->reference(), tool->retopoName()};
    else if (RetopoDrawTool* draw = activeRetopoDraw())
        t = retopo::RetopoDrawTargets{draw->reference(), draw->retopoName()};
    else
        t = retopo::resolveRetopoDraw(scene::Graph::current(), scene::Graph::current().selection());
    if (!t)
        return nullptr;
    if (reference)
        *reference = t->reference;
    return findNode(t->retopo);
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

/** One setting of a construct kind, edited in the active construct's panel. */
struct Field
{
    const char* key;
    const char* label;
    char type; // i(nt), d(ouble), b(ool)
    double min, max, fallback;
    /** Shown from the record's generated counts when the spec leaves it on auto (0); -1 for none. */
    int countIndex;
    const char* tip;
};

std::vector<Field> fieldsFor(ConstructKind k)
{
    switch (k)
    {
    case ConstructKind::Patch:
        return {{"padding", "Padding", 'i', -1, 50, -1, -1, "Loops of quads kept along the whole border before the pattern; -1 uses the fewest that fit."},
                {"variant", "Variant", 'i', 0, 999, 0, -1, "Which pattern fills the border; Ctrl-wheel over the patch cycles them."}};
    case ConstructKind::Strip:
        return {{"divisions", "Along", 'i', 0, 999, 0, 0, "Quads along the strip; 0 keeps them near square."},
                {"rows", "Across", 'i', 1, 50, 1, -1, "Quads across the strip."},
                {"width", "Width", 'd', 0.01, 1e5, 1, -1, "Strip width, cm."}};
    case ConstructKind::Stroke: return {{"divisions", "Divisions", 'i', 1, 999, 8, -1, "Edges along the stroke."}};
    case ConstructKind::Bridge:
        return {{"divisions", "Along", 'i', 0, 999, 0, 0, "Edges along when neither side is an existing border; 0 from the length."},
                {"rows", "Across", 'i', 0, 999, 0, 1, "Edges across the gap; 0 from its width."}};
    case ConstructKind::Sweep:
        return {{"span", "Span", 'i', -999, 999, 1, -1, "Border edges carried along; negative takes the other side of the anchor."},
                {"rows", "Rows", 'i', 0, 999, 0, 0, "Rows along the stroke; 0 from its length."}};
    case ConstructKind::Cylinder:
        return {{"points", "Points", 'i', 3, 512, 8, -1, "Points around each ring."},
                {"rotate", "Rotate", 'd', -360, 360, 0, -1, "Turns where the rings start, degrees."},
                {"twist", "Twist", 'd', -180, 180, 0, -1, "Extra turn per ring, degrees."},
                {"rowDensity", "Row density", 'd', 0.1, 10, 1, -1, "Rows between rings; 1 keeps quads near square."},
                {"capStart", "Cap start", 'b', 0, 1, 0, -1, "Fill the first ring with a patch."},
                {"capEnd", "Cap end", 'b', 0, 1, 0, -1, "Fill the last ring with a patch."}};
    case ConstructKind::Region: return {{"edgeLength", "Edge length", 'd', 0.01, 1e5, 1, -1, "Edge length the auto retopo aims for, cm."}};
    }
    return {};
}

const char* kindLabel(ConstructKind k)
{
    static const char* labels[] = {"Patch", "Strip", "Stroke", "Bridge", "Sweep", "Cylinder", "Region"};
    return labels[size_t(k)];
}
} // namespace

TopologyToolkit::TopologyToolkit(QWidget* parent) : QWidget(parent)
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

    // Session.
    auto* sessionBox = new QWidget(body);
    auto* sessionLayout = new QVBoxLayout(sessionBox);
    sessionLayout->setContentsMargins(2, 0, 2, 2);
    mSession = new QLabel(sessionBox);
    mSession->setWordWrap(true);
    sessionLayout->addWidget(mSession);
    FlowLayout* sessionRow = nullptr;
    QWidget* sessionButtons = flowBox(sessionBox, &sessionRow);
    row(sessionButtons, {{"retopo.setReference", "target", "Set Reference"}}, sessionRow);
    auto* symmetry = new QComboBox(sessionButtons);
    symmetry->addItems({"Symmetry off", "Symmetry X", "Symmetry Y", "Symmetry Z"});
    symmetry->setObjectName("topologySymmetry");
    symmetry->setToolTip("The retopo mesh's mirror plane: every topology tool builds both halves");
    connect(symmetry, &QComboBox::activated, this, [](int i) {
        if (scene::Node* n = sessionRetopo())
        {
            static const char* axes[] = {"off", "x", "y", "z"};
            runCommand("setSymmetry", {{"args", {n->path()}}, {"axis", axes[std::clamp(i, 0, 3)]}});
        }
    });
    sessionRow->addWidget(symmetry);
    sessionLayout->addWidget(sessionButtons);
    root->addWidget(section("Session", sessionBox, body));

    // Tools.
    FlowLayout* tools = nullptr;
    QWidget* toolsBox = flowBox(body, &tools);
    row(toolsBox,
        {{"retopo.draw", "pencil", "Draw"}, {"topo.pencil", "pencil-plus", "Pencil"}, {"topo.patch", "square-plus", "Patch"},
         {"topo.strip", "road", "Strip"}, {"topo.stroke", "line", "Stroke"}, {"topo.cylinder", "cylinder", "Cylinder"},
         {"topo.region", "wand", "Region"}, {"topo.handles", "hand-move", "Handles"}, {"topo.relax", "wave-sine", "Relax"}},
        tools);
    root->addWidget(section("Tools", toolsBox, body));

    // Settings shared by the tools.
    auto* settingsBox = new QWidget(body);
    auto* form = new QFormLayout(settingsBox);
    form->setContentsMargins(4, 0, 4, 4);
    PrefStore& prefs = PrefStore::get();
    auto doubleSpin = [&](const char* key, double def, double lo, double hi, int decimals, const char* suffix, const char* tip, const char* label) {
        auto* s = new QDoubleSpinBox(settingsBox);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSuffix(suffix);
        s->setKeyboardTracking(false);
        s->setValue(prefs.value<double>(kPrefs, key, def));
        s->setToolTip(tip);
        s->setObjectName(key);
        setSpinDefault(s, def);
        connect(s, &QDoubleSpinBox::valueChanged, this, [key](double v) { PrefStore::get().set(kPrefs, key, v); });
        form->addRow(label, s);
    };
    auto intSpin = [&](const char* key, int def, int lo, int hi, const char* tip, const char* label) {
        auto* s = new QSpinBox(settingsBox);
        s->setRange(lo, hi);
        s->setKeyboardTracking(false);
        s->setValue(prefs.value<int>(kPrefs, key, def));
        s->setToolTip(tip);
        s->setObjectName(key);
        setSpinDefault(s, def);
        connect(s, &QSpinBox::valueChanged, this, [key](int v) { PrefStore::get().set(kPrefs, key, v); });
        form->addRow(label, s);
    };
    doubleSpin("/topology/brush", 40.0, 4.0, 600.0, 0, " px", "Brush radius on screen: strip width, region and relax brushes, handle reach (Shift-wheel)", "Brush");
    doubleSpin("/topology/edgeLength", 0.0, 0.0, 1e5, 2, " cm", "Edge length new constructs aim for; 0 matches the retopo mesh's own", "Edge length");
    intSpin("/topology/stripRows", 1, 1, 50, "Quads across a new strip", "Strip rows");
    intSpin("/topology/cylinderPoints", 8, 3, 512, "Points around a new cylinder's rings", "Cylinder points");
    doubleSpin("/topology/relax/evenSize", 0.5, 0.0, 1.0, 2, "", "Relax: 0 keeps each face's size, 1 evens every face to one size", "Even size");
    doubleSpin("/topology/relax/squareness", 0.7, 0.0, 1.0, 2, "", "Relax: 0 only evens spacing, 1 pulls faces fully square", "Squareness");
    doubleSpin("/topology/relax/stiffness", 0.5, 0.05, 1.0, 2, "", "Relax: how far each step goes", "Stiffness");
    doubleSpin("/topology/handleFalloff", 0.5, 0.0, 1.0, 2, "", "Handles: how much of the region fades out toward its border", "Handle falloff");
    root->addWidget(section("Settings", settingsBox, body));

    // The active construct.
    auto* activeBox = new QWidget(body);
    auto* activeLayout = new QVBoxLayout(activeBox);
    activeLayout->setContentsMargins(4, 0, 4, 4);
    mActiveLabel = new QLabel(activeBox);
    activeLayout->addWidget(mActiveLabel);
    mConstructPanel = new QWidget(activeBox);
    mConstructLayout = new QVBoxLayout(mConstructPanel);
    mConstructLayout->setContentsMargins(0, 0, 0, 0);
    activeLayout->addWidget(mConstructPanel);
    auto* activeButtons = new QHBoxLayout();
    auto* freeze = new QPushButton("Freeze", activeBox);
    freeze->setToolTip("Leave the active construct as plain faces (Enter)");
    connect(freeze, &QPushButton::clicked, this, [] {
        TopologyTool* t = activeTopologyTool();
        if (t && t->activeConstruct())
            runCommand("retopoConstructFreeze", {{"mesh", t->retopoName()}, {"ids", {t->activeConstruct()}}});
    });
    auto* remove = new QPushButton("Delete", activeBox);
    remove->setToolTip("Delete the active construct and its faces");
    connect(remove, &QPushButton::clicked, this, [] {
        TopologyTool* t = activeTopologyTool();
        if (t && t->activeConstruct())
            runCommand("retopoConstructDelete", {{"mesh", t->retopoName()}, {"ids", {t->activeConstruct()}}});
    });
    activeButtons->addWidget(freeze);
    activeButtons->addWidget(remove);
    activeLayout->addLayout(activeButtons);
    root->addWidget(section("Active Construct", activeBox, body));

    // Every live construct.
    auto* listBox = new QWidget(body);
    auto* listLayout = new QVBoxLayout(listBox);
    listLayout->setContentsMargins(4, 0, 4, 4);
    mList = new QListWidget(listBox);
    mList->setMinimumHeight(90);
    mList->setToolTip("Live constructs on the retopo mesh; click one to make it active");
    connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        int id = item->data(Qt::UserRole).toInt();
        if (TopologyTool* t = activeTopologyTool())
            t->setActiveConstruct(id);
        mRefresh->start();
    });
    listLayout->addWidget(mList);
    FlowLayout* listRow = nullptr;
    QWidget* listButtons = flowBox(listBox, &listRow);
    row(listButtons, {{"topo.freezeAll", "snowflake", "Freeze All"}}, listRow);
    auto* clearGuides = new QToolButton(listButtons);
    clearGuides->setText("Clear Guides");
    clearGuides->setIcon(icon("eraser"));
    clearGuides->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    clearGuides->setToolTip("Remove every guide drawn with the Pencil");
    connect(clearGuides, &QToolButton::clicked, this, [] {
        if (scene::Node* n = sessionRetopo())
            runCommand("retopoSetGuides", {{"mesh", n->path()}, {"guides", json::array()}});
    });
    listRow->addWidget(clearGuides);
    listLayout->addWidget(listButtons);
    root->addWidget(section("Constructs", listBox, body));

    // Auto retopo and cleanup.
    FlowLayout* autoRow = nullptr;
    QWidget* autoBox = flowBox(body, &autoRow);
    row(autoBox,
        {{"topo.runRegion", "wand", "Retopo Region"}, {"mesh.quadRemesh", "grid-4x4", "Quad Remesh"}, {"retopo.poles", "point", "Poles"},
         {"mesh.voxelRemesh", "box", "Voxel Remesh"}, {"topo.settle", "wave-sine", "Settle"}, {"mesh.relax", "wave-square", "Relax"}},
        autoRow);
    root->addWidget(section("Auto Retopo", autoBox, body));

    // Quality.
    auto* qualityBox = new QWidget(body);
    auto* qualityLayout = new QVBoxLayout(qualityBox);
    qualityLayout->setContentsMargins(4, 0, 4, 4);
    FlowLayout* qualityRow = nullptr;
    QWidget* qualityButtons = flowBox(qualityBox, &qualityRow);
    if (QToolButton* b = actionButton("topo.qualityOverlay", "chart-dots", "Overlay", qualityButtons))
    {
        b->setCheckable(true);
        qualityRow->addWidget(b);
    }
    qualityLayout->addWidget(qualityButtons);
    mStats = new QLabel(qualityBox);
    mStats->setWordWrap(true);
    qualityLayout->addWidget(mStats);
    root->addWidget(section("Quality", qualityBox, body));
    root->addStretch(1);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(60);
    connect(mRefresh, &QTimer::timeout, this, [this] { refreshState(); });
    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) { mRefresh->start(); });
    mToolListener = viewportTools::addListener([this] { mRefresh->start(); });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == kPrefs)
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
    refreshState();
}

TopologyToolkit::~TopologyToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    viewportTools::removeListener(mToolListener);
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* TopologyToolkit::actionButton(const std::string& id, const QString& iconName, const QString& text, QWidget* parent)
{
    if (!actions::find(id))
        return nullptr;
    auto* b = new QToolButton(parent);
    b->setIcon(icon(iconName));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    b->setProperty("lilaAction", QString::fromStdString(id));
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

void TopologyToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    for (QToolButton* b : mButtons)
    {
        std::string id = b->property("lilaAction").toString().toStdString();
        if (actions::find(id))
            b->setToolTip(QString::fromStdString(actions::tooltip(id)));
    }
    refreshState();
}

void TopologyToolkit::refreshState()
{
    TopologyTool* tool = activeTopologyTool();
    for (QToolButton* b : mButtons)
    {
        std::string id = b->property("lilaAction").toString().toStdString();
        if (const Action* a = actions::find(id); a && a->checked)
        {
            b->setCheckable(true);
            b->setChecked(a->checked());
        }
    }
    scene::NodeId refId = scene::kNoNode;
    scene::Node* retopo = sessionRetopo(&refId);
    scene::Node* ref = scene::Graph::current().node(refId);
    mSession->setText(QString("Reference: %1\nRetopo: %2")
                          .arg(ref ? QString::fromStdString(ref->path()) : QString("(select one)"))
                          .arg(retopo ? QString::fromStdString(retopo->path()) : QString("(made on first edit)")));
    if (auto* sym = findChild<QComboBox*>("topologySymmetry"))
    {
        QSignalBlocker block(sym);
        sym->setEnabled(retopo != nullptr);
        sym->setCurrentIndex(retopo ? int(std::clamp<int64_t>(retopo->as<int64_t>(scene::MeshNode::kSymmetry), 0, 3)) : 0);
    }
    for (QDoubleSpinBox* s : findChildren<QDoubleSpinBox*>())
        if (s->objectName().startsWith("/topology/") && !s->hasFocus())
        {
            QSignalBlocker block(s);
            s->setValue(PrefStore::get().value<double>(kPrefs, s->objectName().toStdString(), s->value()));
        }

    std::vector<retopo::ConstructRecord> records;
    if (retopo)
        records = retopo::retopoConstructs(scene::Graph::current(), *retopo);
    mList->clear();
    for (const retopo::ConstructRecord& r : records)
    {
        auto* item = new QListWidgetItem(QString("#%1  %2%3").arg(r.id).arg(kindLabel(r.kind)).arg(r.live ? "" : "  (edited, freezes on next change)"), mList);
        item->setData(Qt::UserRole, r.id);
        if (tool && tool->activeConstruct() == r.id)
            item->setSelected(true);
    }
    rebuildConstructPanel();

    QString stats = "No retopo mesh yet.";
    if (retopo)
    {
        scene::Graph::current().evaluate();
        if (scene::MeshPtr m = retopo->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
        {
            retopo::QualityMetrics q = retopo::measureQuality(*m);
            auto valence = [&](size_t k) { return k < q.valence.size() ? q.valence[k] : 0; };
            geo::Index high = 0;
            for (size_t k = 6; k < q.valence.size(); ++k)
                high += q.valence[k];
            stats = QString("%1 faces, %2% quads, %3 tris, %4 n-gons\nPoles: %5 three, %6 five, %7 six or more")
                        .arg(q.faces)
                        .arg(q.quadPercent, 0, 'f', 1)
                        .arg(q.triangles)
                        .arg(q.ngons)
                        .arg(valence(3))
                        .arg(valence(5))
                        .arg(high);
        }
    }
    mStats->setText(stats);
}

void TopologyToolkit::rebuildConstructPanel()
{
    TopologyTool* tool = activeTopologyTool();
    scene::Node* retopo = sessionRetopo();
    const retopo::ConstructRecord* active = nullptr;
    std::vector<retopo::ConstructRecord> records;
    if (tool && retopo && tool->activeConstruct())
    {
        records = retopo::retopoConstructs(scene::Graph::current(), *retopo);
        for (const retopo::ConstructRecord& r : records)
            if (r.id == tool->activeConstruct())
                active = &r;
    }
    std::string key = active ? std::to_string(active->id) + active->spec.dump() + std::to_string(active->live) : std::string();
    if (key == mPanelKey)
        return;
    mPanelKey = key;
    while (QLayoutItem* item = mConstructLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    if (!active)
    {
        mActiveLabel->setText(tool ? "None: make or click a construct." : "Start a topology tool to edit constructs.");
        return;
    }
    mActiveLabel->setText(QString("#%1 %2%3").arg(active->id).arg(kindLabel(active->kind)).arg(active->live ? "" : " (edited by hand)"));
    auto* form = new QWidget(mConstructPanel);
    auto* layout = new QFormLayout(form);
    layout->setContentsMargins(0, 0, 0, 0);
    const int id = active->id;
    for (const Field& f : fieldsFor(active->kind))
    {
        std::string fieldKey = f.key;
        double value = f.fallback;
        if (active->spec.contains(f.key) && active->spec[f.key].is_number())
            value = active->spec[f.key].get<double>();
        else if (active->spec.contains(f.key) && active->spec[f.key].is_boolean())
            value = active->spec[f.key].get<bool>() ? 1.0 : 0.0;
        if (value == 0.0 && f.countIndex >= 0 && size_t(f.countIndex) < active->counts.size())
            value = active->counts[f.countIndex];
        auto apply = [id, fieldKey](json v) {
            if (TopologyTool* t = activeTopologyTool())
                t->editConstruct(id, [&](json& spec) { spec[fieldKey] = v; });
        };
        QWidget* w = nullptr;
        if (f.type == 'b')
        {
            auto* c = new QCheckBox(form);
            c->setChecked(value != 0.0);
            connect(c, &QCheckBox::toggled, form, [apply](bool on) { apply(on); });
            w = c;
        }
        else if (f.type == 'i')
        {
            auto* s = new QSpinBox(form);
            s->setRange(int(f.min), int(f.max));
            s->setValue(int(std::lround(value)));
            s->setKeyboardTracking(false);
            connect(s, &QSpinBox::valueChanged, form, [apply](int v) { apply(v); });
            w = s;
        }
        else
        {
            auto* s = new QDoubleSpinBox(form);
            s->setRange(f.min, f.max);
            s->setDecimals(2);
            s->setValue(value);
            s->setKeyboardTracking(false);
            connect(s, &QDoubleSpinBox::valueChanged, form, [apply](double v) { apply(v); });
            w = s;
        }
        w->setToolTip(f.tip);
        layout->addRow(f.label, w);
    }
    if (active->kind == ConstructKind::Patch)
    {
        auto* reset = new QPushButton("Reset Pull", form);
        reset->setToolTip("Put the patch's poles back where its pattern places them (double-click the patch)");
        connect(reset, &QPushButton::clicked, form, [id] {
            if (TopologyTool* t = activeTopologyTool())
                t->editConstruct(id, [](json& spec) { spec.erase("pull"); });
        });
        layout->addRow(reset);
    }
    if (active->kind == ConstructKind::Cylinder)
    {
        auto* finish = new QPushButton("Finish Cylinder", form);
        finish->setToolTip("The next line starts a new cylinder (Enter freezes this one instead)");
        connect(finish, &QPushButton::clicked, form, [] {
            if (TopologyTool* t = activeTopologyTool())
                t->finishCylinder();
        });
        layout->addRow(finish);
    }
    mConstructLayout->addWidget(form);
}

void registerTopologyToolkitActions()
{
    actions::add({.id = "window.topologyToolkit", .label = "Topology Toolkit", .category = "Windows", .run = [] {
                      toggleDock(findDock(kTopologyToolkitDock));
                  },
                  .doc = "The retopo session, the topology tools and their settings, live constructs, guides, auto retopo and quality.",
                  .python = "lila.ui.run_action(\"window.topologyToolkit\")"});
}
} // namespace lila::ui
