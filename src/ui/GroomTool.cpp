#include "ui/CurveUi.h"

#include "brush/BrushUi.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "groom/Groom.h"
#include "curve/CurveNodes.h"
#include "groom/Attrs.h"
#include "groom/GroomNodes.h"
#include "groom/GroomPresets.h"
#include "groom/Style.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/AttributeEditor.h"
#include "ui/Icons.h"
#include "ui/NodeHandles.h"
#include "ui/Panels.h"
#include "ui/Tools.h"
#include "ui/UiCommands.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <array>
#include <cmath>
#include <memory>

namespace lila::ui
{
namespace
{
using Point3 = glm::dvec3;
using scene::Graph;
using scene::Node;
using scene::NodeId;

enum class HairMode : uint8_t
{
    Lock,
    Braid,
    Rope,
    Ringlet,
    Part,
    Ponytail,
    Bun,
    Bangs,
    Count
};

constexpr std::array<const char*, size_t(HairMode::Count)> kModeNames = {"lock", "braid", "rope", "ringlet", "part", "ponytail", "bun", "bangs"};
constexpr std::array<const char*, size_t(HairMode::Count)> kModeLabels = {"Lock", "Braid", "Rope", "Ringlet", "Part", "Ponytail", "Bun", "Bangs"};
constexpr const char* kPrefix = "/drawHair";

/** What the Draw Hair tool remembers between sessions. */
struct DrawHairSettings
{
    HairMode mode = HairMode::Lock;
    /** New lock width relative to the groom's. */
    double width = 1.0;
    /** Height above the scalp the stroke draws at, cm. */
    double lift = 0.4;
    double size = 1.0;
    double period = 4.0;
    double partRadius = 3.0;
    int presetCount = 16;
    double presetLength = 20.0;
    double presetRadius = 6.0;
};

std::string key(const char* k) { return std::string(kPrefix) + "/" + k; }

DrawHairSettings& drawHairSettings()
{
    static DrawHairSettings s = [] {
        DrawHairSettings c;
        const PrefStore& p = PrefStore::get();
        const std::string mode = p.value<std::string>("general", key("mode"), "lock");
        for (size_t k = 0; k < kModeNames.size(); ++k)
            if (mode == kModeNames[k])
                c.mode = HairMode(k);
        c.width = std::clamp(p.value<double>("general", key("width"), c.width), 0.05, 20.0);
        c.lift = std::max(0.0, p.value<double>("general", key("lift"), c.lift));
        c.size = std::max(0.01, p.value<double>("general", key("size"), c.size));
        c.period = std::max(0.05, p.value<double>("general", key("period"), c.period));
        c.partRadius = std::max(0.01, p.value<double>("general", key("partRadius"), c.partRadius));
        c.presetCount = std::clamp(p.value<int>("general", key("presetCount"), c.presetCount), 1, 4096);
        c.presetLength = std::max(0.01, p.value<double>("general", key("presetLength"), c.presetLength));
        c.presetRadius = std::max(0.01, p.value<double>("general", key("presetRadius"), c.presetRadius));
        return c;
    }();
    return s;
}

void saveDrawHair()
{
    syncGroomPalette();
    const DrawHairSettings& c = drawHairSettings();
    PrefStore& p = PrefStore::get();
    p.set("general", key("mode"), std::string(kModeNames[size_t(c.mode)]));
    p.set("general", key("width"), c.width);
    p.set("general", key("lift"), c.lift);
    p.set("general", key("size"), c.size);
    p.set("general", key("period"), c.period);
    p.set("general", key("partRadius"), c.partRadius);
    p.set("general", key("presetCount"), c.presetCount);
    p.set("general", key("presetLength"), c.presetLength);
    p.set("general", key("presetRadius"), c.presetRadius);
    viewportTools::requestRedraw();
}

json vec(const Point3& v) { return json::array({v.x, v.y, v.z}); }

/** The groom of the selection, or of the only groom in the scene. */
Node* selectedGroom(Graph& g)
{
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gr = groom::findGroom(g, *n))
                return gr;
    Node* only = nullptr;
    int count = 0;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("groom"))
        {
            only = n;
            ++count;
        }
    return count == 1 ? only : nullptr;
}

constexpr const char* kGroomPaletteId = "groomPalette";
constexpr const char* kGroomKeysPref = "/groom/keys";

struct GroomPaletteButton
{
    const char* icon;
    const char* label;
};
constexpr std::array<GroomPaletteButton, size_t(HairMode::Count)> kModeButtons = {{{"line", "Lock"},
                                                                              {"arrows-shuffle", "Braid"},
                                                                              {"link", "Rope"},
                                                                              {"spiral", "Ringlet"},
                                                                              {"separator-vertical", "Part"},
                                                                              {"pennant", "Ponytail"},
                                                                              {"lifebuoy", "Bun"},
                                                                              {"grip-horizontal", "Bangs"}}};
