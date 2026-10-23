#include "ui/WeightPaintTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "ui/Actions.h"
#include "ui/Tools.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>


#include <cmath>
#include <queue>
#include <unordered_map>

namespace lila::ui
{
namespace
{
using scene::NodeId;
using scene::kNoNode;
using scene::WeightTable;

using Mode = scene::skin::PaintMode;
const char* kModeNames[] = {"Add", "Replace", "Smooth", "Scale"};

struct Settings
{
    Mode mode = Mode::Replace;
    float value = 1.0f;
    double radius = 40.0; // px
    float strength = 1.0f;
    bool mirror = false;
    int mirrorAxis = 0;
    int64_t influence = -1;
};

Settings& settings()
{
    static Settings s;
    return s;
}

/** (1 - t^2)^2: full strength in the middle, smooth to zero at the rim. */
float falloff(double t) { return t >= 1.0 ? 0.0f : float((1.0 - t * t) * (1.0 - t * t)); }

QMainWindow* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* mw = qobject_cast<QMainWindow*>(w))
            return mw;
    return nullptr;
}

/** Floating tool settings: influences with lock toggles, brush mode and sizes, mirror, flood. */
class WeightPaintPanel final : public QWidget
{
public:
    std::function<void()> onFlood, onNormalize, onClosed, onChanged;

    explicit WeightPaintPanel(QWidget* parent) : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Paint Skin Weights");
        auto* col = new QVBoxLayout(this);
        mTarget = new QLabel(this);
        col->addWidget(mTarget);
        mList = new QListWidget(this);
        mList->setToolTip("Current influence. Checked influences are locked: painting and normalizing leave them alone.");
        col->addWidget(mList, 1);

        auto* form = new QFormLayout;
        mMode = new QComboBox(this);
        for (const char* m : kModeNames)
            mMode->addItem(m);
        form->addRow("Mode", mMode);
        mValue = new QDoubleSpinBox(this);
        mValue->setRange(0.0, 1.0);
        mValue->setSingleStep(0.05);
        mValue->setDecimals(3);
        form->addRow("Value", mValue);
        mRadius = new QSpinBox(this);
        mRadius->setRange(2, 1000);
        mRadius->setSuffix(" px");
        form->addRow("Radius", mRadius);
        mStrength = new QDoubleSpinBox(this);
        mStrength->setRange(0.0, 1.0);
        mStrength->setSingleStep(0.05);
        mStrength->setDecimals(3);
        form->addRow("Strength", mStrength);
        auto* mirrorRow = new QHBoxLayout;
        mMirror = new QCheckBox("Mirror", this);
        mAxis = new QComboBox(this);
        mAxis->addItems({"X", "Y", "Z"});
        mirrorRow->addWidget(mMirror);
        mirrorRow->addWidget(mAxis);
        form->addRow(mirrorRow);
        col->addLayout(form);

        auto* buttons = new QHBoxLayout;
        auto* flood = new QPushButton("Flood", this);
        auto* normalize = new QPushButton("Normalize", this);
        buttons->addWidget(flood);
        buttons->addWidget(normalize);
        col->addLayout(buttons);

