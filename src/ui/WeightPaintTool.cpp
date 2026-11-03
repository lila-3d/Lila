#include "ui/WeightPaintTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "brush/BrushUi.h"
#include "brush/Surface.h"
#include "brush/Symmetry.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Symmetry.h"
#include "scene/Components.h"
#include "scene/DeformWeights.h"
#include "scene/Deformers.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "ui/Actions.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"
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

constexpr const char* kWeightPaintPrefix = "/weightPaint";

struct WeightPaintSettings
{
    Mode mode = Mode::Replace;
    float value = 1.0f;
    int mirrorAxis = 0;
    int64_t influence = -1;
    /** The deformer or field whose map is painted instead of an influence, kNoNode for skin weights. */
    NodeId mapNode = kNoNode;
    int mapLayer = -1;
    /** Radius, strength, mirror, falloff (bell: full in the middle, smooth to zero at the rim) and the shared brush. */
    brush::BrushSettings stroke = [] {
        brush::BrushSettings b;
        b.radius = 40.0;
        b.strength = 1.0;
        b.spacing = 0.25;
        b.falloff = "bell";
        b.mirror = false;
        return b;
    }();
};

WeightPaintSettings& weightPaintSettings()
{
    static WeightPaintSettings s = [] {
        WeightPaintSettings out;
        out.stroke.loadPrefs(kWeightPaintPrefix);
        return out;
    }();
    return s;
}

void saveWeightPaintSettings() { weightPaintSettings().stroke.savePrefs(kWeightPaintPrefix); }

/** Floating tool settings: influences with lock toggles, brush mode and sizes, mirror, flood. */
class WeightPaintPanel final : public QWidget
{
public:
    std::function<void()> onFlood, onNormalize, onClosed, onChanged;