constexpr std::array<GroomPaletteButton, size_t(groom::Brush::Count)> kBrushButtons = {{{"hand-move", "Comb"},
                                                                                   {"blur", "Smooth"},
                                                                                   {"ruler", "Length"},
                                                                                   {"arrows-maximize", "Puff"},
                                                                                   {"circle-plus", "Add"},
                                                                                   {"eraser", "Delete"},
                                                                                   {"grid-dots", "Density"},
                                                                                   {"scissors", "Cut"},
                                                                                   {"arrows-minimize", "Pinch"},
                                                                                   {"arrows-move-horizontal", "Slide"},
                                                                                   {"hand-grab", "Grab"},
                                                                                   {"rotate-clockwise", "Twist"},
                                                                                   {"arrow-autofit-width", "Widen"},
                                                                                   {"fold-down", "Flatten"},
                                                                                   {"curly-loop", "Curl"},
                                                                                   {"sparkles", "Frizz"},
                                                                                   {"magnet", "Clump"},
                                                                                   {"arrows-split", "Split"},
                                                                                   {"arrows-join", "Merge"},
                                                                                   {"arrow-bar-to-down", "Settle"}}};

/** The guides the palette's Mirror X works on: the selected groom's, else a selected curve. */
Node* groomPaletteGuides()
{
    Graph& g = Graph::current();
    if (Node* gr = selectedGroom(g))
        return groom::guidesOf(g, *gr);
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("curve"))
            return n;
    return nullptr;
}

/**
 * The Groom palette, shown while Draw Hair or Sculpt Curves is active: what the next stroke draws or which brush paints,
 * their settings, and the guides' mirror. Number keys stay with the app.
 */
class GroomPalette final : public QWidget
{
public:
    explicit GroomPalette(QWidget* parent) : QWidget(parent)
    {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(8, 8, 8, 8);
        v->setSpacing(8);
        auto iconButton = [this](const GroomPaletteButton& def, std::function<void()> fn) {
            auto* b = new QToolButton;
            b->setIcon(icon(def.icon));
            b->setIconSize(QSize(22, 22));
            b->setFixedSize(36, 36);
            b->setCheckable(true);
            b->setAutoRaise(true);
            b->setToolTip(def.label);
            connect(b, &QToolButton::clicked, this, std::move(fn));
            return b;
        };

        mDraw = new QWidget;
        auto* dv = new QVBoxLayout(mDraw);
        dv->setContentsMargins(0, 0, 0, 0);
        auto* modes = new QGridLayout;
        modes->setSpacing(2);
        for (size_t i = 0; i < kModeButtons.size(); ++i)
        {
            mModes[i] = iconButton(kModeButtons[i], [i] {
                drawHairSettings().mode = HairMode(i);
                saveDrawHair();
            });
            modes->addWidget(mModes[i], int(i / 4), int(i % 4));
        }
        dv->addLayout(modes);
        mModeName = new QLabel;
        mModeName->setAlignment(Qt::AlignHCenter);
        dv->addWidget(mModeName);
        auto* form = new QFormLayout;
        auto spin = [&](const char* label, double lo, double hi, const char* suffix, const char* tip, double DrawHairSettings::*field) {
            auto* s = new QDoubleSpinBox;
            s->setRange(lo, hi);
            s->setDecimals(2);
            s->setSingleStep(0.1);
            s->setSuffix(suffix);
            s->setToolTip(tip);
            s->setKeyboardTracking(false);
            connect(s, &QDoubleSpinBox::valueChanged, this, [field](double value) {
                drawHairSettings().*field = value;
                saveDrawHair();
            });
            form->addRow(label, s);
            return s;
        };
        mWidth = spin("Width", 0.05, 20.0, "x", "New locks' width, times the groom's  ([ ], Ctrl+scroll)", &DrawHairSettings::width);
        mLift = spin("Lift", 0.0, 100.0, " cm", "How far above the scalp the stroke draws.", &DrawHairSettings::lift);
        mSize = spin("Size", 0.01, 100.0, " cm", "Braid and rope half width, ringlet radius.", &DrawHairSettings::size);
        mPeriod = spin("Turn", 0.05, 500.0, " cm", "Length of one turn of a braid, rope or ringlet.", &DrawHairSettings::period);
        mPartRadius = spin("Reach", 0.01, 100.0, " cm", "How far from a parting line locks turn.", &DrawHairSettings::partRadius);
        mLength = spin("Length", 0.01, 1000.0, " cm", "Length of a ponytail, bun or bangs.", &DrawHairSettings::presetLength);
        mCount = new QSpinBox;
        mCount->setRange(1, 4096);
        mCount->setToolTip("Locks in a ponytail, bun or bangs.");
        connect(mCount, &QSpinBox::valueChanged, this, [](int value) {
            drawHairSettings().presetCount = value;
            saveDrawHair();
        });
        form->addRow("Count", mCount);
        dv->addLayout(form);
        v->addWidget(mDraw);

        mSculpt = new QWidget;
        auto* sv = new QVBoxLayout(mSculpt);
        sv->setContentsMargins(0, 0, 0, 0);
        for (int set = 0; set < 2; ++set)
        {
            auto* title = new QLabel(set ? "Locks" : "Strands");
            sv->addWidget(title);
            auto* grid = new QGridLayout;
            grid->setSpacing(2);
            const int first = set ? int(groom::kFirstLockBrush) : 0, last = set ? int(groom::Brush::Count) : int(groom::kFirstLockBrush);
            for (int i = first; i < last; ++i)
            {
                mBrushes[size_t(i)] = iconButton(kBrushButtons[size_t(i)], [i] { setCurveSculptBrush(i); });
                grid->addWidget(mBrushes[size_t(i)], (i - first) / 5, (i - first) % 5);
            }
            sv->addLayout(grid);
        }
        mBrushName = new QLabel;
        mBrushName->setAlignment(Qt::AlignHCenter);
        sv->addWidget(mBrushName);
        mSettleGrab = new QCheckBox("Settle While Grabbing");
        mSettleGrab->setToolTip("Grab settles the locks around it, so they make room.");
        connect(mSettleGrab, &QCheckBox::clicked, this, [](bool on) { setCurveSculptSettleGrab(on); });
        sv->addWidget(mSettleGrab);
        v->addWidget(mSculpt);

        mMirror = new QCheckBox("Mirror X");
        connect(mMirror, &QCheckBox::clicked, this, [](bool on) {
            if (Node* guides = groomPaletteGuides())
                runUi("setAttr", {{"args", {guides->path() + ".symmetry", on ? "x" : "off"}}});
        });
        v->addWidget(mMirror);
        mKeys = new QCheckBox("Show keys");
        mKeys->setToolTip("List the tool's keys in the corner of the view.");
        connect(mKeys, &QCheckBox::clicked, this, [](bool on) {
            PrefStore::get().set("general", kGroomKeysPref, on);
            viewportTools::requestRedraw();
        });
        v->addWidget(mKeys);
        v->addStretch();

        mRefresh = new QTimer(this);
        mRefresh->setSingleShot(true);
        mRefresh->setInterval(60);
        connect(mRefresh, &QTimer::timeout, this, [this] { sync(); });
        mGraphListener = Graph::current().addListener([this](scene::Change, NodeId) { mRefresh->start(); });
        sync();
    }

