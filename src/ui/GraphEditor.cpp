#include "ui/GraphEditor.h"

#include "cmds/Command.h"
#include "cmds/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Animation.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <map>

namespace lila::ui
{
namespace
{
namespace anim = scene::anim;
using scene::Graph;
using scene::NodeId;
using scene::PlugRef;

constexpr double kEps = 1e-6;
constexpr int kRulerHeight = 18;
constexpr double kHandleLength = 36.0;

QColor gray(int v, int a = 255) { return QColor(v, v, v, a); }

GraphEditor*& current()
{
    static GraphEditor* e = nullptr;
    return e;
}

/** "translateX" -> "Translate X". */
QString niceName(const std::string& name)
{
    QString out;
    for (size_t i = 0; i < name.size(); ++i)
    {
        char c = name[i];
        if (i == 0)
            out += QChar(c).toUpper();
        else if (std::isupper(static_cast<unsigned char>(c)))
            out += QString(' ') + QChar(c);
        else
            out += QChar(c);
    }
    return out;
}

/** Smallest of 1, 2, 5 x 10^n at least rough. */
double niceNumber(double rough)
{
    if (rough <= 0.0 || !std::isfinite(rough))
        return 1.0;
    double base = std::pow(10.0, std::floor(std::log10(rough)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (base * m >= rough)
            return base * m;
    return base * 10.0;
}

QString formatValue(double v)
{
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 3);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

/** Runs a command quietly; callers group several into one undo chunk. */
bool runQuiet(const char* command, const json& call)
{
    try
    {
        cmds::execute(command, call);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

struct Channel
{
    PlugRef plug;
    NodeId curve = scene::kNoNode;
    std::string curveName;
    QString label;
    int level = 180; // gray
    Qt::PenStyle dash = Qt::SolidLine;
    bool shown = true;
};

struct KeySel
{
    NodeId curve = scene::kNoNode;
    double time = 0.0;
};

QIcon swatch(int level, Qt::PenStyle dash)
{
    QPixmap pm(28, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setPen(QPen(gray(level), 2, dash, Qt::FlatCap));
    p.drawLine(2, 6, 26, 6);
    return QIcon(pm);
}
} // namespace

class CurveView : public QWidget
{
public:
    explicit CurveView(GraphEditor* editor) : QWidget(editor), mEditor(editor)
    {
        setObjectName("curveView");
        setMinimumSize(200, 120);
        setFocusPolicy(Qt::ClickFocus);
        setContextMenuPolicy(Qt::PreventContextMenu);
    }

    std::vector<Channel> channels;
    std::map<NodeId, anim::Curve> curves;
    std::vector<KeySel> sel;
    NodeId activeCurve = scene::kNoNode;

    bool isSelected(NodeId curve, double t) const
    {
        for (const KeySel& k : sel)
            if (k.curve == curve && std::abs(k.time - t) <= kEps)
                return true;
        return false;
    }

    const Channel* channelOf(NodeId curve) const
    {
        for (const Channel& c : channels)
            if (c.curve == curve)
                return &c;
        return nullptr;
    }

    /** Curves edits apply to: those with selected keys, else the clicked one, else every shown one. */
    std::vector<NodeId> targetCurves() const
    {
        std::vector<NodeId> out;
        for (const KeySel& k : sel)
            if (std::find(out.begin(), out.end(), k.curve) == out.end())
                out.push_back(k.curve);
        if (out.empty() && activeCurve && channelOf(activeCurve))
            out.push_back(activeCurve);
        if (out.empty())
            for (const Channel& c : channels)
                if (c.shown)
                    out.push_back(c.curve);
        return out;
    }

    void dropMissing()
    {
        std::erase_if(sel, [&](const KeySel& k) {
            auto it = curves.find(k.curve);
            return it == curves.end() || it->second.find(k.time) < 0 || !channelOf(k.curve) || !channelOf(k.curve)->shown;
        });
        if (activeCurve && !channelOf(activeCurve))
            activeCurve = scene::kNoNode;
    }

    void frame(bool selectedOnly)
    {
        double t0 = 1e300, t1 = -1e300, v0 = 1e300, v1 = -1e300;
        auto grow = [&](double t, double v) {
            t0 = std::min(t0, t);
            t1 = std::max(t1, t);
            v0 = std::min(v0, v);
            v1 = std::max(v1, v);
        };
        if (selectedOnly && !sel.empty())
        {
            for (const KeySel& k : sel)
                if (auto it = curves.find(k.curve); it != curves.end())
                    if (int i = it->second.find(k.time); i >= 0)
                        grow(k.time, it->second.keys[size_t(i)].value);
        }
        else
            for (const Channel& ch : channels)
            {
                auto it = curves.find(ch.curve);
                if (!ch.shown || it == curves.end() || it->second.keys.empty())
                    continue;
                const anim::Curve& c = it->second;
                double a = c.keys.front().time, b = c.keys.back().time;
                for (int s = 0; s <= 64; ++s)
                {
                    double t = a + (b - a) * s / 64.0;
                    grow(t, c.evaluate(t));
                }
                for (const anim::Key& k : c.keys)
                    grow(k.time, k.value);
            }
        if (t0 > t1)
            return;
        double fps = anim::playback().fps;
        if (t1 - t0 < 10.0 / fps)
        {
            t0 -= 5.0 / fps;
            t1 += 5.0 / fps;
        }
        if (v1 - v0 < 1e-3)
        {
            v0 -= 1.0;
            v1 += 1.0;
        }
        double mx = 0.08 * (t1 - t0), my = 0.12 * (v1 - v0);
        t0 -= mx;
        t1 += mx;
        v0 -= my;
        v1 += my;
        double w = std::max(1, width()), h = std::max(1, height() - kRulerHeight);
        mSx = w / (t1 - t0);
        mSy = h / (v1 - v0);
        mT0 = t0;
        mV0 = v0;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        LILA_ZONE();

        QPainter p(this);
        int w = width(), h = height();
        p.fillRect(rect(), gray(28));
        const anim::Playback& pb = anim::playback();
        double rx0 = std::max(0.0, xOf(pb.minTime)), rx1 = std::min(double(w), xOf(pb.maxTime));
        if (rx1 > rx0)
            p.fillRect(QRectF(rx0, 0, rx1 - rx0, h), gray(34));

        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);

        // Grid: frames across, values up.
        double fps = pb.fps;
        double fStep = std::max(1.0, niceNumber(70.0 / (mSx / fps)));
        for (double fr = std::floor(timeAt(0) * fps / fStep) * fStep; xOf(fr / fps) < w; fr += fStep)
        {
            double x = xOf(fr / fps);
            p.setPen(gray(42));
            p.drawLine(QPointF(x, kRulerHeight), QPointF(x, h));
            p.setPen(gray(120));
            p.drawText(QPointF(x + 3, 13), QString::number(fr, 'f', 0));
        }
        double vStep = niceNumber(40.0 / mSy);
        for (double v = std::floor(valueAt(h) / vStep) * vStep; yOf(v) > kRulerHeight; v += vStep)
        {
            double y = yOf(v);
            p.setPen(std::abs(v) < vStep * 1e-3 ? gray(70) : gray(42));
            p.drawLine(QPointF(0, y), QPointF(w, y));
            p.setPen(gray(110));
            p.drawText(QPointF(4, y - 3), formatValue(std::abs(v) < vStep * 1e-3 ? 0.0 : v));
        }
        p.fillRect(QRectF(0, 0, w, kRulerHeight), gray(24, 200));

        // Current time.
        double tx = xOf(Graph::current().time());
        p.setPen(QPen(gray(190), 1));
        p.drawLine(QPointF(tx, 0), QPointF(tx, h));
        QString frameLabel = QString::number(std::lround(anim::frameOf(Graph::current().time())));
        QRectF lr = QRectF(p.fontMetrics().boundingRect(frameLabel).adjusted(-4, -1, 4, 1));
        lr.moveTopLeft(QPointF(tx + 2, 1));
        p.fillRect(lr, gray(200));
        p.setPen(gray(20));
        p.drawText(lr, Qt::AlignCenter, frameLabel);

        p.setRenderHint(QPainter::Antialiasing);
        std::map<NodeId, anim::Curve> shownCurves;
        for (const Channel& ch : channels)
            if (ch.shown && curves.contains(ch.curve))
                shownCurves[ch.curve] = preview(ch.curve);

        std::vector<NodeId> active = activeSet();
        auto isActive = [&](NodeId c) { return std::find(active.begin(), active.end(), c) != active.end(); };
        // Active curves draw last so they sit on top.
        for (int pass = 0; pass < 2; ++pass)
            for (const Channel& ch : channels)
            {
                if (!ch.shown || !shownCurves.contains(ch.curve) || isActive(ch.curve) != (pass == 1))
                    continue;
                drawCurve(p, shownCurves[ch.curve], ch, pass == 1);
            }

        // Keys, then the tangent handles of selected ones.
        for (const Channel& ch : channels)
        {
            if (!ch.shown || !shownCurves.contains(ch.curve))
                continue;
            const anim::Curve& c = shownCurves[ch.curve];
            for (size_t i = 0; i < c.keys.size(); ++i)
            {
                const anim::Key& k = c.keys[i];
                QPointF pt(xOf(k.time), yOf(k.value));
                bool selected = isSelected(ch.curve, keyTimeBeforeDrag(ch.curve, k.time));
                if (selected)
                {
                    p.setPen(QPen(gray(150), 1));
                    QPointF in = pt - handleDir(c.inSlope(i));
                    QPointF out = pt + handleDir(c.outSlope(i));
                    p.drawLine(in, out);
                    p.setBrush(gray(170));
                    p.drawEllipse(in, 3, 3);
                    p.drawEllipse(out, 3, 3);
                }
                p.setPen(QPen(selected ? gray(255) : gray(isActive(ch.curve) ? 230 : ch.level), 1));
                p.setBrush(selected ? gray(255) : gray(22));
                p.drawRect(QRectF(pt.x() - 3, pt.y() - 3, 6, 6));
            }
        }

        if (mDrag == Drag::Box)
        {
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setPen(QPen(gray(210), 1, Qt::DashLine));
            p.setBrush(gray(255, 20));
            p.drawRect(QRectF(mPress, mLast).normalized());
        }
    }

    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        if (!mFramed && !channels.empty())
        {
            mFramed = true;
            frame(false);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        mPress = mLast = e->position();
        mMoved = false;
        bool alt = e->modifiers() & Qt::AltModifier;
        bool shift = e->modifiers() & Qt::ShiftModifier;
        bool ctrl = e->modifiers() & Qt::ControlModifier;
        mDrag = Drag::None;

        if (alt && e->button() == Qt::MiddleButton)
        {
            mDrag = Drag::Pan;
            return;
        }
        if (alt && e->button() == Qt::RightButton)
        {
            mDrag = Drag::Zoom;
            mZoomAnchorT = timeAt(mPress.x());
            mZoomAnchorV = valueAt(mPress.y());
            mZoomSx = mSx;
            mZoomSy = mSy;
            return;
        }
        if (e->button() == Qt::RightButton)
        {
            showMenu(e->globalPosition().toPoint());
            return;
        }
        if (e->button() == Qt::MiddleButton)
        {
            if (!sel.empty())
                beginMove();
            return;
        }
        if (e->button() != Qt::LeftButton)
            return;

        if (mPress.y() < kRulerHeight)
        {
            mDrag = Drag::Time;
            scrubTo(mPress.x());
            return;
        }
        if (hitHandle(mPress))
        {
            mDrag = Drag::Tangent;
            mTangentAngle = std::nan("");
            return;
        }
        if (auto hit = hitKey(mPress))
        {
            bool was = isSelected(hit->curve, hit->time);
            if (ctrl)
                std::erase_if(sel, [&](const KeySel& k) { return k.curve == hit->curve && std::abs(k.time - hit->time) <= kEps; });
            else if (shift)
            {
                if (was)
                    std::erase_if(sel, [&](const KeySel& k) { return k.curve == hit->curve && std::abs(k.time - hit->time) <= kEps; });
                else
                    sel.push_back(*hit);
            }
            else if (!was)
                sel = {*hit};
            activeCurve = hit->curve;
            if (!ctrl && isSelected(hit->curve, hit->time))
                beginMove();
            selectionChanged();
            return;
        }
        if (NodeId c = hitCurve(mPress))
        {
            activeCurve = c;
            if (!shift)
                sel.clear();
            selectionChanged();
            return;
        }
        mDrag = Drag::Box;
        mBoxMode = shift ? 1 : ctrl ? -1 : 0;
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        QPointF pos = e->position();
        QPointF d = pos - mLast;
        if ((pos - mPress).manhattanLength() > 3)
            mMoved = true;
        switch (mDrag)
        {
        case Drag::Pan:
            mT0 -= d.x() / mSx;
            mV0 += d.y() / mSy;
            break;
        case Drag::Zoom:
        {
            QPointF total = pos - mPress;
            mSx = std::clamp(mZoomSx * std::exp(total.x() * 0.01), 1e-3, 1e7);
            mSy = std::clamp(mZoomSy * std::exp(-total.y() * 0.01), 1e-6, 1e9);
            mT0 = mZoomAnchorT - mPress.x() / mSx;
            mV0 = mZoomAnchorV - (height() - mPress.y()) / mSy;
            break;
        }
        case Drag::Time: scrubTo(pos.x()); break;
        case Drag::Move:
        {
            if (!mMoved)
                break;
            double fps = anim::playback().fps;
            double dt = std::round((timeAt(pos.x()) - timeAt(mPress.x())) * fps) / fps;
            double dv = valueAt(pos.y()) - valueAt(mPress.y());
            // Shift locks to the axis the drag mostly follows.
            if (e->modifiers() & Qt::ShiftModifier)
            {
                QPointF total = pos - mPress;
                if (std::abs(total.x()) > std::abs(total.y()))
                    dv = 0.0;
                else
                    dt = 0.0;
            }
            mMoveDt = dt;
            mMoveDv = dv;
            break;
        }
        case Drag::Tangent: mTangentAngle = angleFor(pos); break;
        default: break;
        }
        mLast = pos;
        update();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        Drag drag = mDrag;
        mDrag = Drag::None;
        switch (drag)
        {
        case Drag::Box: finishBox(); break;
        case Drag::Move: commitMove(); break;
        case Drag::Tangent: commitTangent(); break;
        default: break;
        }
        update();
    }

    void wheelEvent(QWheelEvent* e) override
    {
        double f = std::pow(1.0015, e->angleDelta().y());
        QPointF pos = e->position();
        double t = timeAt(pos.x()), v = valueAt(pos.y());
        mSx = std::clamp(mSx * f, 1e-3, 1e7);
        mSy = std::clamp(mSy * f, 1e-6, 1e9);
        mT0 = t - pos.x() / mSx;
        mV0 = v - (height() - pos.y()) / mSy;
        update();
    }

private:
    enum class Drag
    {
        None,
        Pan,
        Zoom,
        Box,
        Move,
        Tangent,
        Time,
    };

    double xOf(double t) const { return (t - mT0) * mSx; }
    double yOf(double v) const { return height() - (v - mV0) * mSy; }
    double timeAt(double x) const { return mT0 + x / mSx; }
    double valueAt(double y) const { return mV0 + (height() - y) / mSy; }

    QPointF handleDir(double slope) const
    {
        QPointF d(mSx, -slope * mSy);
        double len = std::hypot(d.x(), d.y());
        return len > 0 ? d * (kHandleLength / len) : QPointF(kHandleLength, 0);
    }

    std::vector<NodeId> activeSet() const
    {
        std::vector<NodeId> out;
        for (const KeySel& k : sel)
            if (std::find(out.begin(), out.end(), k.curve) == out.end())
                out.push_back(k.curve);
        if (activeCurve && std::find(out.begin(), out.end(), activeCurve) == out.end())
            out.push_back(activeCurve);
        return out;
    }

    /** During a key drag the selection still holds the old times; maps a previewed key back. */
    double keyTimeBeforeDrag(NodeId curve, double t) const
    {
        if (mDrag != Drag::Move || !mMoved)
            return t;
        double back = t - mMoveDt;
        return isSelected(curve, back) ? back : t + 1e9;
    }

    /** The curve as it would be with the drag in progress applied. */
    anim::Curve preview(NodeId id) const
    {
        anim::Curve c = curves.at(id);
        if (mDrag == Drag::Move && mMoved && (mMoveDt != 0.0 || mMoveDv != 0.0))
        {
            std::vector<anim::Key> moved, kept;
            for (const anim::Key& k : c.keys)
            {
                if (isSelected(id, k.time))
                {
                    anim::Key m = k;
                    m.time += mMoveDt;
                    m.value += mMoveDv;
                    moved.push_back(m);
                }
                else
                    kept.push_back(k);
            }
            c.keys = kept;
            for (const anim::Key& m : moved)
            {
                if (int i = c.find(m.time); i >= 0)
                    c.keys[size_t(i)] = m;
                else
                    c.insert(m);
            }
        }
        if (mDrag == Drag::Tangent && mTangent.curve == id && !std::isnan(mTangentAngle))
            if (int i = c.find(mTangent.time); i >= 0)
            {
                anim::Key& k = c.keys[size_t(i)];
                k.in = k.out = anim::Tangent::Fixed;
                k.inAngle = k.outAngle = mTangentAngle;
            }
        return c;
    }

    void drawCurve(QPainter& p, const anim::Curve& c, const Channel& ch, bool active)
    {
        if (c.keys.empty())
            return;
        int level = active ? 255 : ch.level;
        double w = width();
        double xFirst = xOf(c.keys.front().time), xLast = xOf(c.keys.back().time);
        // Regular samples plus both sides of every key, so steps and corners land on the keys.
        auto path = [&](double x0, double x1) {
            std::vector<double> xs;
            for (double x = x0; x < x1; x += 2.0)
                xs.push_back(x);
            xs.push_back(x1);
            for (const anim::Key& k : c.keys)
            {
                double kx = xOf(k.time);
                if (kx > x0 && kx < x1)
                {
                    xs.push_back(kx - 0.01);
                    xs.push_back(kx);
                }
            }
            std::sort(xs.begin(), xs.end());
            QPainterPath out;
            for (double x : xs)
            {
                QPointF pt(x, yOf(c.evaluate(timeAt(x))));
                if (out.elementCount() == 0)
                    out.moveTo(pt);
                else
                    out.lineTo(pt);
            }
            return out;
        };
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(gray(int(level * 0.5)), 1, Qt::DotLine));
        if (xFirst > 0.0)
            p.drawPath(path(-2.0, std::min(xFirst, w + 2.0)));
        if (xLast < w)
            p.drawPath(path(std::max(xLast, -2.0), w + 2.0));
        if (xLast >= -2.0 && xFirst <= w + 2.0)
        {
            p.setPen(QPen(gray(level), active ? 2.0 : 1.4, ch.dash));
            p.drawPath(path(std::max(xFirst, -2.0), std::min(xLast, w + 2.0)));
        }
    }

    std::optional<KeySel> hitKey(QPointF pos) const
    {
        std::optional<KeySel> best;
        double bestD = 7.0;
        for (const Channel& ch : channels)
        {
            if (!ch.shown || !curves.contains(ch.curve))
                continue;
            for (const anim::Key& k : curves.at(ch.curve).keys)
            {
                double d = std::hypot(xOf(k.time) - pos.x(), yOf(k.value) - pos.y());
                if (d < bestD)
                {
                    bestD = d;
                    best = KeySel{ch.curve, k.time};
                }
            }
        }
        return best;
    }

    bool hitHandle(QPointF pos)
    {
        for (const KeySel& s : sel)
        {
            auto it = curves.find(s.curve);
            if (it == curves.end())
                continue;
            const anim::Curve& c = it->second;
            int i = c.find(s.time);
            if (i < 0)
                continue;
            QPointF pt(xOf(s.time), yOf(c.keys[size_t(i)].value));
            QPointF in = pt - handleDir(c.inSlope(size_t(i)));
            QPointF out = pt + handleDir(c.outSlope(size_t(i)));
            for (auto [h, side] : {std::pair{in, -1}, std::pair{out, 1}})
                if (std::hypot(h.x() - pos.x(), h.y() - pos.y()) < 6.0)
                {
                    mTangent = s;
                    mTangentSide = side;
                    return true;
                }
        }
        return false;
    }

    NodeId hitCurve(QPointF pos) const
    {
        double t = timeAt(pos.x());
        for (const Channel& ch : channels)
            if (ch.shown && curves.contains(ch.curve) && std::abs(yOf(curves.at(ch.curve).evaluate(t)) - pos.y()) < 5.0)
                return ch.curve;
        return scene::kNoNode;
    }

    double angleFor(QPointF pos) const
    {
        const anim::Curve& c = curves.at(mTangent.curve);
        int i = c.find(mTangent.time);
        if (i < 0)
            return 0.0;
        QPointF pt(xOf(mTangent.time), yOf(c.keys[size_t(i)].value));
        double dx = pos.x() - pt.x(), dy = pos.y() - pt.y();
        // The in handle sits left of the key; dragging it past the key flips nothing.
        if (mTangentSide > 0)
            dx = std::max(dx, 1.0);
        else
            dx = std::min(dx, -1.0);
        double slope = (-dy / mSy) / (dx / mSx);
        return std::clamp(std::atan(slope) * 180.0 / 3.14159265358979323846, -89.9, 89.9);
    }

    void scrubTo(double x)
    {
        double t = anim::snapToFrame(timeAt(x));
        if (std::abs(t - Graph::current().time()) < kEps)
            return;
        cmds::execute("currentTime", {{"args", {t}}});
        notifyTimeChanged();
    }

    void beginMove()
    {
        mDrag = Drag::Move;
        mMoveDt = mMoveDv = 0.0;
    }

    void finishBox()
    {
        if (!mMoved)
        {
            if (mBoxMode == 0)
            {
                sel.clear();
                activeCurve = scene::kNoNode;
                selectionChanged();
            }
            return;
        }
        QRectF box = QRectF(mPress, mLast).normalized();
        if (mBoxMode == 0)
            sel.clear();
        for (const Channel& ch : channels)
        {
            if (!ch.shown || !curves.contains(ch.curve))
                continue;
            for (const anim::Key& k : curves.at(ch.curve).keys)
            {
                if (!box.contains(QPointF(xOf(k.time), yOf(k.value))))
                    continue;
                bool was = isSelected(ch.curve, k.time);
                if (mBoxMode < 0)
                    std::erase_if(sel, [&](const KeySel& s) { return s.curve == ch.curve && std::abs(s.time - k.time) <= kEps; });
                else if (!was)
                    sel.push_back({ch.curve, k.time});
            }
        }
        selectionChanged();
    }

    void commitMove()
    {
        double dt = mMoveDt, dv = mMoveDv;
        mMoveDt = mMoveDv = 0.0;
        if (!mMoved || (dt == 0.0 && dv == 0.0))
            return;
        std::map<NodeId, std::vector<double>> byCurve;
        for (const KeySel& k : sel)
            byCurve[k.curve].push_back(k.time);
        {
            UndoChunk chunk("Move Keys");
            for (auto& [curve, times] : byCurve)
            {
                // Move the leading keys first so none lands on a selected key that hasn't moved yet.
                std::sort(times.begin(), times.end());
                if (dt > 0.0)
                    std::reverse(times.begin(), times.end());
                std::string name = Graph::current().node(curve)->name();
                for (double t : times)
                    runQuiet("keyframe",
                             {{"args", {name}}, {"time", t}, {"timeChange", dt}, {"valueChange", dv}, {"relative", true}});
            }
        }
        log::write(LogLevel::Echo, "# moved " + std::to_string(sel.size()) + " keys\n");
        for (KeySel& k : sel)
            k.time += dt;
        selectionChanged();
    }

    void commitTangent()
    {
        if (std::isnan(mTangentAngle) || !mMoved)
            return;
        Graph& g = Graph::current();
        if (!g.node(mTangent.curve))
            return;
        try
        {
            cmds::execute("keyTangent",
                          {{"args", {g.node(mTangent.curve)->name()}}, {"time", mTangent.time}, {"inAngle", mTangentAngle}, {"outAngle", mTangentAngle}},
                          CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
        mTangentAngle = std::nan("");
    }

    void selectionChanged()
    {
        mEditor->refreshKeyFields();
        update();
    }

    void showMenu(QPoint at)
    {
        QMenu menu(this);
        QMenu* tangents = menu.addMenu("Tangents");
        for (const char* t : {"auto", "spline", "linear", "flat", "step", "clamped"})
            tangents->addAction(niceName(t), [this, t] { mEditor->applyTangent(t); });
        for (bool pre : {true, false})
        {
            QMenu* inf = menu.addMenu(pre ? "Pre Infinity" : "Post Infinity");
            for (const char* m : {"constant", "linear", "cycle", "cycleRelative", "oscillate"})
                inf->addAction(QString(m) == "cycleRelative" ? QString("Cycle with Offset") : niceName(m),
                               [this, pre, m] { mEditor->applyInfinity(pre, m); });
        }
        menu.addSeparator();
        menu.addAction("Delete Keys", [this] { mEditor->deleteSelectedKeys(); })->setEnabled(!sel.empty());
        menu.addAction("Frame Selection", [this] { frame(true); });
        menu.addAction("Frame All", [this] { frame(false); });
        menu.exec(at);
    }

    GraphEditor* mEditor;
    double mT0 = -0.2; // time at the left edge
    double mV0 = -5.0; // value at the bottom edge
    double mSx = 200.0; // pixels per second
    double mSy = 10.0; // pixels per unit
    bool mFramed = false;

    Drag mDrag = Drag::None;
    QPointF mPress;
    QPointF mLast;
    bool mMoved = false;
    int mBoxMode = 0; // 1 add, -1 remove, 0 replace
    double mZoomAnchorT = 0.0, mZoomAnchorV = 0.0, mZoomSx = 1.0, mZoomSy = 1.0;
    double mMoveDt = 0.0, mMoveDv = 0.0;
    KeySel mTangent;
    int mTangentSide = 1;
    double mTangentAngle = std::nan("");

    friend class GraphEditor;
};

GraphEditor::GraphEditor(QWidget* parent) : QWidget(parent)
{
    current() = this;
    setProperty("hotkeyContext", "graphEditor");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    auto* bar = new QHBoxLayout();
    bar->setSpacing(2);
    for (const char* t : {"auto", "spline", "linear", "flat", "step", "clamped"})
    {
        auto* b = new QToolButton(this);
        b->setText(niceName(t));
        b->setToolTip(QString("Set the selected keys' tangents to %1").arg(t));
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QToolButton::clicked, this, [this, t] { applyTangent(t); });
        bar->addWidget(b);
    }
    auto* infinity = new QToolButton(this);
    infinity->setText("Infinity");
    infinity->setAutoRaise(true);
    infinity->setPopupMode(QToolButton::InstantPopup);
    infinity->setFocusPolicy(Qt::NoFocus);
    auto* infMenu = new QMenu(infinity);
    for (bool pre : {true, false})
    {
        QMenu* sub = infMenu->addMenu(pre ? "Pre Infinity" : "Post Infinity");
        for (const char* m : {"constant", "linear", "cycle", "cycleRelative", "oscillate"})
            sub->addAction(QString(m) == "cycleRelative" ? QString("Cycle with Offset") : niceName(m),
                           [this, pre, m] { applyInfinity(pre, m); });
    }
    infinity->setMenu(infMenu);
    bar->addWidget(infinity);
    bar->addStretch(1);
    bar->addWidget(new QLabel("Frame", this));
    mKeyTime = new QLineEdit(this);
    mKeyTime->setFixedWidth(60);
    bar->addWidget(mKeyTime);
    bar->addWidget(new QLabel("Value", this));
    mKeyValue = new QLineEdit(this);
    mKeyValue->setFixedWidth(70);
    bar->addWidget(mKeyValue);
    auto* frameAll = new QToolButton(this);
    frameAll->setText("Frame All");
    frameAll->setAutoRaise(true);
    frameAll->setFocusPolicy(Qt::NoFocus);
    connect(frameAll, &QToolButton::clicked, this, [this] { frame(false); });
    bar->addWidget(frameAll);
    layout->addLayout(bar);
    connect(mKeyTime, &QLineEdit::editingFinished, this, [this] { editKeyField(true); });
    connect(mKeyValue, &QLineEdit::editingFinished, this, [this] { editKeyField(false); });

    auto* split = new QSplitter(Qt::Horizontal, this);
    mChannels = new QTreeWidget(split);
    mChannels->setHeaderHidden(true);
    mChannels->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mChannels->setIconSize(QSize(28, 12));
    mChannels->setFocusPolicy(Qt::ClickFocus);
    mView = new CurveView(this);
    split->addWidget(mChannels);
    split->addWidget(mView);
    split->setStretchFactor(1, 1);
    split->setSizes({170, 600});
    layout->addWidget(split, 1);
    connect(mChannels, &QTreeWidget::itemSelectionChanged, this, [this] { applyVisibility(); });

    mListener = Graph::current().addListener([this](scene::Change c, NodeId id) {
        if (c == scene::Change::Value)
        {
            if (mView->curves.contains(id) && !mCurvesQueued)
            {
                mCurvesQueued = true;
                QTimer::singleShot(0, this, [this] {
                    mCurvesQueued = false;
                    reloadCurves();
                });
            }
            return;
        }
        scheduleRebuild();
    });
    onTimeChanged(this, [this] { mView->update(); });
    rebuild();
}

GraphEditor::~GraphEditor()
{
    Graph::current().removeListener(mListener);
    if (current() == this)
        current() = nullptr;
}

GraphEditor* GraphEditor::instance()
{
    return current();
}

void GraphEditor::focusInEvent(QFocusEvent* e)
{
    current() = this;
    QWidget::focusInEvent(e);
}

void GraphEditor::frame(bool selectedOnly)
{
    mView->frame(selectedOnly);
}

void GraphEditor::scheduleRebuild()
{
    if (mQueued)
        return;
    mQueued = true;
    QTimer::singleShot(0, this, [this] {
        mQueued = false;
        rebuild();
    });
}

void GraphEditor::rebuild()
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::vector<std::string> keep;
    for (QTreeWidgetItem* item : mChannels->selectedItems())
        keep.push_back(item->data(0, Qt::UserRole + 1).toString().toStdString());

    std::vector<Channel> channels;
    QSignalBlocker block(mChannels);
    mChannels->clear();
    for (NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        std::vector<anim::AnimatedPlug> plugs = anim::animatedPlugs(g, id);
        if (plugs.empty())
            continue;
        auto* top = new QTreeWidgetItem(mChannels, {QString::fromStdString(n->name())});
        top->setData(0, Qt::UserRole + 1, QString::fromStdString(n->name()));
        QFont f = top->font(0);
        f.setWeight(QFont::DemiBold);
        top->setFont(0, f);
        for (const anim::AnimatedPlug& ap : plugs)
        {
            Channel ch;
            ch.plug = ap.plug;
            ch.curve = ap.curve;
            ch.curveName = g.node(ap.curve)->name();
            const scene::AttrDef& a = g.attr(ap.plug);
            std::string attr = ap.plug.component >= 0 ? a.childNames[ap.plug.component] : a.name;
            ch.label = niceName(attr);
            // Neighbouring channels never share a look: gray level and dash both cycle, at different periods.
            static constexpr int kLevels[] = {210, 150, 180, 120};
            static constexpr Qt::PenStyle kDashes[] = {Qt::SolidLine, Qt::DashLine, Qt::DotLine};
            ch.level = kLevels[channels.size() % 4];
            ch.dash = kDashes[channels.size() % 3];
            auto* item = new QTreeWidgetItem(top, {ch.label});
            item->setIcon(0, swatch(ch.level, ch.dash));
            item->setData(0, Qt::UserRole, int(channels.size()));
            item->setData(0, Qt::UserRole + 1, QString::fromStdString(g.plugName(ap.plug)));
            channels.push_back(std::move(ch));
        }
        top->setExpanded(true);
    }
    for (QTreeWidgetItemIterator it(mChannels); *it; ++it)
        if (std::find(keep.begin(), keep.end(), (*it)->data(0, Qt::UserRole + 1).toString().toStdString()) != keep.end())
            (*it)->setSelected(true);

    bool wasEmpty = mView->channels.empty();
    mView->channels = std::move(channels);
    reloadCurves();
    applyVisibility();
    if (wasEmpty && !mView->channels.empty() && mView->isVisible())
    {
        mView->mFramed = true;
        mView->frame(false);
    }
}

void GraphEditor::reloadCurves()
{
    Graph& g = Graph::current();
    mView->curves.clear();
    for (const Channel& ch : mView->channels)
    {
        scene::Node* n = g.node(ch.curve);
        if (!n)
            continue;
        try
        {
            mView->curves[ch.curve] = anim::curveOf(*n);
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", n->name(), e.what());
        }
    }
    mView->dropMissing();
    refreshKeyFields();
    mView->update();
}

void GraphEditor::applyVisibility()
{
    std::vector<int> picked;
    for (QTreeWidgetItem* item : mChannels->selectedItems())
    {
        if (item->childCount() == 0)
            picked.push_back(item->data(0, Qt::UserRole).toInt());
        for (int c = 0; c < item->childCount(); ++c)
            picked.push_back(item->child(c)->data(0, Qt::UserRole).toInt());
    }
    for (int i = 0; i < int(mView->channels.size()); ++i)
        mView->channels[i].shown = picked.empty() || std::find(picked.begin(), picked.end(), i) != picked.end();
    mView->dropMissing();
    refreshKeyFields();
    mView->update();
}

void GraphEditor::applyTangent(const char* type)
{
    Graph& g = Graph::current();
    bool step = std::string_view(type) == "step";
    UndoChunk chunk("Key Tangents");
    if (!mView->sel.empty())
    {
        for (const KeySel& k : mView->sel)
            if (scene::Node* n = g.node(k.curve))
            {
                json call = {{"args", {n->name()}}, {"time", k.time}, {"outTangentType", type}};
                if (!step)
                    call["inTangentType"] = type;
                runQuiet("keyTangent", call);
            }
    }
    else
        for (NodeId c : mView->targetCurves())
            if (scene::Node* n = g.node(c))
            {
                json call = {{"args", {n->name()}}, {"outTangentType", type}};
                if (!step)
                    call["inTangentType"] = type;
                runQuiet("keyTangent", call);
            }
    log::write(LogLevel::Echo, std::string("# tangents: ") + type + "\n");
}

void GraphEditor::applyInfinity(bool pre, const char* mode)
{
    Graph& g = Graph::current();
    json names = json::array();
    for (NodeId c : mView->targetCurves())
        if (scene::Node* n = g.node(c))
            names.push_back(n->name());
    if (names.empty())
        return;
    try
    {
        cmds::execute("setInfinity", {{"args", names}, {pre ? "preInfinite" : "postInfinite", mode}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void GraphEditor::deleteSelectedKeys()
{
    if (mView->sel.empty())
        return;
    Graph& g = Graph::current();
    std::vector<KeySel> doomed = mView->sel;
    mView->sel.clear();
    {
        UndoChunk chunk("Delete Keys");
        for (const KeySel& k : doomed)
            if (scene::Node* n = g.node(k.curve))
                runQuiet("cutKey", {{"args", {n->name()}}, {"time", k.time}, {"clear", true}});
    }
    log::write(LogLevel::Echo, "# deleted " + std::to_string(doomed.size()) + " keys\n");
    refreshKeyFields();
}

void GraphEditor::refreshKeyFields()
{
    const auto& sel = mView->sel;
    std::optional<double> t, v;
    bool sameT = true, sameV = true;
    for (const KeySel& k : sel)
    {
        auto it = mView->curves.find(k.curve);
        if (it == mView->curves.end())
            continue;
        int i = it->second.find(k.time);
        if (i < 0)
            continue;
        double value = it->second.keys[size_t(i)].value;
        if (t && std::abs(*t - k.time) > kEps)
            sameT = false;
        if (v && std::abs(*v - value) > 1e-9)
            sameV = false;
        t = k.time;
        v = value;
    }
    mKeyTime->setEnabled(sel.size() == 1);
    mKeyValue->setEnabled(!sel.empty());
    mKeyTime->setText(t && sameT ? formatValue(anim::frameOf(*t)) : QString());
    mKeyValue->setText(v && sameV ? formatValue(*v) : QString());
}

void GraphEditor::editKeyField(bool time)
{
    QLineEdit* field = time ? mKeyTime : mKeyValue;
    bool ok = false;
    double x = field->text().toDouble(&ok);
    if (!ok || mView->sel.empty())
        return;
    Graph& g = Graph::current();
    if (time)
    {
        if (mView->sel.size() != 1)
            return;
        KeySel& k = mView->sel[0];
        double to = anim::secondsOf(x);
        if (std::abs(to - k.time) < kEps)
            return;
        if (scene::Node* n = g.node(k.curve))
            if (runQuiet("keyframe", {{"args", {n->name()}}, {"time", k.time}, {"timeChange", to}}))
                k.time = to;
        return;
    }
    UndoChunk chunk("Key Values");
    for (const KeySel& k : mView->sel)
        if (scene::Node* n = g.node(k.curve))
            runQuiet("keyframe", {{"args", {n->name()}}, {"time", k.time}, {"valueChange", x}});
}
} // namespace lila::ui