    explicit WeightPaintPanel(QWidget* parent) : QWidget(parent, Qt::Tool)
    {
        setWindowTitle("Paint Weights");
        auto* col = new QVBoxLayout(this);
        mTarget = new QLabel(this);
        col->addWidget(mTarget);
        mList = new QListWidget(this);
        mList->setToolTip("What to paint: a skin influence (checked ones are locked: painting and normalizing leave them alone), a "
                          "deformer's weights or a field's painted layer.");
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
        mRadius->setRange(2, 2000);
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
        mStrokeOptions = new brush::ui::StrokeOptions(this, []() -> brush::BrushSettings& { return weightPaintSettings().stroke; }, [this] { changed(); });
        col->addWidget(mStrokeOptions);

        auto* buttons = new QHBoxLayout;
        auto* flood = new QPushButton("Flood", this);
        auto* normalize = new QPushButton("Normalize", this);
        buttons->addWidget(flood);
        buttons->addWidget(normalize);
        col->addLayout(buttons);

        syncFromSettings();
        connect(mMode, &QComboBox::currentIndexChanged, this, [this](int i) { weightPaintSettings().mode = Mode(i); changed(); });
        connect(mValue, &QDoubleSpinBox::valueChanged, this, [this](double v) { weightPaintSettings().value = float(v); changed(); });
        connect(mRadius, &QSpinBox::valueChanged, this, [this](int v) { weightPaintSettings().stroke.radius = v; changed(); });
        connect(mStrength, &QDoubleSpinBox::valueChanged, this, [this](double v) { weightPaintSettings().stroke.strength = v; changed(); });
        connect(mMirror, &QCheckBox::toggled, this, [this](bool on) { weightPaintSettings().stroke.mirror = on; changed(); });
        connect(mAxis, &QComboBox::currentIndexChanged, this, [this](int i) { weightPaintSettings().mirrorAxis = i; changed(); });
        connect(flood, &QPushButton::clicked, this, [this] { if (onFlood) onFlood(); });
        connect(normalize, &QPushButton::clicked, this, [this] { if (onNormalize) onNormalize(); });
        connect(mList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
            if (!item)
                return;
            if (item->data(kMapRole).isValid())
            {
                weightPaintSettings().mapNode = item->data(kMapRole).toULongLong();
                weightPaintSettings().mapLayer = item->data(kMapRole + 1).toInt();
            }
            else
            {
                weightPaintSettings().mapNode = kNoNode;
                weightPaintSettings().influence = item->data(Qt::UserRole).toLongLong();
            }
            changed();
        });
        connect(mList, &QListWidget::itemChanged, this, [](QListWidgetItem* item) {
            if (item->data(kMapRole).isValid())
                return;
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
        const WeightPaintSettings& s = weightPaintSettings();
        QSignalBlocker b1(mMode), b2(mValue), b3(mRadius), b4(mStrength), b5(mMirror), b6(mAxis);
        mMode->setCurrentIndex(int(s.mode));
        mValue->setValue(s.value);
        mRadius->setValue(int(std::lround(s.stroke.radius)));
        mStrength->setValue(s.stroke.strength);
        mMirror->setChecked(s.stroke.mirror);
        mAxis->setCurrentIndex(s.mirrorAxis);
        if (mStrokeOptions)
            mStrokeOptions->sync();
    }

    void setInfluences(const QString& target, const std::vector<scene::skin::Influence>& infs,
                       const std::vector<scene::deformWeights::MapTarget>& maps = {})
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
            item->setData(Qt::UserRole + 1, QString::fromStdString(scene::Graph::current().node(i.link)->path()));
            if (weightPaintSettings().mapNode == kNoNode && int64_t(i.index) == weightPaintSettings().influence)
                mList->setCurrentItem(item);
        }
        for (const auto& m : maps)
        {
            auto* item = new QListWidgetItem(QString::fromStdString(m.label), mList);
            item->setData(kMapRole, qulonglong(m.node));
            item->setData(kMapRole + 1, m.layer);
            item->setToolTip(m.layer < 0 ? "The deformer's weight map" : "A painted layer of the field the deformers read");
            if (weightPaintSettings().mapNode == m.node && weightPaintSettings().mapLayer == m.layer)
                mList->setCurrentItem(item);
        }
        if (!mList->currentItem() && mList->count() > 0)
        {
            mList->setCurrentRow(0);
            QListWidgetItem* first = mList->item(0);
            if (first->data(kMapRole).isValid())
            {
                weightPaintSettings().mapNode = first->data(kMapRole).toULongLong();
                weightPaintSettings().mapLayer = first->data(kMapRole + 1).toInt();
            }
            else
            {
                weightPaintSettings().mapNode = kNoNode;
                weightPaintSettings().influence = first->data(Qt::UserRole).toLongLong();
            }
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
    static constexpr int kMapRole = Qt::UserRole + 2;

    void changed()
    {
        saveWeightPaintSettings();
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
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
};

class WeightPaintTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "weightPaint"; }