    ~GroomPalette() override { Graph::current().removeListener(mGraphListener); }

    void setPage(bool draw)
    {
        mDrawPage = draw;
        sync();
    }

    void sync()
    {
        mDraw->setVisible(mDrawPage);
        mSculpt->setVisible(!mDrawPage);
        const DrawHairSettings& s = drawHairSettings();
        for (size_t i = 0; i < mModes.size(); ++i)
            mModes[i]->setChecked(HairMode(i) == s.mode);
        mModeName->setText(kModeLabels[size_t(s.mode)]);
        for (auto [box, value] : {std::pair{mWidth, s.width}, std::pair{mLift, s.lift}, std::pair{mSize, s.size}, std::pair{mPeriod, s.period},
                                  std::pair{mPartRadius, s.partRadius}, std::pair{mLength, s.presetLength}})
        {
            const QSignalBlocker block(box);
            box->setValue(value);
        }
        {
            const QSignalBlocker block(mCount);
            mCount->setValue(s.presetCount);
        }
        const bool plait = s.mode == HairMode::Braid || s.mode == HairMode::Rope || s.mode == HairMode::Ringlet;
        const bool preset = s.mode == HairMode::Ponytail || s.mode == HairMode::Bun || s.mode == HairMode::Bangs;
        mSize->setEnabled(plait);
        mPeriod->setEnabled(plait);
        mPartRadius->setEnabled(s.mode == HairMode::Part);
        mLength->setEnabled(preset);
        mCount->setEnabled(preset);
        const int brush = curveSculptBrush();
        for (size_t i = 0; i < mBrushes.size(); ++i)
            mBrushes[i]->setChecked(int(i) == brush);
        mBrushName->setText(kBrushButtons[size_t(std::clamp(brush, 0, int(mBrushes.size()) - 1))].label);
        mSettleGrab->setChecked(curveSculptSettleGrab());
        mSettleGrab->setEnabled(brush == int(groom::Brush::Grab));
        const Node* guides = groomPaletteGuides();
        mMirror->setEnabled(guides);
        mMirror->setChecked(guides && curve::symmetryAxis(*guides) == 0);
        mMirror->setToolTip(guides ? QString("Mirror %1 across X.").arg(QString::fromStdString(guides->name())) : "Select a groom to mirror its guides.");
        mKeys->setChecked(groomKeysShown());
    }

private:
    bool mDrawPage = true;
    QWidget *mDraw = nullptr, *mSculpt = nullptr;
    std::array<QToolButton*, kModeButtons.size()> mModes{};
    std::array<QToolButton*, kBrushButtons.size()> mBrushes{};
    QLabel *mModeName = nullptr, *mBrushName = nullptr;
    QDoubleSpinBox *mWidth = nullptr, *mLift = nullptr, *mSize = nullptr, *mPeriod = nullptr, *mPartRadius = nullptr, *mLength = nullptr;
    QSpinBox* mCount = nullptr;
    QCheckBox *mSettleGrab = nullptr, *mMirror = nullptr, *mKeys = nullptr;
    QTimer* mRefresh = nullptr;
    int mGraphListener = 0;
};

QPointer<GroomPalette> gGroomPalette;