        syncFromSettings();
        connect(mMode, &QComboBox::currentIndexChanged, this, [this](int i) { settings().mode = Mode(i); changed(); });
        connect(mValue, &QDoubleSpinBox::valueChanged, this, [this](double v) { settings().value = float(v); changed(); });
        connect(mRadius, &QSpinBox::valueChanged, this, [this](int v) { settings().radius = v; changed(); });
        connect(mStrength, &QDoubleSpinBox::valueChanged, this, [this](double v) { settings().strength = float(v); changed(); });
        connect(mMirror, &QCheckBox::toggled, this, [this](bool on) { settings().mirror = on; changed(); });
        connect(mAxis, &QComboBox::currentIndexChanged, this, [this](int i) { settings().mirrorAxis = i; changed(); });
        connect(flood, &QPushButton::clicked, this, [this] { if (onFlood) onFlood(); });
        connect(normalize, &QPushButton::clicked, this, [this] { if (onNormalize) onNormalize(); });
        connect(mList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
            if (!item)
                return;
            settings().influence = item->data(Qt::UserRole).toLongLong();
            changed();
        });
        connect(mList, &QListWidget::itemChanged, this, [](QListWidgetItem* item) {
            std::string link = item->data(Qt::UserRole + 1).toString().toStdString();
            bool lock = item->checkState() == Qt::Checked;
            actions::run({{"command", "setAttr"}, {"args", {{"args", {link + ".lockWeights", lock}}}}});
        });

        resize(260, 460);
        if (parent)
            move(parent->mapToGlobal(QPoint(60, 90)));
    }

    void syncFromSettings()
    {
        const Settings& s = settings();
        QSignalBlocker b1(mMode), b2(mValue), b3(mRadius), b4(mStrength), b5(mMirror), b6(mAxis);
        mMode->setCurrentIndex(int(s.mode));
        mValue->setValue(s.value);
        mRadius->setValue(int(std::lround(s.radius)));
        mStrength->setValue(s.strength);
        mMirror->setChecked(s.mirror);
        mAxis->setCurrentIndex(s.mirrorAxis);
    }

    void setInfluences(const QString& target, const std::vector<scene::skin::Influence>& infs)
    {
        QSignalBlocker block(mList);
        mTarget->setText(target);
        mList->clear();
        for (const auto& i : infs)
        {
            auto* item = new QListWidgetItem(QString::fromStdString(i.name.empty() ? "(disconnected)" : i.name), mList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(i.locked ? Qt::Checked : Qt::Unchecked);
            item->setData(Qt::UserRole, qlonglong(i.index));
            item->setData(Qt::UserRole + 1, QString::fromStdString(scene::Graph::current().node(i.link)->name()));
            if (int64_t(i.index) == settings().influence)
                mList->setCurrentItem(item);
        }
        if (!mList->currentItem() && mList->count() > 0)
        {
            mList->setCurrentRow(0);
            settings().influence = mList->item(0)->data(Qt::UserRole).toLongLong();
        }
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        if (onClosed)
            onClosed();
        QWidget::closeEvent(e);
    }

private:
    void changed()
    {
        if (onChanged)
            onChanged();
    }

    QLabel* mTarget = nullptr;
    QListWidget* mList = nullptr;
    QComboBox* mMode = nullptr;
    QDoubleSpinBox* mValue = nullptr;
    QSpinBox* mRadius = nullptr;
    QDoubleSpinBox* mStrength = nullptr;
    QCheckBox* mMirror = nullptr;
    QComboBox* mAxis = nullptr;
};

class WeightPaintTool final : public ViewportTool
{
public:
    const char* name() const override { return "weightPaint"; }