    void activate() override
    {
        mAlive = std::make_shared<bool>(true);
        mControls.activate();
        mPanel = new WeightPaintPanel(mainWindow());
        mPanel->onChanged = [] { viewportTools::requestRedraw(); };
        mPanel->onFlood = [this] { flood(); };
        mPanel->onNormalize = [this] {
            if (mSkin != kNoNode && !mapMode())
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
        mControls.deactivate();
        scene::Graph::current().removeListener(mListener);
        mAlive.reset();
        if (mPanel)
        {
            mPanel->onClosed = nullptr;
            mPanel->deleteLater();
        }
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if (!canPaint())
            return true;
        beginStroke();
        mMods = e->modifiers();
        const ViewInfo v = view;
        mStroke.begin(weightPaintSettings().stroke, mControls.input(e),
                      [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(QPointF(px.x, px.y), v, out); }, *this, false);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo&) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mStroking)
        {
            mMods = e->modifiers();
            mStroke.move(mControls.input(e));
            viewportTools::requestRedraw();
            return true;
        }
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        switch (e->key())
        {
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
        if (!g.node(mMesh) || (mapMode() ? !g.node(mMap.node) : (mSkin == kNoNode || !g.node(mSkin))))
        {
            drawToolMessage(p, view, "Paint weights: select a skinned mesh or a mesh with deformers");
            return;
        }
        ensureBvh();
        addWeightTriangles(s);

        QString infName = "(none)";
        for (const auto& i : mInfluences)
            if (int64_t(i.index) == weightPaintSettings().influence)
                infName = QString::fromStdString(i.name);
        if (mapMode())
            infName = QString::fromStdString(mMap.label);
        const Qt::KeyboardModifiers mods = mStroking ? mMods : QApplication::keyboardModifiers();
        const Mode mode = (mods & Qt::ShiftModifier) ? Mode::Smooth : weightPaintSettings().mode;
        drawToolMessage(p, view, QString("Paint weights: %1  (%2)").arg(infName, kModeNames[int(mode)]));

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        std::optional<brush::ui::CursorSurface> surface;
        geo::BvhHit hit;
        if (raycast(at, view, hit))
        {
            const glm::dmat4 w = meshWorld();
            surface = brush::ui::CursorSurface{glm::dvec3(w * glm::dvec4(hit.point, 1.0)),
                                               glm::normalize(glm::dvec3(glm::transpose(glm::inverse(glm::dmat3(w))) * triangleNormal(hit.triangle)))};
        }
        brush::ui::drawCursor(p, view, weightPaintSettings().stroke, at, surface,
                              {.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .stroke = &mStroke});
    }

private:
    static std::string nodeName(NodeId id) { return scene::Graph::current().node(id)->path(); }

    glm::dmat4 meshWorld() const { return scene::Graph::current().node(mMesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix); }

    /** Painting a deformer's or field's map rather than a skin influence. */
    bool mapMode() const { return mMap.node != kNoNode; }
    bool canPaint() const { return mapMode() ? mMap.mesh != nullptr : mSkin != kNoNode && weightPaintSettings().influence >= 0; }