QDockWidget* groomPaletteDock()
{
    for (QWidget* w = gGroomPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

/**
 * Draw Hair: strokes on the scalp become locks, braids, ropes or ringlets; a stroke can also part the hair, or place a
 * ponytail, bun or bangs. The stroke hugs the scalp at the lift height and carries on in the view plane past its edge,
 * so a lock can be drawn falling off the head. Pen pressure is the width along it.
 */
class DrawHairTool final : public ViewportTool
{
public:
    const char* name() const override { return "Draw Hair"; }

    void activate() override
    {
        Graph& g = Graph::current();
        Node* gr = selectedGroom(g);
        if (!gr)
        {
            // A mesh selected on its own becomes the scalp of a new groom.
            for (NodeId id : g.selection())
                if (Node* n = g.node(id); n && n->type().isA("mesh"))
                {
                    const json made = runUi("createGroom", argsCall({n->path()}));
                    if (made.is_object())
                        gr = g.find(made.value("groom", std::string()));
                    break;
                }
        }
        mGroom = gr ? gr->id() : scene::kNoNode;
        mControls.activate();
        showGroomPalette(true);
        if (!gr)
            log::warn("Draw Hair: select a groom, or the mesh hair grows from");
    }

    void deactivate() override
    {
        mControls.deactivate();
        hideGroomPalette();
        mStroke.clear();
        mPressure.clear();
        viewportTools::requestRedraw();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        if (!groomNode())
        {
            log::warn("Draw Hair: select a groom, or the mesh hair grows from");
            return true;
        }
        mStroke.clear();
        mPressure.clear();
        Point3 p;
        if (!point(e->position(), view, true, p))
            return true;
        mStroke.push_back(p);
        mPressure.push_back(mControls.input(e).pressure);
        mLastPx = e->position();
        mStroking = true;
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mStroking)
            return false;
        if (QLineF(mLastPx, e->position()).length() < 4.0)
            return true;
        Point3 p;
        if (point(e->position(), view, false, p))
        {
            mStroke.push_back(p);
            mPressure.push_back(mControls.input(e).pressure);
            mLastPx = e->position();
            viewportTools::requestRedraw();
        }
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mStroking)
            return false;
        mStroking = false;
        finish();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ControlModifier))
            return false;
        DrawHairSettings& s = drawHairSettings();
        s.width = std::clamp(s.width * std::pow(1.1, e->angleDelta().y() / 120.0), 0.05, 20.0);
        saveDrawHair();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override { return key(e->key()); }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        if (e.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            return false;
        // [ ] are app hotkeys too; the tool takes them for the lock width while active.
        const int k = e.key();
        return (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight) && key(k);
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();
        const DrawHairSettings& s = drawHairSettings();
        const glm::vec4 col(0.95f, 0.75f, 0.35f, 1.0f);
        for (size_t i = 1; i < mStroke.size(); ++i)
        {
            f.lines.push_back({glm::vec3(mStroke[i - 1]), col});
            f.lines.push_back({glm::vec3(mStroke[i]), col});
        }
        Node* gr = groomNode();
        Node* guides = gr ? groom::guidesOf(Graph::current(), *gr) : nullptr;
        const bool mirrored = guides && groom::mirrorPlane(*guides);
        drawToolStatus(p, view,
                       {.tool = "Draw Hair",
                        .subject = gr ? QString::fromStdString(gr->name()) : QString(),
                        .mode = kModeLabels[size_t(s.mode)],
                        .fields = {{"width", QString::number(s.width, 'f', 2)}},
                        .prompt = gr ? QString() : QString("select a groom or a scalp mesh"),
                        .symmetry = mirrored ? std::optional<ToolSymmetry>(ToolSymmetry{true, QString("XYZ").mid(curve::symmetryAxis(*guides), 1)}) : std::nullopt});
        if (groomKeysShown())
            drawKeyList(p, QPointF(16.0, view.height - 84.0), keys(mirrored));
    }