    void activate() override
    {
        mAlive = std::make_shared<bool>(true);
        mPanel = new WeightPaintPanel(mainWindow());
        mPanel->onChanged = [] { viewportTools::requestRedraw(); };
        mPanel->onFlood = [this] { flood(); };
        mPanel->onNormalize = [this] {
            if (mSkin != kNoNode)
                actions::run({{"command", "normalizeWeights"}, {"args", {{"args", {nodeName(mSkin)}}}}});
        };
        mPanel->onClosed = [] { QTimer::singleShot(0, [] { if (viewportTools::active() && std::string(viewportTools::active()->name()) == "weightPaint") viewportTools::set(nullptr); }); };
        mPanel->show();
        std::weak_ptr<bool> alive = mAlive;
        mListener = scene::Graph::current().addListener([this, alive](scene::Change c, NodeId) {
            if (c == scene::Change::Value || mStroking || mRefreshQueued)
                return;
            mRefreshQueued = true;
            QTimer::singleShot(0, [this, alive] {
                if (alive.expired())
                    return;
                mRefreshQueued = false;
                retarget();
            });
        });
        retarget();
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        scene::Graph::current().removeListener(mListener);
        mAlive.reset();
        if (mPanel)
        {
            mPanel->onClosed = nullptr;
            mPanel->deleteLater();
        }
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mHover = e->position();
        if (mResizeArmed || tools::softSelectAdjusting())
        {
            mResizing = true;
            mResizeStart = e->position();
            mResizeFrom = settings().radius;
            return true;
        }
        if (mSkin == kNoNode || settings().influence < 0)
            return true;
        beginStroke();
        dab(e->position(), view, e->modifiers());
        mLastDab = e->position();
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mResizing)
        {
            settings().radius = std::clamp(mResizeFrom + (e->position().x() - mResizeStart.x()), 2.0, 1000.0);
            if (mPanel)
                mPanel->syncFromSettings();
            viewportTools::requestRedraw();
            return true;
        }
        if (mStroking)
        {
            // Dabs every quarter radius along the drag, so fast strokes stay continuous.
            QPointF d = e->position() - mLastDab;
            double len = std::hypot(d.x(), d.y()), step = std::max(1.0, settings().radius * 0.25);
            int n = int(len / step);
            for (int k = 1; k <= n; ++k)
                dab(mLastDab + d * (k * step / len), view, e->modifiers());
            if (n > 0)
                mLastDab += d * (n * step / len);
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent*, const ViewInfo&) override
    {
        if (mResizing)
        {
            mResizing = false;
            mResizeArmed = false;
            return true;
        }
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_B:
            if (!e->isAutoRepeat())
                mResizeArmed = true;
            return true;
        case Qt::Key_BracketLeft:
        case Qt::Key_BracketRight:
            settings().radius = std::clamp(settings().radius * (e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25), 2.0, 1000.0);
            if (mPanel)
                mPanel->syncFromSettings();
            viewportTools::requestRedraw();
            return true;
        case Qt::Key_Escape:
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        default: return false;
        }
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        if (mSkin == kNoNode || !g.node(mSkin) || !g.node(mMesh))
        {
            drawToolMessage(p, view, "Paint weights: select a skinned mesh");
            return;
        }
        ensureBvh();
        addWeightTriangles(s);

        QString infName = "(none)";
        for (const auto& i : mInfluences)
            if (int64_t(i.index) == settings().influence)
                infName = QString::fromStdString(i.name);
        drawToolMessage(p, view, QString("Paint weights: %1  (%2)").arg(infName, kModeNames[int(settings().mode)]));

        // Brush rim: the radius circle laid on the surface under the cursor, or a flat one when off the mesh.
        const glm::vec4 rim(0.95f, 0.95f, 0.95f, 1.0f);
        geo::BvhHit hit;
        if (raycast(mHover, view, hit))
        {
            glm::dmat4 w = meshWorld();
            glm::dvec3 c(w * glm::dvec4(hit.point, 1.0));
            glm::dvec3 n = glm::normalize(glm::dvec3(glm::transpose(glm::inverse(glm::dmat3(w))) * triangleNormal(hit.triangle)));
            double r = settings().radius * view.unitsPerPixel(c);
            glm::dvec3 ref = std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
            glm::dvec3 u = glm::normalize(glm::cross(n, ref)), v = glm::cross(n, u);
            std::vector<QPointF> pts;
            for (int k = 0; k < 48; ++k)
            {
                double a = 2.0 * 3.14159265358979 * k / 48;
                if (auto q = view.project(c + (u * std::cos(a) + v * std::sin(a)) * r))
                    pts.push_back(*q);
            }
            p.polyline(pts, 1.5f, rim, true);
        }
        else
        {
            std::vector<QPointF> pts;
            for (int k = 0; k < 48; ++k)
            {
                double a = 2.0 * 3.14159265358979 * k / 48;
                pts.push_back(mHover + QPointF(std::cos(a), std::sin(a)) * settings().radius);
            }
            p.polyline(pts, 1.0f, {0.6f, 0.6f, 0.6f, 0.8f}, true);
        }
    }

