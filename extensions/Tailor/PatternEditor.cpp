// The 2D Pattern Editor: the selected garment's panels laid out flat, with seams, grain and lengths. Edits run Tailor
// commands, so they undo, show in the script history and re-drape the garment in the background.

#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Panels.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QSplitter>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>

using namespace lila;
using json = nlohmann::json;

namespace tailor
{
namespace
{
constexpr double kPick = 7.0; // px

double cm(double metres) { return metres * kCmPerM; }

/** The garment node behind the selection: a tailorGarment, or a mesh one feeds. */
scene::Node* selectedGarment(scene::Graph& g)
{
    for (scene::NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (n->type().isA(kGarmentType))
            return n;
        const int in = n->type().attrIndex("inMesh");
        if (in < 0)
            continue;
        const auto src = g.source({n->id(), in});
        if (scene::Node* s = src ? g.node(src->node) : nullptr; s && s->type().isA(kGarmentType))
            return s;
    }
    return nullptr;
}

bool run(const char* command, json call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
        return true;
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return false;
    }
}

class PatternView final : public QWidget
{
public:
    enum class Mode
    {
        Edit,
        Sew,
        Pen,
        Notch
    };

    explicit PatternView(QWidget* parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(320, 240);
    }

    std::function<void(const QString&)> onTitle;
    /** The pattern check's findings, after every refresh. */
    std::function<void(const json&)> onIssues;

    void highlight(int panel, int seam)
    {
        mSelected = panel;
        mHighlightSeam = seam;
        update();
    }

    const Garment& garment() const { return mG; }

    void setMode(Mode m)
    {
        mMode = m;
        mSewFrom.reset();
        mPen.clear();
        setCursor(m == Mode::Pen ? Qt::CrossCursor : Qt::ArrowCursor);
        update();
    }

    /** Re-reads the garment: the selected one, else the one shown while it still exists. */
    void refresh()
    {
        scene::Graph& g = scene::Graph::current();
        scene::Node* n = selectedGarment(g);
        if (!n && mGarment != scene::kNoNode)
            n = g.node(mGarment);
        const bool changed = !n || n->id() != mGarment;
        mGarment = n ? n->id() : scene::kNoNode;
        mPath = n ? n->path() : std::string();
        mG = Garment{};
        if (n)
        {
            try
            {
                const std::string& text = n->as<std::string>(n->type().attrIndex("garment"));
                if (!text.empty())
                    mG = garmentFromJson(json::parse(text));
            }
            catch (const std::exception& e)
            {
                log::warn("Pattern Editor: {}", e.what());
            }
        }
        layoutPanels();
        if (changed)
            fit();
        syncFromComponents();
        if (onIssues)
        {
            json issues = json::array();
            if (n)
                try
                {
                    issues = ops::execute("tailorCheck", {{"args", {mPath}}});
                }
                catch (const std::exception& e)
                {
                    log::warn("Pattern Editor: {}", e.what());
                }
            onIssues(issues);
        }
        if (onTitle)
            onTitle(n ? QString::fromStdString(n->name()) + QString("  %1 panels, %2 seams").arg(mG.panels.size()).arg(mG.seams.size())
                      : QString("Select a garment"));
        update();
    }

    void fit()
    {
        if (mG.panels.empty())
            return;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (size_t p = 0; p < mG.panels.size(); ++p)
            for (const Vec2& v : mG.panels[p].outline)
            {
                x0 = std::min(x0, v[0] + mOffset[p].x());
                x1 = std::max(x1, v[0] + mOffset[p].x());
                y0 = std::min(y0, v[1] + mOffset[p].y());
                y1 = std::max(y1, v[1] + mOffset[p].y());
            }
        const double w = std::max(x1 - x0, 0.05), h = std::max(y1 - y0, 0.05);
        mScale = 0.9 * std::min(width() / w, height() / h);
        mPan = QPointF(0.5 * width() - mScale * 0.5 * (x0 + x1), 0.5 * height() + mScale * 0.5 * (y0 + y1));
        update();
    }

    void addPanel()
    {
        if (!mPath.empty())
            run("tailorAddPanel", {{"args", {mPath}}});
    }