private:
    Node* groomNode() const
    {
        Node* n = Graph::current().node(mGroom);
        return n && n->type().isA("groom") ? n : nullptr;
    }

    static std::vector<KeyHint> keys(bool mirrored)
    {
        const HairMode m = drawHairSettings().mode;
        const char* what = m == HairMode::Part ? "Part" : m == HairMode::Ponytail || m == HairMode::Bun ? "Place" : "Draw";
        std::vector<KeyHint> out = {{"Drag", QString("%1 %2").arg(what, QString(kModeLabels[size_t(m)]).toLower())}};
        if (mirrored)
            out.push_back({"X+drag", "Snap to mirror"});
        out.insert(out.end(), {{"[  ]", "Narrower, wider"}, {"Ctrl+scroll", "Width"}, {"Esc", "Exit"}});
        return out;
    }

    bool key(int k)
    {
        DrawHairSettings& s = drawHairSettings();
        if (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight)
        {
            s.width = std::clamp(s.width * (k == Qt::Key_BracketRight ? 1.1 : 1.0 / 1.1), 0.05, 20.0);
            saveDrawHair();
            return true;
        }
        if (k == Qt::Key_Escape)
        {
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        }
        return false;
    }

    /**
     * With the guides mirrored, a stroke whose root is within a lock's width of the mirror plane roots on it, and one
     * that runs along the plane lies on it: one centred lock rather than a pair hugging the middle. X widens the reach.
     */
    static void snapToMirror(Node& gr, std::vector<Point3>& pts)
    {
        Graph& g = Graph::current();
        Node* guides = groom::guidesOf(g, gr);
        const auto plane = guides ? groom::mirrorPlane(*guides) : std::nullopt;
        if (!plane || pts.empty())
            return;
        const auto [at, n] = *plane;
        const double half = 0.5 * groom::lockOptionsOf(gr).width * drawHairSettings().width;
        const double reach = (tools::snaps() & SnapGrid) ? 4.0 * half : half;
        auto offset = [&](const Point3& p) { return glm::dot(p - at, n); };
        if (std::ranges::all_of(pts, [&](const Point3& p) { return std::abs(offset(p)) < reach; }))
            for (Point3& p : pts)
                p -= n * offset(p);
        else if (std::abs(offset(pts.front())) < reach)
            pts.front() -= n * offset(pts.front());
    }

    /** The scalp in world space, rebuilt when its mesh or matrix changes. */
    const groom::Surface* scalp()
    {
        Node* gr = groomNode();
        if (!gr)
            return nullptr;
        Graph& g = Graph::current();
        Node* guides = groom::guidesOf(g, *gr);
        Node* mesh = guides ? g.find(groom::groomSurfaceOf(*guides)) : nullptr;
        if (!mesh || !mesh->type().isA("mesh"))
            return nullptr;
        g.evaluate();
        scene::MeshPtr m = mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        const glm::dmat4 w = mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        if (m.get() != mScalpMesh || w != mScalpMatrix || !mScalp)
        {
            mScalpMesh = m.get();
            mScalpMatrix = w;
            mScalp.reset();
            if (m && m->numFaces() > 0)
            {
                geo::Mesh c = *m;
                for (geo::Index pi = 0; pi < c.pointCapacity(); ++pi)
                    if (c.pointValid(pi))
                        c.setPosition(pi, Point3(w * glm::dvec4(c.position(pi), 1.0)));
                mScalp = std::make_unique<groom::Surface>(std::move(c));
            }
        }
        return mScalp.get();
    }

    /** Where the stroke is at px: on the scalp lifted off it, else on the plane through the last point facing the view. */
    bool point(QPointF px, const ViewInfo& view, bool first, Point3& out)
    {
        Point3 o, d;
        view.ray(px, o, d);
        d = glm::normalize(d);
        const DrawHairSettings& s = drawHairSettings();
        const bool parting = s.mode == HairMode::Part;
        if (const groom::Surface* sc = scalp())
            if (auto h = sc->raycast(o, d))
            {
                // The first point sits on the scalp: it is the root.
                out = h->point + h->normal * (first || parting ? 0.0 : s.lift);
                return true;
            }
        if (first || mStroke.empty())
            return false;
        // Off the edge of the head: keep drawing at the depth of the last point.
        const Point3 anchor = mStroke.back();
        const Point3 n = -view.viewDir(anchor);
        const double den = glm::dot(d, n);
        if (std::abs(den) < 1e-9)
            return false;
        const double t = glm::dot(anchor - o, n) / den;
        if (t <= 0.0)
            return false;
        out = o + d * t;
        return true;
    }

    void finish()
    {
        Node* gr = groomNode();
        const DrawHairSettings& s = drawHairSettings();
        std::vector<Point3> pts = std::move(mStroke);
        std::vector<double> pressure = std::move(mPressure);
        mStroke.clear();
        mPressure.clear();
        viewportTools::requestRedraw();
        if (!gr || pts.empty())
            return;
        snapToMirror(*gr, pts);
        json points = json::array();
        for (const Point3& p : pts)
            points.push_back(vec(p));
        const json args = {gr->path()};
        switch (s.mode)
        {
        case HairMode::Lock:
        case HairMode::Braid:
        case HairMode::Rope:
        case HairMode::Ringlet:
            if (pts.size() < 2)
                return;
            runUi("groomDraw", {{"args", args},
                                {"points", points},
                                {"pressure", pressure},
                                {"shape", kModeNames[size_t(s.mode)]},
                                {"size", s.size},
                                {"period", s.period},
                                {"width", s.width}});
            break;
        case HairMode::Part:
            if (pts.size() >= 2)
                runUi("groomPart", {{"args", args}, {"points", points}, {"radius", s.partRadius}});
            break;
        case HairMode::Ponytail:
        case HairMode::Bun:
            runUi("groomPreset", {{"args", args},
                                  {"preset", kModeNames[size_t(s.mode)]},
                                  {"at", vec(pts.front())},
                                  {"count", s.presetCount},
                                  {"length", s.presetLength},
                                  {"radius", s.presetRadius}});
            break;
        case HairMode::Bangs:
            if (pts.size() >= 2)
                runUi("groomPreset",
                      {{"args", args}, {"preset", "bangs"}, {"at", vec(pts.front())}, {"to", vec(pts.back())}, {"count", s.presetCount}, {"length", s.presetLength * 0.5}});
            break;
        default: break;
        }
    }

    brush::BrushSettings mBrush;
    brush::ui::BrushControls mControls{[this]() -> brush::BrushSettings& { return mBrush; }, [] {}, {}};
    NodeId mGroom = scene::kNoNode;
    std::vector<Point3> mStroke;
    std::vector<double> mPressure;
    QPointF mLastPx;
    bool mStroking = false;
    const geo::Mesh* mScalpMesh = nullptr;
    glm::dmat4 mScalpMatrix{0.0};
    std::unique_ptr<groom::Surface> mScalp;
};

/** Where a guide's lock handles sit, world space: the width handle on its edge 40% along, the twist handle 85% along. */
struct LockHandles
{
    Point3 widthAt{0.0}, widthCentre{0.0}, twistAt{0.0}, twistCentre{0.0};
    double width = 1.0;
    double twist = 0.0; // degrees at the tip, from the guide's tilt
};