private:
    static std::string nodeName(NodeId id) { return scene::Graph::current().node(id)->name(); }

    glm::dmat4 meshWorld() const { return scene::Graph::current().node(mMesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix); }

    /** The skin's own output: point ids match the weights even if topology modifiers sit above it. */
    scene::MeshPtr deformed() const
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        return g.node(mSkin)->as<scene::MeshPtr>(scene::SkinClusterNode::kOutMesh);
    }

    void retarget()
    {
        scene::Graph& g = scene::Graph::current();
        NodeId mesh = kNoNode, skin = kNoNode;
        const auto& sel = g.selection();
        for (auto it = sel.rbegin(); it != sel.rend() && skin == kNoNode; ++it)
        {
            scene::Node* n = g.node(*it);
            if (!n)
                continue;
            if (n->type().isA("mesh"))
            {
                if (NodeId s = scene::skin::find(g, n->id()); s != kNoNode)
                {
                    mesh = n->id();
                    skin = s;
                }
            }
            else if (dynamic_cast<scene::SkinClusterNode*>(n))
            {
                skin = n->id();
                mesh = scene::skin::meshOf(g, skin);
            }
        }
        // Keep painting the current mesh when the selection moves to a joint (picking influences from the viewport).
        if (skin == kNoNode && mSkin != kNoNode && g.node(mSkin) && g.node(mMesh))
        {
            mesh = mMesh;
            skin = mSkin;
        }
        if (skin != kNoNode && mesh == kNoNode)
            skin = kNoNode;
        if (skin != mSkin)
        {
            mBvhMesh.reset();
            mSymmetryFor.reset();
        }
        mMesh = mesh;
        mSkin = skin;
        mInfluences = skin != kNoNode ? scene::skin::influences(g, skin) : std::vector<scene::skin::Influence>{};
        // A selected joint that is an influence becomes the one painted, like Maya's paint tool.
        [&] {
            for (auto it = sel.rbegin(); it != sel.rend(); ++it)
                for (const auto& i : mInfluences)
                    if (i.joint == *it)
                    {
                        settings().influence = i.index;
                        return;
                    }
        }();
        if (mPanel)
            mPanel->setInfluences(skin != kNoNode ? QString::fromStdString(nodeName(mesh)) : QString("Select a skinned mesh"), mInfluences);
        viewportTools::requestRedraw();
    }

    void ensureBvh()
    {
        scene::MeshPtr m = deformed();
        if (!m || m == mBvhMesh || mStroking)
            return;
        mBvhMesh = m;
        mBvh.build(*m);
    }

    glm::dvec3 triangleNormal(geo::Index t) const
    {
        const auto& tris = mBvh.triangles();
        const geo::Mesh& m = *mBvhMesh;
        glm::dvec3 a = m.position(m.cornerPoint(tris.corners[size_t(t) * 3]));
        glm::dvec3 b = m.position(m.cornerPoint(tris.corners[size_t(t) * 3 + 1]));
        glm::dvec3 c = m.position(m.cornerPoint(tris.corners[size_t(t) * 3 + 2]));
        glm::dvec3 n = glm::cross(b - a, c - a);
        return glm::length(n) > 1e-18 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
    }

    bool raycast(QPointF px, const ViewInfo& view, geo::BvhHit& hit)
    {
        if (mSkin == kNoNode || !mBvhMesh || mBvh.empty())
            return false;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dmat4 inv = glm::inverse(meshWorld());
        return mBvh.raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit);
    }

    void beginStroke()
    {
        scene::Graph& g = scene::Graph::current();
        ensureBvh();
        mStroking = true;
        mStart = std::get<scene::DataPtr>(g.storedValue({mSkin, scene::SkinClusterNode::kWeights, -1}));
        mWork = scene::skin::table(g, mSkin);
        mInfluences = scene::skin::influences(g, mSkin);
        mLocked = scene::skin::lockMask(mInfluences);
        mMaxInf = int(g.node(mSkin)->as<int64_t>(scene::SkinClusterNode::kMaxInfluences));
        if (settings().mirror)
            buildSymmetry();
    }

    void buildSymmetry()
    {
        scene::Graph& g = scene::Graph::current();
        scene::MeshPtr input = scene::skin::inputMesh(g, mSkin);
        if (!input || (input == mSymmetryFor && mSymmetryAxis == settings().mirrorAxis))
            return;
        geo::Mesh world = *input;
        glm::dmat4 geom = g.node(mSkin)->as<glm::dmat4>(scene::SkinClusterNode::kGeomMatrix);
        for (geo::Index p = 0; p < world.pointCapacity(); ++p)
            world.setPosition(p, glm::dvec3(geom * glm::dvec4(input->position(p), 1.0)));
        geo::SymmetryPlane plane;
        plane.normal = glm::dvec3(0.0);
        plane.normal[settings().mirrorAxis] = 1.0;
        mSymmetry = geo::SymmetryMap::build(world, plane, 0.1);
        mSymmetryFor = input;
        mSymmetryAxis = settings().mirrorAxis;
        mPartner.clear();
        for (const auto& i : mInfluences)
        {
            std::string other = scene::rig::mirrorName(i.name, "_l", "_r");
            if (other == i.name)
                other = scene::rig::mirrorName(i.name, "_r", "_l");
            int64_t j = scene::skin::indexOf(mInfluences, other);
            mPartner[i.index] = j >= 0 ? uint32_t(j) : i.index;
        }
    }

    /** Points within the brush, by distance walked along edges from the hit (so it never jumps across a gap). */
    std::vector<std::pair<uint32_t, float>> brushPoints(const geo::BvhHit& hit, double radius)
    {
        const geo::Mesh& m = *mBvhMesh;
        const auto& tris = mBvh.triangles();
        if (mDist.size() != m.pointCapacity())
            mDist.assign(m.pointCapacity(), std::numeric_limits<double>::infinity());
        using Item = std::pair<double, geo::Index>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
        std::vector<geo::Index> touched;
        for (int k = 0; k < 3; ++k)
        {
            geo::Index p = m.cornerPoint(tris.corners[size_t(hit.triangle) * 3 + k]);
            double d = glm::length(m.position(p) - hit.point);
            if (d < mDist[p])
            {
                if (std::isinf(mDist[p]))
                    touched.push_back(p);
                mDist[p] = d;
                queue.push({d, p});
            }
        }
        while (!queue.empty())
        {
            auto [d, p] = queue.top();
            queue.pop();
            if (d > mDist[p] || d > radius)
                continue;
            m.forEachPointEdge(p, [&](geo::Index e) {
                geo::Index o = m.edgeOtherPoint(e, p);
                double nd = d + glm::length(m.position(o) - m.position(p));
                if (nd < mDist[o] && nd <= radius)
                {
                    if (std::isinf(mDist[o]))
                        touched.push_back(o);
                    mDist[o] = nd;
                    queue.push({nd, o});
                }
            });
        }
        std::vector<std::pair<uint32_t, float>> out;
        for (geo::Index p : touched)
        {
            if (mDist[p] <= radius)
                out.push_back({p, falloff(mDist[p] / radius)});
            mDist[p] = std::numeric_limits<double>::infinity();
        }
        return out;
    }

    void apply(const std::vector<std::pair<uint32_t, float>>& pts, Mode mode, bool invert, uint32_t inf)
    {
        const Settings& s = settings();
        if (mode == Mode::Smooth)
        {
            std::vector<std::pair<uint32_t, float>> scaled;
            for (auto [p, f] : pts)
                scaled.push_back({p, f * s.strength});
            scene::skin::smoothPoints(*mBvhMesh, mWork, scaled, mLocked, mMaxInf);
            return;
        }
        for (auto [p, f] : pts)
            scene::skin::paintPoint(mWork, p, inf, mode, invert, s.value, f * s.strength, mLocked, mMaxInf);
    }

    void dab(QPointF px, const ViewInfo& view, Qt::KeyboardModifiers mods)
    {
        LILA_ZONE();

        geo::BvhHit hit;
        if (!raycast(px, view, hit))
            return;
        glm::dmat4 w = meshWorld();
        glm::dvec3 c(w * glm::dvec4(hit.point, 1.0));
        double scale = std::max(1e-12, glm::length(glm::dvec3(w[0])));
        double radius = settings().radius * view.unitsPerPixel(c) / scale;
        auto pts = brushPoints(hit, radius);
        Mode mode = (mods & Qt::ShiftModifier) ? Mode::Smooth : settings().mode;
        bool invert = mods & Qt::ControlModifier;
        auto inf = uint32_t(settings().influence);
        apply(pts, mode, invert, inf);
        if (settings().mirror && !mSymmetry.empty())
        {
            std::vector<std::pair<uint32_t, float>> mirrored;
            for (auto [p, f] : pts)
                if (geo::Index q = mSymmetry.mirrorPoint(p); q != geo::kInvalid && q != p)
                    mirrored.push_back({q, f});
            apply(mirrored, mode, invert, mPartner.contains(inf) ? mPartner[inf] : inf);
        }
        pushLive();
    }

    /** Shows the stroke so far without recording undo; endStroke records the whole stroke once. */
    void pushLive()
    {
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        scene::Graph::current().setValue({mSkin, scene::SkinClusterNode::kWeights, -1}, scene::DataPtr(mWork.pack()));
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void endStroke()
    {
        mStroking = false;
        scene::Graph& g = scene::Graph::current();
        if (!g.node(mSkin))
            return;
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({mSkin, scene::SkinClusterNode::kWeights, -1}, mStart);
        u.setEnabled(was);
        UndoChunk chunk("paintSkinWeights");
        scene::skin::setWeights(g, mSkin, mWork);
        mStart.reset();
    }

    void flood()
    {
        if (mSkin == kNoNode || settings().influence < 0)
            return;
        scene::Graph& g = scene::Graph::current();
        ensureBvh();
        if (!mBvhMesh)
            return;
        mWork = scene::skin::table(g, mSkin);
        mInfluences = scene::skin::influences(g, mSkin);
        mLocked = scene::skin::lockMask(mInfluences);
        mMaxInf = int(g.node(mSkin)->as<int64_t>(scene::SkinClusterNode::kMaxInfluences));
        std::vector<uint32_t> ids;
        if (scene::components::mode() != scene::ComponentMode::Object)
            ids = scene::components::pointsOf(mMesh);
        if (ids.empty())
            for (geo::Index p = 0; p < mBvhMesh->pointCapacity(); ++p)
                if (mBvhMesh->pointValid(p))
                    ids.push_back(p);
        std::vector<std::pair<uint32_t, float>> pts;
        for (uint32_t p : ids)
            pts.push_back({p, 1.0f});
        apply(pts, settings().mode, false, uint32_t(settings().influence));
        UndoChunk chunk("floodSkinWeights");
        scene::skin::setWeights(g, mSkin, mWork);
        viewportTools::requestRedraw();
    }

    /** The painted influence over the skinned surface through the weight colour ramp (or gray, by the display pref). */
    void addWeightTriangles(render::FrameState& s)
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        scene::MeshPtr m = deformed();
        scene::DataPtr w = std::get<scene::DataPtr>(g.storedValue({mSkin, scene::SkinClusterNode::kWeights, -1}));
        glm::dmat4 world = meshWorld();
        bool same = m == mDrawnMesh && w == mDrawnWeights && settings().influence == mDrawnInfluence && world == mDrawnWorld &&
                    glm::dvec3(s.eye) == mDrawnEye;
        if (!same && m && mBvhMesh && m->pointCapacity() == mBvhMesh->pointCapacity())
        {
            mDrawnMesh = m;
            mDrawnWeights = w;
            mDrawnInfluence = settings().influence;
            mDrawnWorld = world;
            mDrawnEye = s.eye;
            const auto* sw = dynamic_cast<const scene::SkinWeights*>(w.get());
            const bool gray = viewportDisplay::grayscaleWeights();
            std::vector<glm::vec4> ramp(m->pointCapacity(), viewportDisplay::weightColor(0.0f, gray));
            if (sw)
                for (uint32_t p = 0; p < std::min<uint32_t>(sw->pointCount(), uint32_t(ramp.size())); ++p)
                    for (uint32_t k = sw->offsets[p]; k < sw->offsets[p + 1]; ++k)
                        if (int64_t(sw->influences[k]) == settings().influence)
                            ramp[p] = viewportDisplay::weightColor(sw->weights[k], gray);
            const auto& tris = mBvh.triangles();
            size_t count = tris.faces.size();
            mTris.resize(count * 3);
            glm::dmat3 nrm = glm::transpose(glm::inverse(glm::dmat3(world)));
            tbb::parallel_for(size_t(0), count, [&](size_t t) {
                glm::dvec3 pw[3];
                geo::Index pid[3];
                for (int k = 0; k < 3; ++k)
                {
                    pid[k] = m->cornerPoint(tris.corners[t * 3 + k]);
                    pw[k] = glm::dvec3(world * glm::dvec4(m->position(pid[k]), 1.0));
                }
                glm::dvec3 n = nrm * glm::cross(pw[1] - pw[0], pw[2] - pw[0]);
                glm::dvec3 toEye = glm::dvec3(s.eye) - pw[0];
                double facing = glm::length(n) > 1e-18 && glm::length(toEye) > 1e-18
                                    ? std::abs(glm::dot(glm::normalize(n), glm::normalize(toEye)))
                                    : 1.0;
                // A little headlight shading so the form still reads through the ramp.
                float shade = float(0.7 + 0.3 * facing);
                for (int k = 0; k < 3; ++k)
                {
                    mTris[t * 3 + k] = {glm::vec3(pw[k]), glm::vec4(glm::vec3(ramp[pid[k]]) * shade, 1.0f)};
                }
            });
        }
        s.tris3d.insert(s.tris3d.end(), mTris.begin(), mTris.end());
    }

    std::shared_ptr<bool> mAlive;
    QPointer<WeightPaintPanel> mPanel;
    int mListener = 0;
    bool mRefreshQueued = false;

    NodeId mMesh = kNoNode;
    NodeId mSkin = kNoNode;
    std::vector<scene::skin::Influence> mInfluences;

    scene::MeshPtr mBvhMesh;
    geo::MeshBvh mBvh;
    std::vector<double> mDist;

    scene::MeshPtr mSymmetryFor;
    int mSymmetryAxis = -1;
    geo::SymmetryMap mSymmetry;
    std::unordered_map<uint32_t, uint32_t> mPartner;

    bool mStroking = false;
    scene::DataPtr mStart;
    WeightTable mWork;
    std::vector<char> mLocked;
    int mMaxInf = 4;
    QPointF mLastDab;
    QPointF mHover;

    bool mResizeArmed = false;
    bool mResizing = false;
    QPointF mResizeStart;
    double mResizeFrom = 0.0;

    scene::MeshPtr mDrawnMesh;
    scene::DataPtr mDrawnWeights;
    int64_t mDrawnInfluence = -2;
    glm::dmat4 mDrawnWorld{0.0};
    glm::dvec3 mDrawnEye{0.0};
    std::vector<render::LineVertex> mTris;
};
} // namespace

void startWeightPaint()
{
    viewportTools::set(std::make_unique<WeightPaintTool>());
}
} // namespace lila::ui