    void autoSew()
    {
        if (!mPath.empty())
            run("tailorAutoSew", {{"args", {mPath}}});
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(38, 38, 42));
        drawGrid(p);
        if (mG.panels.empty())
        {
            p.setPen(QColor(150, 150, 150));
            p.drawText(rect(), Qt::AlignCenter, mPath.empty() ? "Select a garment to edit its pattern" : "This garment has no panels");
            return;
        }
        for (size_t s = 0; s < mG.seams.size(); ++s)
            drawSeam(p, s);
        for (size_t k = 0; k < mG.panels.size(); ++k)
            drawPanel(p, int(k));
        drawHover(p);
        drawPen(p);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        const double f = std::pow(1.0015, e->angleDelta().y());
        const QPointF c = e->position();
        mPan = c + (mPan - c) * f;
        mScale *= f;
        update();
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        mLast = e->position();
        if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && e->modifiers() & Qt::AltModifier))
        {
            mPanning = true;
            return;
        }
        if (e->button() == Qt::RightButton)
        {
            contextMenu(e);
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;
        if (mMode == Mode::Pen)
        {
            if (mPen.size() >= 3 && std::hypot(e->position().x() - toScreenCanvas(mPen.front().at).x(),
                                               e->position().y() - toScreenCanvas(mPen.front().at).y()) < kPick)
                closePen();
            else
                mPen.push_back({toCanvas(e->position()), std::nullopt});
            update();
            return;
        }
        const Hit h = hit(e->position());
        if (mMode == Mode::Notch && (h.edge >= 0 || h.vertex >= 0))
        {
            const QPointF w = toPattern(e->position(), h.panel);
            run("tailorAddNotch", {{"args", {mPath}}, {"panel", mG.panels[size_t(h.panel)].name}, {"position", {cm(w.x()), cm(w.y())}}});
            return;
        }
        if (mMode == Mode::Edit && h.vertex >= 0)
        {
            mDrag = h;
            mDragStart = mG.panels[size_t(h.panel)].outline[size_t(h.vertex)];
        }
        else if (mMode == Mode::Edit && h.panel >= 0 && h.vertex < 0 && h.edge < 0)
        {
            mSelected = h.panel;
            selectPanelFaces(h.panel);
        }
        else if (mMode == Mode::Sew && h.edge >= 0)
        {
            if (!mSewFrom)
                mSewFrom = h;
            else
            {
                sew(*mSewFrom, h, !(e->modifiers() & Qt::ShiftModifier));
                mSewFrom.reset();
            }
        }
        update();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPointF d = e->position() - mLast;
        mLast = e->position();
        if (mPanning)
        {
            mPan += d;
            update();
            return;
        }
        if (mMode == Mode::Pen && !mPen.empty() && (e->buttons() & Qt::LeftButton))
        {
            // Dragging out a point pulls a curve handle through it.
            mPen.back().handle = toCanvas(e->position());
            update();
            return;
        }
        if (mDrag.vertex >= 0)
        {
            const QPointF w = toPattern(e->position(), mDrag.panel);
            Panel& pn = mG.panels[size_t(mDrag.panel)];
            pn.outline[size_t(mDrag.vertex)] = {float(w.x()), float(w.y())};
            if (pn.twin >= 0 && size_t(pn.twin) < mG.panels.size() && mG.panels[size_t(pn.twin)].outline.size() == pn.outline.size())
            {
                const size_t n = pn.outline.size();
                mG.panels[size_t(pn.twin)].outline[(n - size_t(mDrag.vertex)) % n] = {-float(w.x()), float(w.y())};
            }
            update();
            return;
        }
        mHover = hit(e->position());
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        mPanning = false;
        if (mDrag.vertex < 0)
            return;
        const Panel& pn = mG.panels[size_t(mDrag.panel)];
        const Vec2 now = pn.outline[size_t(mDrag.vertex)];
        mDrag = {};
        if (now == mDragStart)
            return;
        json outline = json::array();
        for (const Vec2& v : pn.outline)
            outline.push_back({cm(v[0]), cm(v[1])});
        run("tailorEditPanel", {{"args", {mPath}}, {"panel", pn.name}, {"outline", outline}});
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (mMode == Mode::Pen)
        {
            closePen();
            return;
        }
        const Hit h = hit(e->position());
        if (mMode != Mode::Edit || h.edge < 0 || h.vertex >= 0)
            return;
        const QPointF w = toPattern(e->position(), h.panel);
        run("tailorInsertPoint", {{"args", {mPath}},
                                  {"panel", mG.panels[size_t(h.panel)].name},
                                  {"index", h.edge + 1},
                                  {"position", {cm(w.x()), cm(w.y())}}});
    }

    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_F)
            fit();
        else if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && mHover.vertex >= 0)
            run("tailorDeletePoint", {{"args", {mPath}}, {"panel", mG.panels[size_t(mHover.panel)].name}, {"index", mHover.vertex}});
        else if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && mMode == Mode::Pen)
            closePen();
        else if (e->key() == Qt::Key_Escape)
        {
            mSewFrom.reset();
            mPen.clear();
            update();
        }
        else
            QWidget::keyPressEvent(e);
    }

    void resizeEvent(QResizeEvent*) override
    {
        if (!mFitted && width() > 50)
        {
            mFitted = true;
            fit();
        }
    }