/** Each own guide's handles, or nothing for a curve that isn't a groom's guides. */
std::vector<std::optional<LockHandles>> lockHandles(const Node& curveNode)
{
    Graph& g = Graph::current();
    Node* gr = groom::groomOf(g, curveNode);
    if (!gr)
        return {};
    const curve::Splines& own = curve::shapeSplines(g, curveNode.id());
    const glm::dmat4 w = curveNode.as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const groom::LockOptions lo = groom::lockOptionsOf(*gr);
    std::vector<std::optional<LockHandles>> out(own.size());
    for (size_t k = 0; k < own.size(); ++k)
    {
        curve::Spline s = own[k];
        for (Point3& p : s.points)
            p = Point3(w * glm::dvec4(p, 1.0));
        const curve::Splines r = groom::resolveLocks({s}, nullptr, lo);
        if (r.empty() || r[0].size() < 2)
            continue;
        const curve::Spline& lock = r[0];
        auto at = [&](double t, Point3& centre, Point3& edge) {
            const size_t i = std::min(lock.size() - 1, size_t(std::lround(t * double(lock.size() - 1))));
            const Point3 side(groom::attr::point(lock, groom::attr::kSideX, i, 1.0), groom::attr::point(lock, groom::attr::kSideY, i, 0.0),
                              groom::attr::point(lock, groom::attr::kSideZ, i, 0.0));
            centre = lock.points[i];
            edge = centre + side * groom::attr::point(lock, groom::attr::kHalfWidth, i, 0.5);
        };
        LockHandles h;
        at(0.4, h.widthCentre, h.widthAt);
        at(0.85, h.twistCentre, h.twistAt);
        h.width = groom::attr::get(own[k], groom::attr::kWidth, 1.0);
        h.twist = own[k].tilt.empty() ? 0.0 : glm::degrees(own[k].tilt.back());
        out[k] = h;
    }
    return out;
}

/** Drags a guide's width or twist: live on the guides with undo off, landing as one groomEditLock. */
class LockHandleDrag final : public nodeHandles::Drag
{
public:
    LockHandleDrag(NodeId guides, size_t index, bool twist, const LockHandles& h, QPointF press, const ViewInfo& view)
        : mGuides(guides), mIndex(index), mTwist(twist), mStart(h), mPress(press)
    {
        Graph& g = Graph::current();
        mStartValue = g.value({guides, curve::CurveShape::kCreate, -1});
        if (auto c = view.project(twist ? h.twistCentre : h.widthCentre))
            mCentre = *c;
        mValue = twist ? h.twist : h.width;
    }

    void move(QPointF px, const ViewInfo&, Qt::KeyboardModifiers mods) override
    {
        const double fine = (mods & Qt::ControlModifier) ? 0.2 : 1.0;
        if (mTwist)
            mValue = mStart.twist + (px.x() - mPress.x()) * fine;
        else
        {
            const double d0 = std::max(1.0, QLineF(mCentre, mPress).length());
            mValue = std::max(0.01, mStart.width * (1.0 + (QLineF(mCentre, px).length() / d0 - 1.0) * fine));
        }
        Graph& g = Graph::current();
        curve::Splines s = curve::splinesOf(mStartValue);
        if (mIndex >= s.size())
            return;
        apply(s[mIndex]);
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({mGuides, curve::CurveShape::kCreate, -1}, curve::makeCurveData(std::move(s)));
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void release(bool moved) override
    {
        cancel();
        if (!moved)
            return;
        Node* n = Graph::current().node(mGuides);
        if (!n)
            return;
        json call = {{"args", {n->path()}}, {"index", mIndex}};
        call[mTwist ? "twist" : "width"] = mValue;
        runUi("groomEditLock", call);
    }

    void cancel() override
    {
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        Graph::current().setValue({mGuides, curve::CurveShape::kCreate, -1}, mStartValue);
        u.setEnabled(was);
    }

    QString readout() const override { return mTwist ? QString("Twist %1\xc2\xb0").arg(mValue, 0, 'f', 0) : QString("Width %1x").arg(mValue, 0, 'f', 2); }

private:
    void apply(curve::Spline& sp) const
    {
        if (!mTwist)
        {
            groom::attr::set(sp, groom::attr::kWidth, mValue);
            return;
        }
        const double len = std::max(1e-12, groom::attr::length(sp));
        sp.tilt.assign(sp.size(), 0.0);
        double walked = 0.0;
        for (size_t k = 1; k < sp.size(); ++k)
        {
            walked += glm::distance(sp.points[k - 1], sp.points[k]);
            sp.tilt[k] = glm::radians(mValue) * walked / len;
        }
    }

    NodeId mGuides;
    size_t mIndex;
    bool mTwist;
    LockHandles mStart;
    QPointF mPress, mCentre;
    scene::PlugValue mStartValue;
    double mValue = 1.0;
};

void registerLockHandles()
{
    if (nodeHandles::registry().find("lila.groomLocks"))
        return;
    nodeHandles::add({
        .id = "lila.groomLocks",
        .nodeType = "curve",
        .draw =
            [](OverlayPainter& p, const nodeHandles::Context& c, int hovered, int active) {
                const auto hs = lockHandles(c.node);
                for (size_t k = 0; k < hs.size(); ++k)
                {
                    if (!hs[k])
                        continue;
                    const auto wc = c.view.project(hs[k]->widthCentre), we = c.view.project(hs[k]->widthAt);
                    const auto tc = c.view.project(hs[k]->twistCentre), te = c.view.project(hs[k]->twistAt);
                    const int wid = int(k) * 2, tid = wid + 1;
                    auto colour = [&](int id) {
                        return id == active ? glm::vec4(1.0f, 0.85f, 0.3f, 1.0f) : id == hovered ? glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) : glm::vec4(0.6f, 0.8f, 1.0f, 0.9f);
                    };
                    if (wc && we)
                    {
                        p.line(*wc, *we, 1.0f, glm::vec4(0.6f, 0.8f, 1.0f, 0.5f));
                        p.rect(QRectF(we->x() - 4.0, we->y() - 4.0, 8.0, 8.0), colour(wid));
                    }
                    if (tc && te)
                    {
                        p.line(*tc, *te, 1.0f, glm::vec4(0.6f, 0.8f, 1.0f, 0.5f));
                        p.disc(*te, 4.5f, colour(tid));
                    }
                }
            },
        .pick =
            [](QPointF px, const nodeHandles::Context& c) {
                const auto hs = lockHandles(c.node);
                int best = -1;
                double bestD = 8.0;
                for (size_t k = 0; k < hs.size(); ++k)
                {
                    if (!hs[k])
                        continue;
                    for (int which = 0; which < 2; ++which)
                        if (auto q = c.view.project(which ? hs[k]->twistAt : hs[k]->widthAt))
                            if (double d = QLineF(*q, px).length(); d < bestD)
                            {
                                bestD = d;
                                best = int(k) * 2 + which;
                            }
                }
                return best;
            },
        .begin = [](int handle, QPointF px, const nodeHandles::Context& c) -> std::unique_ptr<nodeHandles::Drag> {
            const auto hs = lockHandles(c.node);
            const size_t k = size_t(handle / 2);
            if (handle < 0 || k >= hs.size() || !hs[k])
                return nullptr;
            return std::make_unique<LockHandleDrag>(c.node.id(), k, handle % 2 == 1, *hs[k], px, c.view);
        },
    });
}