    /**
     * The surface painted on, with point ids matching the weights: the skin's own output, or for a map the mesh as shown
     * when nothing above changes its points, else the deformer's input.
     */
    scene::MeshPtr deformed() const
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        if (mapMode())
        {
            scene::MeshPtr shown = g.node(mMesh) ? g.node(mMesh)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
            return shown && mMap.mesh && shown->pointCapacity() == mMap.mesh->pointCapacity() ? shown : mMap.mesh;
        }
        return g.node(mSkin)->as<scene::MeshPtr>(scene::SkinClusterNode::kOutMesh);
    }

    /** What changes when the painted map changes, for redraws. */
    scene::DataPtr mapKey() const
    {
        scene::Graph& g = scene::Graph::current();
        if (!mapMode() || !g.node(mMap.node))
            return nullptr;
        int attr = mMap.layer < 0 ? int(scene::DeformerNode::kWeights) : int(scene::fields::kLayers);
        return std::get<scene::DataPtr>(g.storedValue({mMap.node, attr, -1}));
    }

    void retarget()
    {
        scene::Graph& g = scene::Graph::current();
        NodeId mesh = kNoNode, skin = kNoNode;
        const auto& sel = g.selection();
        for (auto it = sel.rbegin(); it != sel.rend() && mesh == kNoNode; ++it)
        {
            scene::Node* n = g.node(*it);
            if (!n)
                continue;
            if (n->type().isA("mesh") && !n->type().isA("lattice"))
            {
                skin = scene::skin::find(g, n->id());
                if (skin != kNoNode || !scene::deformers::list(g, n->id()).empty())
                    mesh = n->id();
            }
            else if (dynamic_cast<scene::SkinClusterNode*>(n))
            {
                skin = n->id();
                mesh = scene::skin::meshOf(g, skin);
            }
            else if (scene::deformers::isDeformer(*n))
            {
                mesh = scene::modifiers::meshOf(g, n->id());
                skin = mesh != kNoNode ? scene::skin::find(g, mesh) : kNoNode;
                weightPaintSettings().mapNode = n->id();
                weightPaintSettings().mapLayer = -1;
            }
        }
        // Keep painting the current mesh when the selection moves to a joint (picking influences from the viewport).
        if (mesh == kNoNode && mMesh != kNoNode && g.node(mMesh) && (mSkin == kNoNode || g.node(mSkin)))
        {
            mesh = mMesh;
            skin = mSkin;
        }
        if (mesh == kNoNode)
            skin = kNoNode;
        std::vector<scene::deformWeights::MapTarget> maps;
        if (mesh != kNoNode)
            try
            {
                maps = scene::deformWeights::paintable(g, mesh);
            }
            catch (const std::exception& e)
            {
                log::warn("paint weights: {}", e.what());
            }
        scene::deformWeights::MapTarget map;
        for (const auto& m : maps)
            if (m.node == weightPaintSettings().mapNode && m.layer == weightPaintSettings().mapLayer)
                map = m;
        if (map.node == kNoNode && skin == kNoNode && !maps.empty())
        {
            map = maps.front();
            weightPaintSettings().mapNode = map.node;
            weightPaintSettings().mapLayer = map.layer;
        }
        if (skin != mSkin || mesh != mMesh || map.node != mMap.node || map.layer != mMap.layer || map.mesh != mMap.mesh)
        {
            mBvhMesh.reset();
            mSymmetryFor.reset();
        }
        mMesh = mesh;
        mSkin = skin;
        mMap = map;
        mMaps = maps;
        mInfluences = skin != kNoNode ? scene::skin::influences(g, skin) : std::vector<scene::skin::Influence>{};
        // A selected joint that is an influence becomes the one painted, like Maya's paint tool.
        [&] {
            for (auto it = sel.rbegin(); it != sel.rend(); ++it)
                for (const auto& i : mInfluences)
                    if (i.joint == *it)
                    {
                        weightPaintSettings().influence = i.index;
                        weightPaintSettings().mapNode = kNoNode;
                        mMap = {};
                        return;
                    }
        }();
        if (mPanel)
            mPanel->setInfluences(mesh != kNoNode ? QString::fromStdString(nodeName(mesh)) : QString("Select a skinned mesh or a mesh with deformers"),
                                  mInfluences, mMaps);
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
        if (mMesh == kNoNode || !mBvhMesh || mBvh.empty())
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
        if (mapMode())
        {
            mMapStart = scene::deformWeights::read(g, mMap);
            mMapWork = mMapStart;
            if (weightPaintSettings().stroke.mirror)
                buildSymmetry();
            return;
        }
        mStart = std::get<scene::DataPtr>(g.storedValue({mSkin, scene::SkinClusterNode::kWeights, -1}));
        mWork = scene::skin::table(g, mSkin);
        mInfluences = scene::skin::influences(g, mSkin);
        mLocked = scene::skin::lockMask(mInfluences);
        mMaxInf = int(g.node(mSkin)->as<int64_t>(scene::SkinClusterNode::kMaxInfluences));
        if (weightPaintSettings().stroke.mirror)
            buildSymmetry();
    }

    void buildSymmetry()
    {
        scene::Graph& g = scene::Graph::current();
        scene::MeshPtr input = mapMode() ? mMap.mesh : scene::skin::inputMesh(g, mSkin);
        if (!input || (input == mSymmetryFor && mSymmetryAxis == weightPaintSettings().mirrorAxis))
            return;
        geo::Mesh world = *input;
        glm::dmat4 geom = mapMode() ? mMap.world : g.node(mSkin)->as<glm::dmat4>(scene::SkinClusterNode::kGeomMatrix);
        for (geo::Index p = 0; p < world.pointCapacity(); ++p)
            world.setPosition(p, glm::dvec3(geom * glm::dvec4(input->position(p), 1.0)));
        geo::SymmetryPlane plane;
        plane.normal = glm::dvec3(0.0);
        plane.normal[weightPaintSettings().mirrorAxis] = 1.0;
        mSymmetry = geo::SymmetryMap::build(world, plane, 0.1);
        mSymmetryFor = input;
        mSymmetryAxis = weightPaintSettings().mirrorAxis;
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

    void apply(const std::vector<std::pair<uint32_t, float>>& pts, Mode mode, bool invert, uint32_t inf, float amount)
    {
        const WeightPaintSettings& s = weightPaintSettings();
        if (mapMode())
        {
            if (mode == Mode::Smooth)
            {
                std::vector<std::pair<uint32_t, float>> scaled;
                for (auto [p, f] : pts)
                    scaled.push_back({p, f * amount});
                scene::deformWeights::smooth(*mMap.mesh, mMapWork, scaled);
                return;
            }
            for (auto [p, f] : pts)
                if (p < mMapWork.size())
                    scene::deformWeights::paint(mMapWork[p], mode, invert, s.value, f * amount);
            return;
        }
        if (mode == Mode::Smooth)
        {
            std::vector<std::pair<uint32_t, float>> scaled;
            for (auto [p, f] : pts)
                scaled.push_back({p, f * amount});
            scene::skin::smoothPoints(*mBvhMesh, mWork, scaled, mLocked, mMaxInf);
            return;
        }
        for (auto [p, f] : pts)
            scene::skin::paintPoint(mWork, p, inf, mode, invert, s.value, f * amount, mLocked, mMaxInf);
    }

    /** The shared stroke's view of the skinned surface under a pixel, in the mesh's object space. */
    bool surfaceQuery(QPointF px, const ViewInfo& view, brush::SurfaceHit& out)
    {
        geo::BvhHit hit;
        if (!raycast(px, view, hit))
            return false;
        const glm::dmat4 w = meshWorld();
        const glm::dvec3 c(w * glm::dvec4(hit.point, 1.0));
        const double scale = std::max(1e-12, glm::length(glm::dvec3(w[0])));
        glm::dvec3 o, d;
        view.ray(px, o, d);
        out.center = hit.point;
        out.normal = triangleNormal(hit.triangle);
        out.viewDir = glm::normalize(glm::dvec3(glm::inverse(w) * glm::dvec4(d, 0.0)));
        out.right = brush::ui::screenRight(view, w);
        out.pixelSize = view.unitsPerPixel(c) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    void dab(const brush::Dab& d) override
    {
        LILA_ZONE();

        const WeightPaintSettings& s = weightPaintSettings();
        const Mode mode = (mMods & Qt::ShiftModifier) ? Mode::Smooth : s.mode;
        const bool invert = mMods & Qt::ControlModifier;
        const auto inf = uint32_t(s.influence);
        const float amount = float(s.stroke.strength * d.opacity * d.flow);
        const brush::Falloff falloff = s.stroke.resolveFalloff();
        std::vector<std::pair<uint32_t, float>> pts;
        for (brush::Dab copy : brush::radialDabs(d, s.stroke.radial))
        {
            geo::Index tri = geo::kInvalid;
            if (brush::SurfaceWalk::snap(mBvh, copy, tri))
                for (auto [p, w] : mWalk.gather(*mBvhMesh, mBvh, tri, copy, falloff))
                    pts.push_back({p, w});
        }
        if (s.stroke.radial.any())
            brush::mergeHits(pts);
        apply(pts, mode, invert, inf, amount);
        if (s.stroke.mirror && !mSymmetry.empty())
        {
            std::vector<std::pair<uint32_t, float>> mirrored;
            for (auto [p, f] : pts)
                if (geo::Index q = mSymmetry.mirrorPoint(p); q != geo::kInvalid && q != p)
                    mirrored.push_back({q, f});
            apply(mirrored, mode, invert, mPartner.contains(inf) ? mPartner[inf] : inf, amount);
        }
        pushLive();
    }

    /** Shows the stroke so far without recording undo; endStroke records the whole stroke once. */
    void pushLive()
    {
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        if (mapMode())
            scene::deformWeights::write(scene::Graph::current(), mMap, mMapWork);
        else
            scene::Graph::current().setValue({mSkin, scene::SkinClusterNode::kWeights, -1}, scene::DataPtr(mWork.pack()));
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        scene::Graph& g = scene::Graph::current();
        if (mapMode())
        {
            if (!g.node(mMap.node))
                return;
            UndoStack& u = UndoStack::get();
            bool was = u.enabled();
            u.setEnabled(false);
            scene::deformWeights::write(g, mMap, mMapStart);
            u.setEnabled(was);
            UndoChunk chunk("paintDeformerWeights");
            scene::deformWeights::write(g, mMap, mMapWork);
            return;
        }
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
        if (mapMode())
        {
            static const char* kModeArgs[] = {"add", "replace", "smooth", "scale"};
            nlohmann::json call = {{"args", {nodeName(mMap.node)}},
                                   {"value", weightPaintSettings().value},
                                   {"mode", kModeArgs[int(weightPaintSettings().mode)]},
                                   {"strength", weightPaintSettings().stroke.strength}};
            if (mMap.layer >= 0)
            {
                call["layer"] = mMap.layer;
                call["mesh"] = nodeName(mMesh);
            }
            if (scene::components::mode() != scene::ComponentMode::Object)
                if (auto ids = scene::components::pointsOf(mMesh); !ids.empty())
                    call["points"] = ids;
            actions::run({{"command", "floodDeformerWeights"}, {"args", call}});
            viewportTools::requestRedraw();
            return;
        }
        if (mSkin == kNoNode || weightPaintSettings().influence < 0)
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
        apply(pts, weightPaintSettings().mode, false, uint32_t(weightPaintSettings().influence), float(weightPaintSettings().stroke.strength));
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
        scene::DataPtr w = mapMode() ? mapKey() : std::get<scene::DataPtr>(g.storedValue({mSkin, scene::SkinClusterNode::kWeights, -1}));
        glm::dmat4 world = meshWorld();
        bool same = m == mDrawnMesh && w == mDrawnWeights && weightPaintSettings().influence == mDrawnInfluence && world == mDrawnWorld &&
                    glm::dvec3(s.eye) == mDrawnEye;
        if (!same && m && mBvhMesh && m->pointCapacity() == mBvhMesh->pointCapacity())
        {
            mDrawnMesh = m;
            mDrawnWeights = w;
            mDrawnInfluence = weightPaintSettings().influence;
            mDrawnWorld = world;
            mDrawnEye = s.eye;
            const auto* sw = dynamic_cast<const scene::SkinWeights*>(w.get());
            const bool gray = mapMode() || viewportDisplay::grayscaleWeights();
            std::vector<glm::vec4> ramp(m->pointCapacity(), viewportDisplay::weightColor(0.0f, gray));
            if (mapMode())
            {
                std::vector<float> values = mStroking ? mMapWork : scene::deformWeights::read(g, mMap);
                for (size_t p = 0; p < std::min(values.size(), ramp.size()); ++p)
                    ramp[p] = viewportDisplay::weightColor(values[p], true);
            }
            else if (sw)
                for (uint32_t p = 0; p < std::min<uint32_t>(sw->pointCount(), uint32_t(ramp.size())); ++p)
                    for (uint32_t k = sw->offsets[p]; k < sw->offsets[p + 1]; ++k)
                        if (int64_t(sw->influences[k]) == weightPaintSettings().influence)
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
    scene::deformWeights::MapTarget mMap;
    std::vector<scene::deformWeights::MapTarget> mMaps;
    std::vector<float> mMapStart, mMapWork;

    scene::MeshPtr mBvhMesh;
    geo::MeshBvh mBvh;
    brush::SurfaceWalk mWalk;

    scene::MeshPtr mSymmetryFor;
    int mSymmetryAxis = -1;
    geo::SymmetryMap mSymmetry;
    std::unordered_map<uint32_t, uint32_t> mPartner;

    bool mStroking = false;
    scene::DataPtr mStart;
    WeightTable mWork;
    std::vector<char> mLocked;
    int mMaxInf = 4;
    QPointF mHover;
    Qt::KeyboardModifiers mMods;
    brush::Stroke mStroke;
    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return weightPaintSettings().stroke; },
                                       [this] {
                                           if (mPanel)
                                               mPanel->syncFromSettings();
                                           viewportTools::requestRedraw();
                                       },
                                       [] { saveWeightPaintSettings(); }};

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