private:
    struct Hit
    {
        int panel = -1;
        int vertex = -1;
        int edge = -1; // from vertex edge to edge + 1
        double t = 0.0; // along the edge
    };

    /** A pen point, with the curve handle dragged out of it. */
    struct PenPoint
    {
        QPointF at;
        std::optional<QPointF> handle;
    };

    QPointF toCanvas(const QPointF& s) const { return {(s.x() - mPan.x()) / mScale, (mPan.y() - s.y()) / mScale}; }
    QPointF toScreenCanvas(const QPointF& c) const { return {mPan.x() + mScale * c.x(), mPan.y() - mScale * c.y()}; }

    /** The pen's outline in canvas metres, curves sampled: a smooth point's handle leaves forward and mirrors back. */
    std::vector<QPointF> penOutline(bool closed) const
    {
        std::vector<QPointF> out;
        const size_t n = mPen.size();
        for (size_t i = 0; i < n; ++i)
        {
            out.push_back(mPen[i].at);
            if (i + 1 == n && !closed)
                break;
            const PenPoint &a = mPen[i], &b = mPen[(i + 1) % n];
            if (!a.handle && !b.handle)
                continue;
            const QPointF c1 = a.handle ? *a.handle : a.at, c2 = b.handle ? 2.0 * b.at - *b.handle : b.at;
            for (int k = 1; k < 12; ++k)
            {
                const double t = k / 12.0, u = 1.0 - t;
                out.push_back(u * u * u * a.at + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * b.at);
            }
        }
        return out;
    }

    void closePen()
    {
        // A double-click lands its second press on the last point.
        if (mPen.size() >= 2)
        {
            const QPointF d = toScreenCanvas(mPen.back().at) - toScreenCanvas(mPen[mPen.size() - 2].at);
            if (std::hypot(d.x(), d.y()) < kPick)
                mPen.pop_back();
        }
        if (mPen.size() >= 3 && !mPath.empty())
        {
            json outline = json::array();
            for (const QPointF& v : penOutline(true))
                outline.push_back({cm(v.x()), cm(v.y())});
            run("tailorAddPanel", {{"args", {mPath}}, {"name", "Panel"}, {"outline", outline}});
        }
        mPen.clear();
        update();
    }

    void drawPen(QPainter& p) const
    {
        if (mPen.empty())
            return;
        const std::vector<QPointF> pts = penOutline(false);
        p.setPen(QPen(QColor(255, 255, 255), 1.5));
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            p.drawLine(toScreenCanvas(pts[i]), toScreenCanvas(pts[i + 1]));
        p.setPen(QPen(QColor(255, 255, 255), 1.0, Qt::DashLine));
        p.drawLine(toScreenCanvas(pts.back()), mLast);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 220, 120));
        for (const PenPoint& v : mPen)
            p.drawEllipse(toScreenCanvas(v.at), 3.5, 3.5);
    }

    QPointF toScreen(const Vec2& v, int panel) const
    {
        const QPointF o = mOffset[size_t(panel)];
        return {mPan.x() + mScale * (v[0] + o.x()), mPan.y() - mScale * (v[1] + o.y())};
    }

    QPointF toPattern(const QPointF& s, int panel) const
    {
        const QPointF o = mOffset[size_t(panel)];
        return {(s.x() - mPan.x()) / mScale - o.x(), (mPan.y() - s.y()) / mScale - o.y()};
    }

    /** The mesh the garment feeds, whose faces carry their panel. */
    const scene::Node* garmentMesh() const
    {
        scene::Graph& g = scene::Graph::current();
        const scene::Node* n = g.node(mGarment);
        if (!n)
            return nullptr;
        for (const scene::PlugRef& d : g.destinations({mGarment, n->type().attrIndex("outMesh")}))
            if (const scene::Node* m = g.node(d.node); m && m->type().isA("mesh"))
                return m;
        return nullptr;
    }

    /** Selects the faces of a panel on the garment's mesh, so the 3D view shows the piece picked here. */
    void selectPanelFaces(int panel)
    {
        const scene::Node* mesh = garmentMesh();
        if (!mesh)
            return;
        scene::Graph& g = scene::Graph::current();
        const scene::PlugValue v = g.value({mesh->id(), scene::MeshNode::kOutMesh});
        const scene::MeshPtr* m = std::get_if<scene::MeshPtr>(&v);
        if (!m || !*m)
            return;
        const std::span<const int32_t> owner = (*m)->faceAttrs().get<int32_t>("panel");
        json faces = json::array();
        for (geo::Index f = 0; f < (*m)->faceCapacity() && f < owner.size(); ++f)
            if ((*m)->faceValid(f) && owner[f] == panel)
                faces.push_back(f);
        if (faces.empty())
            return;
        mSyncing = true;
        run("selectMode", {{"args", {"face"}}});
        run("selectComponents", {{"args", {mesh->path()}}, {"faces", faces}});
        mSyncing = false;
    }

    /** The panel most of the garment mesh's selected faces belong to becomes the one shown selected here. */
    void syncFromComponents()
    {
        const scene::Node* mesh = garmentMesh();
        if (mSyncing || !mesh)
            return;
        json sel;
        try
        {
            sel = ops::execute("componentSelection", json::object());
        }
        catch (const std::exception&)
        {
            return;
        }
        const json faces = sel.is_object() && sel.contains(mesh->path()) ? sel[mesh->path()].value("faces", json::array()) : json::array();
        if (faces.empty())
            return;
        scene::Graph& g = scene::Graph::current();
        const scene::PlugValue v = g.value({mesh->id(), scene::MeshNode::kOutMesh});
        const scene::MeshPtr* m = std::get_if<scene::MeshPtr>(&v);
        if (!m || !*m)
            return;
        const std::span<const int32_t> owner = (*m)->faceAttrs().get<int32_t>("panel");
        std::map<int, int> votes;
        for (const json& f : faces)
            if (f.is_number_integer() && f.get<size_t>() < owner.size())
                ++votes[owner[f.get<size_t>()]];
        int best = -1, most = 0;
        for (const auto& [panel, count] : votes)
            if (count > most)
                best = panel, most = count;
        if (best >= 0 && size_t(best) < mG.panels.size())
            mSelected = best;
    }

    /** Panels side by side, in a row per layer, each at its own pattern origin. */
    void layoutPanels()
    {
        mOffset.assign(mG.panels.size(), QPointF());
        std::map<int, double> rowX, rowY;
        int lowest = 0, highest = 0;
        for (const Panel& p : mG.panels)
        {
            lowest = std::min(lowest, p.layer);
            highest = std::max(highest, p.layer);
        }
        double y = 0.0;
        for (int layer = lowest; layer <= highest; ++layer)
        {
            double x = 0.0, tallest = 0.0;
            for (size_t k = 0; k < mG.panels.size(); ++k)
            {
                const Panel& p = mG.panels[k];
                if (p.layer != layer || p.outline.empty())
                    continue;
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (const Vec2& v : p.outline)
                {
                    x0 = std::min(x0, v[0]);
                    x1 = std::max(x1, v[0]);
                    y0 = std::min(y0, v[1]);
                    y1 = std::max(y1, v[1]);
                }
                mOffset[k] = QPointF(x - x0, y - y0);
                x += (x1 - x0) + 0.1;
                tallest = std::max(tallest, double(y1 - y0));
            }
            y -= tallest + 0.15;
        }
    }

    static double segDist(const QPointF& p, const QPointF& a, const QPointF& b, double* t = nullptr)
    {
        const QPointF ab = b - a;
        const double l2 = QPointF::dotProduct(ab, ab);
        const double u = l2 > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / l2, 0.0, 1.0) : 0.0;
        if (t)
            *t = u;
        const QPointF q = a + ab * u - p;
        return std::sqrt(QPointF::dotProduct(q, q));
    }

    Hit hit(const QPointF& s) const
    {
        Hit best;
        double bestD = kPick;
        for (size_t k = 0; k < mG.panels.size(); ++k)
        {
            const auto& o = mG.panels[k].outline;
            for (size_t i = 0; i < o.size(); ++i)
            {
                const QPointF d = toScreen(o[i], int(k)) - s;
                if (const double l = std::hypot(d.x(), d.y()); l < bestD)
                {
                    bestD = l;
                    best = {int(k), int(i), -1};
                }
            }
        }
        if (best.vertex >= 0)
            return best;
        bestD = kPick;
        for (size_t k = 0; k < mG.panels.size(); ++k)
        {
            const auto& o = mG.panels[k].outline;
            for (size_t i = 0; i < o.size(); ++i)
            {
                double t = 0.0;
                if (const double d = segDist(s, toScreen(o[i], int(k)), toScreen(o[(i + 1) % o.size()], int(k)), &t); d < bestD)
                {
                    bestD = d;
                    best = {int(k), -1, int(i), t};
                }
            }
        }
        if (best.edge >= 0)
            return best;
        for (size_t k = 0; k < mG.panels.size(); ++k)
            if (outlinePath(int(k)).contains(s))
                return {int(k), -1, -1};
        return {};
    }

    QPainterPath outlinePath(int k) const
    {
        QPainterPath path;
        const auto& o = mG.panels[size_t(k)].outline;
        for (size_t i = 0; i < o.size(); ++i)
            i == 0 ? path.moveTo(toScreen(o[i], k)) : path.lineTo(toScreen(o[i], k));
        path.closeSubpath();
        return path;
    }

    /** The outline points a side walks over, forward from `from` to `to`, wrapping. */
    std::vector<QPointF> sidePoints(const SeamSide& s) const
    {
        std::vector<QPointF> out;
        if (s.panel < 0 || size_t(s.panel) >= mG.panels.size())
            return out;
        const auto& o = mG.panels[size_t(s.panel)].outline;
        const int n = int(o.size());
        if (n == 0)
            return out;
        for (int i = s.from % n;; i = (i + 1) % n)
        {
            out.push_back(toScreen(o[size_t(i)], s.panel));
            if (i == s.to % n || out.size() > size_t(n))
                break;
        }
        return out;
    }

    double sideLength(const SeamSide& s) const
    {
        const auto& o = mG.panels[size_t(s.panel)].outline;
        const int n = int(o.size());
        double len = 0.0;
        for (int i = s.from % n; i != s.to % n; i = (i + 1) % n)
            len += std::hypot(o[size_t((i + 1) % n)][0] - o[size_t(i)][0], o[size_t((i + 1) % n)][1] - o[size_t(i)][1]);
        return len;
    }

    static QColor seamColour(size_t s) { return QColor::fromHsvF(float(std::fmod(0.07 + 0.618034 * double(s), 1.0)), 0.65f, 0.95f); }

    void drawGrid(QPainter& p) const
    {
        const double step = mScale * 0.1; // 10 cm
        if (step < 6)
            return;
        p.setPen(QColor(48, 48, 54));
        for (double x = std::fmod(mPan.x(), step); x < width(); x += step)
            p.drawLine(QPointF(x, 0), QPointF(x, height()));
        for (double y = std::fmod(mPan.y(), step); y < height(); y += step)
            p.drawLine(QPointF(0, y), QPointF(width(), y));
    }

    void drawPanel(QPainter& p, int k) const
    {
        const Panel& pn = mG.panels[size_t(k)];
        if (pn.outline.empty())
            return;
        const QColor base = QColor::fromHsvF(float(std::fmod(0.55 + 0.13 * k, 1.0)), 0.25f, 0.75f);
        QColor fill = base;
        fill.setAlpha(k == mSelected ? 70 : 38);
        const QPainterPath path = outlinePath(k);
        p.fillPath(path, fill);
        p.setPen(QPen(k == mSelected ? QColor(240, 240, 240) : base, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        for (const auto& line : pn.internalLines)
        {
            p.setPen(QPen(base, 1.0, Qt::DashLine));
            for (size_t i = 0; i + 1 < line.size(); ++i)
                p.drawLine(toScreen(line[i], k), toScreen(line[i + 1], k));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(220, 220, 220));
        for (const Vec2& v : pn.outline)
            p.drawRect(QRectF(toScreen(v, k) - QPointF(2.5, 2.5), QSizeF(5, 5)));
        // Notches: a tick into the panel, square to the outline.
        p.setPen(QPen(QColor(255, 170, 60), 2.0));
        const size_t n = pn.outline.size();
        for (int v : pn.notches)
        {
            if (v < 0 || size_t(v) >= n)
                continue;
            const QPointF at = toScreen(pn.outline[size_t(v)], k);
            const QPointF prev = toScreen(pn.outline[(size_t(v) + n - 1) % n], k), next = toScreen(pn.outline[(size_t(v) + 1) % n], k);
            QPointF along = next - prev;
            const double l = std::hypot(along.x(), along.y());
            if (l < 1e-6)
                continue;
            along /= l;
            // Screen y points down, so the inside of a counter-clockwise outline is on the right of travel.
            p.drawLine(at, at + QPointF(-along.y(), along.x()) * 9.0);
        }

        Vec2 c{0.f, 0.f};
        for (const Vec2& v : pn.outline)
            c = {c[0] + v[0], c[1] + v[1]};
        c = {c[0] / float(pn.outline.size()), c[1] / float(pn.outline.size())};
        // Grain: warp direction, the way the fabric's length runs.
        const QPointF a = toScreen(c, k);
        const QPointF dir(std::cos(pn.grainAngle), -std::sin(pn.grainAngle));
        const double len = std::min(60.0, mScale * 0.08);
        p.setPen(QPen(QColor(200, 200, 120), 1.2));
        p.drawLine(a - dir * len, a + dir * len);
        const QPointF side(-dir.y(), dir.x());
        p.drawLine(a + dir * len, a + dir * (len - 7) + side * 4);
        p.drawLine(a + dir * len, a + dir * (len - 7) - side * 4);
        p.setPen(QColor(230, 230, 230));
        QString label = QString::fromStdString(pn.name);
        if (pn.twin >= 0 && size_t(pn.twin) < mG.panels.size())
            label += QString("  (mirrors %1)").arg(QString::fromStdString(mG.panels[size_t(pn.twin)].name));
        p.drawText(a + QPointF(6, -6), label);
    }

    void drawSeam(QPainter& p, size_t s) const
    {
        const Seam& seam = mG.seams[s];
        const QColor c = seamColour(s);
        const float width = int(s) == mHighlightSeam ? 8.f : 4.f;
        double la = 0.0, lb = 0.0;
        QPointF ma, mb;
        auto drawSides = [&](const std::vector<SeamSide>& sides, double& len, QPointF& mid) {
            for (const SeamSide& side : sides)
            {
                if (side.panel < 0 || size_t(side.panel) >= mG.panels.size())
                    continue;
                const auto pts = sidePoints(side);
                p.setPen(QPen(c, width, Qt::SolidLine, Qt::RoundCap));
                for (size_t i = 0; i + 1 < pts.size(); ++i)
                    p.drawLine(pts[i], pts[i + 1]);
                if (!pts.empty())
                    mid = pts[pts.size() / 2];
                len += sideLength(side);
            }
        };
        drawSides(seam.a, la, ma);
        drawSides(seam.b, lb, mb);
        // Red for sides that should match but don't; a difference of 15% or more is a gather, as the pattern check reads it.
        const double ratio = std::max(la, lb) / std::max(std::min(la, lb), 1e-6) - 1.0;
        const bool mismatch = std::abs(la - lb) > 0.01 * std::max(la, lb) + 1e-4 && ratio < 0.15 && seam.shrink >= 1.f &&
                              seam.a.size() == 1 && seam.b.size() == 1; // a sleeve cap against its armhole chain eases on purpose
        p.setPen(QPen(mismatch ? QColor(240, 80, 70) : c, 1.0, Qt::DotLine));
        p.drawLine(ma, mb);
        if (mismatch)
        {
            p.setPen(QColor(240, 110, 100));
            p.drawText(0.5 * (ma + mb), QString("%1 / %2 cm").arg(cm(la), 0, 'f', 1).arg(cm(lb), 0, 'f', 1));
        }
    }

    void drawHover(QPainter& p) const
    {
        if (mSewFrom)
        {
            const auto& o = mG.panels[size_t(mSewFrom->panel)].outline;
            p.setPen(QPen(QColor(255, 255, 255), 3.0));
            p.drawLine(toScreen(o[size_t(mSewFrom->edge)], mSewFrom->panel), toScreen(o[size_t(mSewFrom->edge + 1) % o.size()], mSewFrom->panel));
            p.setPen(QPen(QColor(255, 255, 255), 1.0, Qt::DashLine));
            p.drawLine(toScreen(o[size_t(mSewFrom->edge)], mSewFrom->panel), mLast);
        }
        if (mHover.edge >= 0)
        {
            const auto& o = mG.panels[size_t(mHover.panel)].outline;
            const Vec2 a = o[size_t(mHover.edge)], b = o[size_t(mHover.edge + 1) % o.size()];
            p.setPen(QPen(QColor(255, 220, 120), 2.5));
            p.drawLine(toScreen(a, mHover.panel), toScreen(b, mHover.panel));
            p.setPen(QColor(255, 230, 160));
            p.drawText(mLast + QPointF(12, -10), QString("%1 cm").arg(cm(std::hypot(b[0] - a[0], b[1] - a[1])), 0, 'f', 1));
        }
        if (mHover.vertex >= 0)
        {
            p.setPen(QPen(QColor(255, 220, 120), 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(toScreen(mG.panels[size_t(mHover.panel)].outline[size_t(mHover.vertex)], mHover.panel), 6, 6);
        }
    }

    void sew(const Hit& a, const Hit& b, bool reversed)
    {
        auto side = [&](const Hit& h) {
            const size_t n = mG.panels[size_t(h.panel)].outline.size();
            return mG.panels[size_t(h.panel)].name + ":" + std::to_string(h.edge) + ":" + std::to_string((size_t(h.edge) + 1) % n);
        };
        run("tailorSew", {{"args", {mPath}}, {"sideA", side(a)}, {"sideB", side(b)}, {"reversed", reversed}});
    }

    /** The seams that sew outline edge `edge` of panel `panel`. */
    std::vector<int> seamsOn(int panel, int edge) const
    {
        std::vector<int> out;
        for (size_t s = 0; s < mG.seams.size(); ++s)
            for (const auto* sides : {&mG.seams[s].a, &mG.seams[s].b})
                for (const SeamSide& side : *sides)
                {
                    if (side.panel != panel)
                        continue;
                    const int n = int(mG.panels[size_t(panel)].outline.size());
                    for (int i = side.from % n; i != side.to % n; i = (i + 1) % n)
                        if (i == edge)
                            out.push_back(int(s));
                }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    void contextMenu(QMouseEvent* e)
    {
        const Hit h = hit(e->position());
        if (h.panel < 0 || mPath.empty())
            return;
        QMenu menu(this);
        const std::string name = mG.panels[size_t(h.panel)].name;
        if (h.edge >= 0)
            for (int s : seamsOn(h.panel, h.edge))
                menu.addAction(QString("Unsew Seam %1").arg(s), [this, s] { run("tailorUnsew", {{"args", {mPath}}, {"seam", {s}}}); });
        if (h.edge >= 0)
            menu.addAction("Add Dart Here", [this, name, edge = h.edge, t = h.t] {
                run("tailorAddDart", {{"args", {mPath}}, {"panel", name}, {"edge", edge}, {"at", t}});
            });
        if (h.vertex >= 0)
        {
            menu.addAction("Delete Point", [this, name, v = h.vertex] {
                run("tailorDeletePoint", {{"args", {mPath}}, {"panel", name}, {"index", v}});
            });
            const auto& notches = mG.panels[size_t(h.panel)].notches;
            if (std::find(notches.begin(), notches.end(), h.vertex) != notches.end())
                menu.addAction("Remove Notch", [this, name, v = h.vertex] {
                    run("tailorRemoveNotch", {{"args", {mPath}}, {"panel", name}, {"index", v}});
                });
        }
        menu.addAction("Mirror Panel (Linked)", [this, name] { run("tailorMirrorPanel", {{"args", {mPath}}, {"panel", name}}); });
        {
            const QPointF at = toPattern(e->position(), h.panel);
            menu.addAction("Add Pocket Here", [this, name, at] {
                run("tailorAddPocket", {{"args", {mPath}}, {"panel", name}, {"position", {cm(at.x()) - 7.0, cm(at.y()) - 8.0}}});
            });
            menu.addAction("Add Belt Loops Here", [this, name, at] {
                run("tailorAddBeltLoops", {{"args", {mPath}}, {"panel", name}, {"height", cm(at.y())}});
            });
        }
        menu.addAction("Add Collar", [this] { run("tailorAddCollar", {{"args", {mPath}}}); });
        if (mG.panels[size_t(h.panel)].twin >= 0)
            menu.addAction("Unlink Mirror", [this, name] { run("tailorUnlinkPanel", {{"args", {mPath}}, {"panel", name}}); });
        menu.addAction("Rotate Grain 90", [this, name, g = mG.panels[size_t(h.panel)].grainAngle] {
            run("tailorEditPanel", {{"args", {mPath}}, {"panel", name}, {"grainAngle", (g + 1.5707963) * 180.0 / 3.14159265}});
        });
        menu.addSeparator();
        menu.addAction("Delete Panel", [this, name] { run("tailorDeletePanel", {{"args", {mPath}}, {"panel", {name}}}); });
        menu.exec(e->globalPosition().toPoint());
    }

    Mode mMode = Mode::Edit;
    scene::NodeId mGarment = scene::kNoNode;
    std::string mPath;
    Garment mG;
    std::vector<QPointF> mOffset;
    double mScale = 400.0; // px per metre
    QPointF mPan{40, 300};
    QPointF mLast;
    bool mPanning = false;
    bool mFitted = false;
    Hit mHover, mDrag;
    Vec2 mDragStart{0.f, 0.f};
    std::optional<Hit> mSewFrom;
    std::vector<PenPoint> mPen;
    bool mSyncing = false;
    int mSelected = -1;
    int mHighlightSeam = -1;
};

QWidget* makePatternEditor(QWidget* parent)
{
    auto* w = new QWidget(parent);
    w->setMinimumWidth(460);
    auto* col = new QVBoxLayout(w);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(6, 4, 6, 4);
    auto* view = new PatternView(w);
    auto button = [&](const QString& text, const QString& tip, bool checkable) {
        auto* b = new QToolButton(w);
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setAutoExclusive(checkable);
        bar->addWidget(b);
        return b;
    };
    QToolButton* edit = button("Edit", "Drag points; double-click an edge to add a point; Delete removes the hovered point.", true);
    QToolButton* sew = button("Sew", "Click one edge, then another, to sew them; hold Shift on the second to sew them the same way round.", true);
    QToolButton* pen = button("Pen", "Click to place outline points, drag to curve through them; click the first point, double-click or "
                                     "press Enter to close the new panel. Esc cancels.", true);
    QToolButton* notch = button("Notch", "Click an edge to mark a notch. Seams with as many notches on each side sew notch to notch.", true);
    edit->setChecked(true);
    QToolButton* add = button("Add Panel", "Add a 40 x 60 cm panel in front of the garment.", false);
    QToolButton* autoSew = button("Auto-Sew", "Sew the unsewn edges that meet where the panels are placed, with lengths that match.", false);
    QToolButton* fit = button("Fit", "Frame every panel (F).", false);
    auto* title = new QLabel(w);
    // The garment name never widens the panel; the toolbar sets its floor.
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    bar->addSpacing(12);
    bar->addWidget(title, 1);
    col->addLayout(bar);
    auto* split = new QSplitter(Qt::Vertical, w);
    split->addWidget(view);
    auto* issues = new QListWidget(w);
    issues->setWordWrap(true);
    issues->setToolTip("The pattern check: what a pattern maker would flag, with what to change. Click one to see where.");
    split->addWidget(issues);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    col->addWidget(split, 1);
    view->onIssues = [issues](const json& found) {
        issues->clear();
        if (found.empty())
        {
            issues->addItem("No problems found.");
            return;
        }
        for (const json& f : found)
        {
            const std::string level = f.value("level", "info");
            auto* item = new QListWidgetItem(QString::fromStdString(f.value("message", "") + "\n" + f.value("tip", "")));
            item->setForeground(level == "error" ? QColor(240, 110, 100) : level == "warning" ? QColor(240, 200, 110) : QColor(170, 190, 220));
            item->setData(Qt::UserRole, QString::fromStdString(f.value("panel", "")));
            item->setData(Qt::UserRole + 1, f.contains("seam") ? f["seam"].get<int>() : -1);
            issues->addItem(item);
        }
    };
    QObject::connect(issues, &QListWidget::currentItemChanged, view, [view](QListWidgetItem* item) {
        if (!item)
            return;
        const std::string panel = item->data(Qt::UserRole).toString().toStdString();
        int pi = -1;
        for (size_t k = 0; k < view->garment().panels.size(); ++k)
            if (view->garment().panels[k].name == panel)
                pi = int(k);
        view->highlight(pi, item->data(Qt::UserRole + 1).toInt());
    });

    QObject::connect(edit, &QToolButton::clicked, view, [view] { view->setMode(PatternView::Mode::Edit); });
    QObject::connect(sew, &QToolButton::clicked, view, [view] { view->setMode(PatternView::Mode::Sew); });
    QObject::connect(pen, &QToolButton::clicked, view, [view] { view->setMode(PatternView::Mode::Pen); });
    QObject::connect(notch, &QToolButton::clicked, view, [view] { view->setMode(PatternView::Mode::Notch); });
    QObject::connect(add, &QToolButton::clicked, view, [view] { view->addPanel(); });
    QObject::connect(autoSew, &QToolButton::clicked, view, [view] { view->autoSew(); });
    QObject::connect(fit, &QToolButton::clicked, view, [view] { view->fit(); });
    view->onTitle = [title](const QString& t) { title->setText(t); };

    // Selection picks the garment; any undoable edit (ours, the Attribute Editor's, a script's) may change it.
    std::vector<events::SubscriptionId> subs;
    for (events::Event e : {events::Event::SelectionChanged, events::Event::UndoStackChanged, events::Event::NodeDeleted,
                            events::Event::SceneOpened, events::Event::SceneNew})
        subs.push_back(events::subscribe(e, [view](const events::EventData&) { view->refresh(); }, "Tailor"));
    QObject::connect(w, &QObject::destroyed, [subs] {
        for (events::SubscriptionId id : subs)
            events::unsubscribe(id);
    });
    view->refresh();
    return w;
}
} // namespace

void registerPatternEditor(ext::Context& ctx)
{
    ui::panels::add(ctx, {.id = "tailorPatternEditor",
                      .title = "Pattern Editor",
                      .create = makePatternEditor,
                      .icon = "shirt",
                      .open = false,
                      .docs = "tailor"});
}
} // namespace tailor