/** The groom's style stack in the Attribute Editor: each modifier with on/off, up, down and remove, and an add menu. */
QWidget* stackPanel(NodeId id)
{
    Graph& g = Graph::current();
    Node* part = g.node(id);
    Node* gr = part ? groom::findGroom(g, *part) : nullptr;
    if (!gr)
        return nullptr;
    g.evaluate();
    auto* w = new QWidget;
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(8, 2, 4, 2);
    const std::string path = gr->path();
    const int locks = int(gr->as<int64_t>(gr->type().attrIndex("lockCount")));
    v->addWidget(new QLabel(QString("%1 locks").arg(locks)));
    for (Node* n : groom::styleStack(g, *gr))
    {
        auto* row = new QHBoxLayout;
        const std::string np = n->path();
        auto* on = new QCheckBox(QString::fromStdString(n->name()));
        on->setChecked(n->as<bool>(n->type().attrIndex("enabled")));
        on->setToolTip(QString::fromStdString(n->type().doc));
        QObject::connect(on, &QCheckBox::toggled, on, [path, np](bool) { runUi("groomStack", {{"args", {path}}, {"edit", "toggle"}, {"node", np}}); });
        row->addWidget(on, 1);
        auto tool = [&](const char* text, const char* tip, std::function<void()> fn) {
            auto* b = new QToolButton;
            b->setText(text);
            b->setToolTip(tip);
            QObject::connect(b, &QToolButton::clicked, b, std::move(fn));
            row->addWidget(b);
        };
        const auto stack = groom::styleStack(g, *gr);
        const int at = int(std::ranges::find(stack, n) - stack.begin());
        tool("\xe2\x96\xb2", "Earlier in the stack", [path, np, at] { runUi("groomStack", {{"args", {path}}, {"edit", "move"}, {"node", np}, {"to", std::max(0, at - 1)}}); });
        tool("\xe2\x96\xbc", "Later in the stack", [path, np, at] { runUi("groomStack", {{"args", {path}}, {"edit", "move"}, {"node", np}, {"to", at + 1}}); });
        tool("\xc3\x97", "Remove", [path, np] { runUi("groomStack", {{"args", {path}}, {"edit", "remove"}, {"node", np}}); });
        v->addLayout(row);
    }
    auto* add = new QComboBox;
    add->addItem("Add Style...");
    for (int k = 0; k < int(groom::Style::Count); ++k)
    {
        QString n = groom::styleName(groom::Style(k));
        add->addItem(n.left(1).toUpper() + n.mid(1), QString(groom::styleName(groom::Style(k))));
    }
    QObject::connect(add, &QComboBox::activated, add, [add, path](int i) {
        if (i <= 0)
            return;
        const std::string style = add->itemData(i).toString().toStdString();
        QTimer::singleShot(0, [path, style] { runUi("groomAddStyle", {{"args", {path}}, {"style", style}}); });
    });
    v->addWidget(add);
    auto* preset = new QComboBox;
    preset->addItem("Apply Preset...");
    for (const groom::presets::Preset& pr : groom::presets::list())
    {
        preset->addItem(QString::fromStdString(pr.name + (pr.guides ? "  (with guides)" : "")), QString::fromStdString(pr.id));
        preset->setItemData(preset->count() - 1, QString::fromStdString(pr.doc), Qt::ToolTipRole);
    }
    QObject::connect(preset, &QComboBox::activated, preset, [preset, path](int i) {
        if (i <= 0)
            return;
        const std::string id = preset->itemData(i).toString().toStdString();
        QTimer::singleShot(0, [path, id] { runUi("groomLoadPreset", {{"args", {path}}, {"preset", id}}); });
    });
    v->addWidget(preset);
    auto* buttons = new QHBoxLayout;
    auto button = [&](const char* text, const char* tip, std::function<void()> fn) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        QObject::connect(b, &QPushButton::clicked, b, std::move(fn));
        buttons->addWidget(b);
    };
    button("Draw", "Draw Hair tool: draw locks, braids, partings and presets on the scalp.", [] { viewportTools::activate("groom.drawHair"); });
    button("Settle", "Settle the guides under gravity onto the scalp and each other.", [path] { runUiWithOptions("groomSettle", argsCall({path})); });
    button("Shell", "One mesh over all the locks, ready for retopology.", [path] { runUiWithOptions("groomShell", argsCall({path})); });
    button("Save Preset", "Save the lock settings and style stack, and optionally the guides, as a hairstyle preset.", [path] {
        bool ok = false;
        const QString name = QInputDialog::getText(QApplication::activeWindow(), "Save Hair Preset", "Name", QLineEdit::Normal, {}, &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        const QStringList kinds = {"Style only", "Style and guides"};
        const QString kind = QInputDialog::getItem(QApplication::activeWindow(), "Save Hair Preset", "Keep", kinds, 0, false, &ok);
        if (ok)
            runUi("groomSavePreset", {{"args", {path}}, {"name", name.trimmed().toStdString()}, {"guides", kind == kinds[1]}});
    });
    v->addLayout(buttons);
    return w;
}
} // namespace

void showGroomPalette(bool drawHair)
{
    QDockWidget* before = groomPaletteDock();
    const bool wasFloating = before && before->isFloating() && before->isVisible();
    panels::show(kGroomPaletteId);
    if (gGroomPalette)
        gGroomPalette->setPage(drawHair);
    QDockWidget* d = groomPaletteDock();
    if (!d || wasFloating || !d->isFloating())
        return;
    // Floated at the viewport's top-left, out of the way of the head.
    d->resize(d->sizeHint());
    if (QWidget* window = d->parentWidget() ? d->parentWidget()->window() : nullptr)
        d->move(window->geometry().topLeft() + QPoint(80, 160));
}

void hideGroomPalette()
{
    if (QDockWidget* d = groomPaletteDock())
        d->hide();
}

void syncGroomPalette()
{
    if (gGroomPalette)
        gGroomPalette->sync();
    viewportTools::requestRedraw();
}

bool groomKeysShown() { return PrefStore::get().value<bool>("general", kGroomKeysPref, true); }

void registerGroomUi()
{
    panels::add({.id = kGroomPaletteId,
                 .title = "Groom",
                 .create = [](QWidget* parent) -> QWidget* { return gGroomPalette = new GroomPalette(parent); },
                 .area = Qt::LeftDockWidgetArea,
                 .icon = "feather",
                 .open = false,
                 .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                 .floatFirst = true});
    if (!viewportTools::registry().find("groom.drawHair"))
        viewportTools::add({.id = "groom.drawHair", .label = "Draw Hair", .icon = "brush", .create = [] { return std::make_unique<DrawHairTool>(); }});
    actions::add({.id = "groom.drawHair",
                  .label = "Draw Hair",
                  .category = "Groom",
                  .run = [] { viewportTools::activate("groom.drawHair"); },
                  .doc = "Draws locks, braids, ropes and ringlets on the scalp with pen pressure as width, parts the hair, and places "
                         "ponytails, buns and bangs. Select a groom, or a mesh to start one on it.",
                  .python = "lila.ui.run_action(\"groom.drawHair\")"});
    actions::add({.id = "groom.create",
                  .label = "New Groom",
                  .category = "Groom",
                  .doc = "Starts a groom on the selected mesh: empty guides with X symmetry, the groom node and its locks mesh.",
                  .python = ops::toPython("createGroom", json::object()),
                  .command = "createGroom",
                  .call = json::object()});
    actions::add({.id = "groom.settle",
                  .label = "Settle Hair",
                  .category = "Groom",
                  .doc = "Settles the selected groom's guides under gravity: roots stay, locks rest on the scalp and on each other.",
                  .python = ops::toPython("groomSettle", json::object()),
                  .command = "groomSettle",
                  .call = json::object()});
    actions::add({.id = "groom.shell",
                  .label = "Hair Shell",
                  .category = "Groom",
                  .doc = "One watertight mesh over the selected groom's locks, joined smoothly, ready for retopology.",
                  .python = ops::toPython("groomShell", json::object()),
                  .command = "groomShell",
                  .call = json::object()});
    for (int k = 0; k < int(groom::Style::Count); ++k)
    {
        const std::string n = groom::styleName(groom::Style(k));
        std::string label = n;
        label[0] = char(std::toupper(label[0]));
        actions::add({.id = "groom.style." + n,
                      .label = label,
                      .category = "Groom",
                      .doc = "Adds " + n + " to the selected groom's style stack.",
                      .python = ops::toPython("groomAddStyle", {{"style", n}}),
                      .command = "groomAddStyle",
                      .call = {{"style", n}}});
    }
    for (const char* t : {"groom", "curve", "mesh"})
        attributeEditor::addSection(t, "Groom", [](NodeId id) { return stackPanel(id); });
    registerLockHandles();
}
} // namespace lila::ui
