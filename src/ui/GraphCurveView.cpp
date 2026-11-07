#include "ui/GraphCurveView.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "ui/Animation.h"
#include "ui/AudioUi.h"
#include "ui/GraphEditor.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"

#include <QDateTime>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>

#include <array>
#include <cmath>

namespace lila::ui::graphed
{
using scene::Graph;

namespace
{
constexpr double kHandleLength = 36.0;
constexpr double kStripHeight = 72.0;
constexpr double kBandHalf = 1.25; // a Stacked band spans its -1..1 plus a margin

double cvDegrees(double slope) { return std::atan(slope) * 180.0 / 3.14159265358979323846; }
double cvRadians(double degrees) { return degrees * 3.14159265358979323846 / 180.0; }
double cvFps() { return anim::playback().fps > 0.0 ? anim::playback().fps : 30.0; }

/** Region handles 0..3 are corners, 4..7 the top, right, bottom and left edges; each needs extent along what it scales. */
bool regionHandleUsable(int i, const QRectF& r)
{
    const bool wide = r.width() >= 1.0, tall = r.height() >= 1.0;
    if (i < 4)
        return wide && tall;
    return i == 4 || i == 6 ? tall : wide;
}

std::array<QPointF, 8> regionHandles(const QRectF& o)
{
    return {o.topLeft(), o.topRight(), o.bottomRight(), o.bottomLeft(), QPointF(o.center().x(), o.top()), QPointF(o.right(), o.center().y()),
            QPointF(o.center().x(), o.bottom()), QPointF(o.left(), o.center().y())};
}

/** Gray brightened towards the soft-select orange by w. */
QColor falloffTint(double w)
{
    const QColor a = gray(22), b(255, 160, 60);
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * w), float(a.greenF() + (b.greenF() - a.greenF()) * w),
                            float(a.blueF() + (b.blueF() - a.blueF()) * w));
}
} // namespace

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

bool runUnechoed(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call);
        return true;
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return false;
    }
}

bool axisColors() { return viewportDisplay::accentColors(viewportDisplay::kGraphAxisColors); }

QColor channelColor(const Channel& ch, int level)
{
    if (ch.axis < 0 || !axisColors())
        return gray(level);
    glm::vec3 c = viewportDisplay::axisColor(ch.axis);
    float k = float(level) / 210.0f;
    c = k <= 1.0f ? c * k : glm::mix(c, glm::vec3(1.0f), std::min(1.0f, (k - 1.0f) * 1.5f));
    return QColor::fromRgbF(c.r, c.g, c.b);
}

double quatComponent(const glm::dquat& q, int c) { return c == 0 ? q.w : c == 1 ? q.x : c == 2 ? q.y : q.z; }

anim::Curve derivedChannel(const anim::RotationCurve& rc, int slot)
{
    anim::Curve out;
    out.weighted = rc.weighted;
    glm::dquat ref = rc.keys.empty() ? glm::dquat(1.0, 0.0, 0.0, 0.0) : rc.keys.front().q;
    auto quatAt = [&](double t) {
        glm::dquat q = rc.evaluateQuat(t);
        return quatComponent(glm::dot(q, ref) < 0.0 ? -q : q, slot - 3);
    };
    for (size_t i = 0; i < rc.keys.size(); ++i)
    {
        const anim::RotKey& k = rc.keys[i];
        anim::Key d{.time = k.time, .in = anim::Tangent::Fixed, .out = anim::Tangent::Fixed};
        d.inWeight = k.inWeight;
        d.outWeight = k.outWeight;
        d.broken = k.broken;
        double inSlope, outSlope;
        if (slot < 3)
        {
            d.value = k.euler[slot];
            inSlope = rc.eulerSlope(i, slot, false);
            outSlope = rc.eulerSlope(i, slot, true);
        }
        else
        {
            constexpr double h = 1e-4;
            d.value = quatAt(k.time);
            inSlope = (d.value - quatAt(k.time - h)) / h;
            outSlope = (quatAt(k.time + h) - d.value) / h;
        }
        d.inAngle = cvDegrees(inSlope);
        d.outAngle = cvDegrees(outSlope);
        out.keys.push_back(d);
    }
    return out;
}

CurveView::CurveView(GraphEditor* editor) : QWidget(editor), mEditor(editor)
{
    setObjectName("curveView");
    setMinimumSize(200, 120);
    setFocusPolicy(Qt::ClickFocus);
    setContextMenuPolicy(Qt::PreventContextMenu);
    mAxisColors = axisColors();
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file != "general" || axisColors() == mAxisColors)
            return;
        mAxisColors = !mAxisColors;
        mEditor->scheduleRebuild();
    });
}

CurveView::~CurveView() { PrefStore::get().removeOnChanged(mPrefListener); }

std::string CurveView::commandTarget(NodeId id) const
{
    Graph& g = Graph::current();
    int slot = slotOf(id);
    if (slot < 0)
    {
        scene::Node* n = g.node(id);
        return n ? n->path() : std::string();
    }
    if (slot > 2 || !g.node(realCurve(id)))
        return {};
    const Channel* ch = channelOf(id);
    return ch ? g.plugName(ch->plug) : std::string();
}

const Channel* CurveView::channelOf(NodeId curve) const
{
    for (const Channel& c : channels)
        if (c.curve == curve)
            return &c;
    return nullptr;
}

bool CurveView::isSelected(NodeId curve, double t) const
{
    for (const KeySel& k : sel)
        if (k.curve == curve && std::abs(k.time - t) <= kEps)
            return true;
    return false;
}

std::vector<NodeId> CurveView::targetCurves() const
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

void CurveView::dropMissing()
{
    std::erase_if(sel, [&](const KeySel& k) {
        auto it = curves.find(k.curve);
        return it == curves.end() || it->second.find(k.time) < 0 || !channelOf(k.curve) || !channelOf(k.curve)->shown;
    });
    if (activeCurve && !channelOf(activeCurve))
        activeCurve = scene::kNoNode;
}

std::vector<double> CurveView::selectedTimes(NodeId id) const
{
    std::vector<double> out;
    for (const KeySel& k : sel)
        if (k.curve == id)
            out.push_back(k.time);
    return out;
}

json CurveView::keysArgument() const
{
    std::map<NodeId, std::vector<double>> byCurve;
    for (const KeySel& k : sel)
        byCurve[k.curve].push_back(k.time);
    json keys = json::array();
    for (auto& [id, times] : byCurve)
        if (std::string target = commandTarget(id); !target.empty())
            keys.push_back({{"curve", target}, {"times", times}});
    return keys;
}

json CurveView::targetNames() const
{
    json names = json::array();
    for (NodeId id : targetCurves())
        if (std::string target = commandTarget(id); !target.empty() && std::find(names.begin(), names.end(), target) == names.end())
            names.push_back(target);
    return names;
}

void CurveView::updateMaps()
{
    int band = 0;
    for (Channel& ch : channels)
    {
        ch.mapScale = 1.0;
        ch.mapOffset = 0.0;
        ch.bandCentre = 0.0;
        auto it = curves.find(ch.curve);
        if (settings.view == ViewMode::Absolute || !ch.shown || it == curves.end() || it->second.keys.empty())
            continue;
        const anim::Curve& c = it->second;
        auto eval = sampler(ch.curve, c);
        double lo = 1e300, hi = -1e300;
        const double a = c.keys.front().time, b = c.keys.back().time;
        for (int s = 0; s <= 64; ++s)
        {
            const double v = eval(a + (b - a) * s / 64.0);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        for (const anim::Key& k : c.keys)
        {
            lo = std::min(lo, k.value);
            hi = std::max(hi, k.value);
        }
        if (hi - lo < 1e-9)
        {
            ch.mapOffset = -lo;
        }
        else
        {
            ch.mapScale = 2.0 / (hi - lo);
            ch.mapOffset = -1.0 - lo * ch.mapScale;
        }
        if (settings.view == ViewMode::Stacked)
        {
            ch.bandCentre = -2.0 * kBandHalf * band++;
            ch.mapOffset += ch.bandCentre;
        }
    }
}

void CurveView::frame(bool selectedOnly)
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
                    if (const Channel* ch = channelOf(k.curve))
                        grow(k.time, ch->display(it->second.keys[size_t(i)].value));
    }
    else
        for (const Channel& ch : channels)
        {
            auto it = curves.find(ch.curve);
            if (!ch.shown || it == curves.end() || it->second.keys.empty())
                continue;
            const anim::Curve& c = it->second;
            auto eval = sampler(ch.curve, c);
            double a = c.keys.front().time, b = c.keys.back().time;
            for (int s = 0; s <= 64; ++s)
            {
                double t = a + (b - a) * s / 64.0;
                grow(t, ch.display(eval(t)));
            }
            for (const anim::Key& k : c.keys)
                grow(k.time, ch.display(k.value));
            if (settings.view == ViewMode::Stacked)
            {
                grow(a, ch.bandCentre - kBandHalf);
                grow(a, ch.bandCentre + kBandHalf);
            }
        }
    if (t0 > t1)
        return;
    double fps = cvFps();
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
    double mx = 0.08 * (t1 - t0), my = (settings.view == ViewMode::Stacked ? 0.02 : 0.12) * (v1 - v0);
    t0 -= mx;
    t1 += mx;
    v0 -= my;
    v1 += my;
    double w = std::max(1, width()), h = std::max(1.0, plotBottom() - kRulerHeight);
    mSx = w / (t1 - t0);
    mSy = h / (v1 - v0);
    mT0 = t0;
    mV0 = v0;
    update();
}

void CurveView::loopKeys(bool on)
{
    Graph& g = Graph::current();
    json names = json::array();
    std::set<NodeId> done;
    for (NodeId c : targetCurves())
        if (scene::Node* n = g.node(realCurve(c)); n && done.insert(n->id()).second)
            names.push_back(n->path());
    if (names.empty())
        return;
    try
    {
        ops::execute(on ? "setLoopKey" : "clearLoopKey", {{"args", names}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void CurveView::beginTween(anim::BlendMode mode)
{
    mTweening = !sel.empty();
    mTweenMode = mode;
    mTweenAmount = 0.0;
}

void CurveView::setTween(double amount)
{
    if (!mTweening)
        return;
    mTweenAmount = amount;
    pushPreviews();
    update();
}

void CurveView::endTween()
{
    if (!mTweening)
        return;
    dropPreviews();
    mTweening = false;
    if (std::abs(mTweenAmount) > 1e-6)
    {
        try
        {
            ops::execute("blendKeys", {{"keys", keysArgument()}, {"mode", anim::blendModeName(mTweenMode)}, {"amount", mTweenAmount}},
                         CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }
    mTweenAmount = 0.0;
    update();
}

double CurveView::stripHeight() const
{
    return kStripHeight * ((settings.speedStrip ? 1 : 0) + (settings.accelStrip ? 1 : 0));
}

std::vector<NodeId> CurveView::activeSet() const
{
    std::vector<NodeId> out;
    for (const KeySel& k : sel)
        if (std::find(out.begin(), out.end(), k.curve) == out.end())
            out.push_back(k.curve);
    if (activeCurve && std::find(out.begin(), out.end(), activeCurve) == out.end())
        out.push_back(activeCurve);
    return out;
}

double CurveView::defaultFor(const Channel& ch) const
{
    Graph& g = Graph::current();
    if (!ch.plug.node || !g.node(ch.plug.node))
        return 0.0;
    const scene::PlugValue& d = g.attr(ch.plug).defaultValue;
    if (const auto* v = std::get_if<double>(&d))
        return *v;
    if (const auto* v = std::get_if<glm::dvec3>(&d))
        return (*v)[std::clamp(ch.plug.component, 0, 2)];
    return 0.0;
}

anim::KeyTransform CurveView::transformFor(NodeId id) const
{
    anim::KeyTransform x = mXform;
    const Channel* ch = channelOf(id);
    x.valueOffset = ch ? mDisplayOffset / ch->mapScale : mDisplayOffset;
    auto it = mValuePivots.find(id);
    x.valuePivot = it != mValuePivots.end() ? it->second : 0.0;
    x.falloff = settings.falloff ? settings.falloffFrames / cvFps() : 0.0;
    x.snap = std::abs(x.timeScale - 1.0) > 1e-12;
    return x;
}

double CurveView::keyTimeBeforeDrag(NodeId curve, double t) const
{
    if (!transforming())
        return t;
    const anim::KeyTransform x = transformFor(curve);
    for (const KeySel& k : sel)
        if (k.curve == curve && std::abs(x.time(k.time, cvFps()) - t) <= kEps)
            return k.time;
    return t + 1e9;
}

double CurveView::falloffWeightAt(NodeId id, double t) const
{
    if (!transforming() || !settings.falloff || isSelected(id, t))
        return 0.0;
    double nearest = 1e300;
    for (const KeySel& k : sel)
        if (k.curve == id)
            nearest = std::min(nearest, std::abs(k.time - t));
    return anim::falloffWeight(nearest, settings.falloffFrames / cvFps());
}

anim::RotationCurve CurveView::previewRotation(NodeId realId) const
{
    anim::RotationCurve rc = rotations.at(realId);
    if (transforming())
    {
        std::array<std::vector<double>, 3> times;
        std::array<anim::KeyTransform, 3> xs;
        for (int a = 0; a < 3; ++a)
        {
            times[size_t(a)] = selectedTimes(slotId(realId, a));
            xs[size_t(a)] = transformFor(slotId(realId, a));
        }
        anim::transformKeys(rc, times, xs, cvFps());
    }
    if (mTweening)
        for (int a = 0; a < 3; ++a)
            if (const Channel* ch = channelOf(slotId(realId, a)))
                if (std::vector<double> times = selectedTimes(ch->curve); !times.empty())
                    anim::blendKeys(rc, a, times, mTweenMode, mTweenAmount, defaultFor(*ch));
    return rc;
}

anim::Curve CurveView::preview(NodeId id) const
{
    const int slot = slotOf(id);
    anim::Curve c;
    if (slot >= 0 && (transforming() || mTweening) && rotations.contains(realCurve(id)))
        c = derivedChannel(previewRotation(realCurve(id)), slot);
    else
    {
        c = curves.at(id);
        if (slot < 0 && transforming())
            anim::transformKeys(c, selectedTimes(id), transformFor(id), cvFps());
        if (slot < 0 && mTweening)
            if (const Channel* ch = channelOf(id); ch && !selectedTimes(id).empty())
                anim::blendKeys(c, selectedTimes(id), mTweenMode, mTweenAmount, defaultFor(*ch));
    }
    if (mDrag == Drag::Tangent && mTangent.curve == id && mMoved)
        if (int i = c.find(mTangent.time); i >= 0)
        {
            anim::Key& k = c.keys[size_t(i)];
            const bool out = mTangentSide > 0;
            if (!std::isnan(mTangentAngle))
            {
                if (k.broken)
                {
                    const double other = out ? c.inSlope(size_t(i)) : c.outSlope(size_t(i));
                    k.in = k.out = anim::Tangent::Fixed;
                    (out ? k.outAngle : k.inAngle) = mTangentAngle;
                    (out ? k.inAngle : k.outAngle) = cvDegrees(other);
                }
                else
                {
                    k.in = k.out = anim::Tangent::Fixed;
                    k.inAngle = k.outAngle = mTangentAngle;
                }
            }
            if (!std::isnan(mTangentWeight))
                (out ? k.outWeight : k.inWeight) = mTangentWeight;
        }
    if (mDrag == Drag::TangentRotate && mMoved)
        if (const Channel* ch = channelOf(id))
            for (size_t i = 0; i < c.keys.size(); ++i)
                if (isSelected(id, c.keys[i].time))
                {
                    const double in = c.inSlope(i), out = c.outSlope(i);
                    anim::Key& k = c.keys[i];
                    k.in = k.out = anim::Tangent::Fixed;
                    k.inAngle = cvDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, in) + mRotateDelta));
                    k.outAngle = cvDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, out) + mRotateDelta));
                }
    return c;
}

std::function<double(double)> CurveView::sampler(NodeId id, const anim::Curve& c) const
{
    const Channel* ch = channelOf(id);
    std::vector<anim::Modifier> mods = ch ? ch->modifiers : std::vector<anim::Modifier>{};
    const int slot = slotOf(id);
    auto it = rotations.find(realCurve(id));
    const bool tangentEdit = (mDrag == Drag::Tangent && mTangent.curve == id) || (mDrag == Drag::TangentRotate && !selectedTimes(id).empty());
    if (slot < 0 || it == rotations.end() || (slot < 3 && tangentEdit && mMoved))
    {
        if (mods.empty())
            return [c](double t) { return c.evaluate(t); };
        return [c, mods](double t) { return anim::evaluateModified(c, mods, t); };
    }
    anim::RotationCurve rc = (transforming() || mTweening) ? previewRotation(realCurve(id)) : it->second;
    if (slot < 3)
    {
        if (mods.empty())
            return [rc, slot](double t) { return rc.evaluate(t)[slot]; };
        return [rc, slot, mods](double t) { return anim::evaluateModified(rc, mods, t)[slot]; };
    }
    glm::dquat ref = rc.keys.empty() ? glm::dquat(1.0, 0.0, 0.0, 0.0) : rc.keys.front().q;
    return [rc, slot, ref](double t) {
        glm::dquat q = rc.evaluateQuat(t);
        return quatComponent(glm::dot(q, ref) < 0.0 ? -q : q, slot - 3);
    };
}

double CurveView::screenAngleOf(const Channel& ch, double slope) const
{
    return cvDegrees(slope * ch.mapScale * mSy / mSx);
}

double CurveView::slopeOfScreenAngle(const Channel& ch, double degrees) const
{
    return std::tan(cvRadians(std::clamp(degrees, -89.9, 89.9))) * mSx / (mSy * ch.mapScale);
}

QPointF CurveView::handlePoint(const anim::Curve& c, size_t i, bool out, const Channel& ch) const
{
    const anim::Key& k = c.keys[i];
    const QPointF pt(xOf(k.time), yOfKey(ch, k.value));
    const double slope = out ? c.outSlope(i) : c.inSlope(i);
    const bool neighbour = out ? i + 1 < c.keys.size() : i > 0;
    if (c.weighted && neighbour)
    {
        const double dt = out ? c.keys[i + 1].time - k.time : k.time - c.keys[i - 1].time;
        const double h = (out ? k.outWeight : k.inWeight) * dt / 3.0;
        const double t = out ? k.time + h : k.time - h;
        const double v = out ? k.value + slope * h : k.value - slope * h;
        return QPointF(xOf(t), yOfKey(ch, v));
    }
    QPointF d(mSx, -slope * ch.mapScale * mSy);
    const double len = std::hypot(d.x(), d.y());
    d = len > 0 ? d * (kHandleLength / len) : QPointF(kHandleLength, 0);
    return out ? pt + d : pt - d;
}

void CurveView::drawCurve(QPainter& p, const anim::Curve& c, const Channel& ch, bool active, const std::function<double(double)>& eval,
                          const QColor* ghost)
{
    if (c.keys.empty())
        return;
    int level = active ? 255 : ch.level;
    // Curves the selection isn't on recede while some are picked.
    if (!active && !activeSet().empty())
        level = int(level * 0.6);
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
            QPointF pt(x, yOfKey(ch, eval(timeAt(x))));
            if (out.elementCount() == 0)
                out.moveTo(pt);
            else
                out.lineTo(pt);
        }
        return out;
    };
    p.setBrush(Qt::NoBrush);
    if (ghost)
    {
        p.setPen(QPen(*ghost, 1.2, Qt::DashLine));
        p.drawPath(path(-2.0, w + 2.0));
        return;
    }
    p.setPen(QPen(channelColor(ch, int(level * 0.5)), 1, Qt::DotLine));
    if (xFirst > 0.0)
        p.drawPath(path(-2.0, std::min(xFirst, w + 2.0)));
    if (xLast < w)
        p.drawPath(path(std::max(xLast, -2.0), w + 2.0));
    if (xLast >= -2.0 && xFirst <= w + 2.0)
    {
        p.setPen(QPen(channelColor(ch, level), active ? 2.0 : 1.4, ch.dash));
        p.drawPath(path(std::max(xFirst, -2.0), std::min(xLast, w + 2.0)));
    }
}

void CurveView::drawGrid(QPainter& p)
{
    const int w = width();
    const double bottom = plotBottom();
    const double fps = cvFps();
    double fStep = std::max(1.0, niceNumber(70.0 / (mSx / fps)));
    for (double fr = std::floor(timeAt(0) * fps / fStep) * fStep; xOf(fr / fps) < w; fr += fStep)
    {
        double x = xOf(fr / fps);
        p.setPen(gray(42));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, bottom));
        p.setPen(gray(120));
        p.drawText(QPointF(x + 3, 13), QString::number(fr, 'f', 0));
    }
    if (settings.view == ViewMode::Stacked)
    {
        int band = 0;
        for (const Channel& ch : channels)
        {
            if (!ch.shown || !curves.contains(ch.curve))
                continue;
            const double top = yOf(ch.bandCentre + kBandHalf), bot = yOf(ch.bandCentre - kBandHalf);
            if (bot < kRulerHeight || top > bottom)
            {
                ++band;
                continue;
            }
            p.fillRect(QRectF(0, top, w, bot - top), band++ % 2 ? gray(30) : gray(27));
            for (double d : {-1.0, 0.0, 1.0})
            {
                p.setPen(d == 0.0 ? gray(48) : gray(40));
                p.drawLine(QPointF(0, yOf(ch.bandCentre + d)), QPointF(w, yOf(ch.bandCentre + d)));
            }
            p.setPen(channelColor(ch, 170));
            p.drawText(QPointF(4, top + 12), ch.label);
            // The band's real range at the right, clear of the name.
            p.setPen(gray(105));
            for (double d : {1.0, -1.0})
            {
                const QString text = formatValue(ch.value(ch.bandCentre + d));
                p.drawText(QPointF(w - p.fontMetrics().horizontalAdvance(text) - 6, yOf(ch.bandCentre + d) - 3), text);
            }
        }
        return;
    }
    double vStep = niceNumber(40.0 / mSy);
    for (double v = std::floor(displayAt(bottom) / vStep) * vStep; yOf(v) > kRulerHeight; v += vStep)
    {
        double y = yOf(v);
        p.setPen(std::abs(v) < vStep * 1e-3 ? gray(70) : gray(42));
        p.drawLine(QPointF(0, y), QPointF(w, y));
        p.setPen(gray(110));
        p.drawText(QPointF(4, y - 3), formatValue(std::abs(v) < vStep * 1e-3 ? 0.0 : v));
    }
}

void CurveView::drawStrips(QPainter& p, const std::map<NodeId, anim::Curve>& shown)
{
    if (stripHeight() <= 0.0)
        return;
    const double fps = cvFps();
    const int w = width();
    Graph& g = Graph::current();
    const bool world = settings.quantity != Quantity::Channels;
    const bool angular = settings.quantity == Quantity::WorldAngular;
    std::vector<MotionSeries> speed, accel;
    QString unit;
    if (world)
    {
        std::vector<uint64_t> nodes;
        for (NodeId id : g.selection())
            if (scene::Node* n = g.node(id); n && n->type().isA("transform"))
                nodes.push_back(id);
        // Whole blocks of 30 frames, so panning reuses the samples.
        const double block = 30.0 / fps;
        const double t0 = std::floor(timeAt(0) / block) * block, t1 = std::ceil(timeAt(w) / block) * block;
        mWorld.update(g, nodes, t0, t1, fps);
        speed = mWorld.speed(angular);
        accel = mWorld.acceleration(angular);
        unit = angular ? "°" : "cm";
    }
    else
    {
        QString shared;
        bool same = true;
        for (const Channel& ch : channels)
        {
            auto it = shown.find(ch.curve);
            if (!ch.shown || it == shown.end())
                continue;
            if (shared.isEmpty() && same)
                shared = ch.unit;
            same = same && ch.unit == shared;
            auto eval = sampler(ch.curve, it->second);
            MotionSeries s{ch.label, channelColor(ch, ch.level), {}}, a{ch.label, channelColor(ch, ch.level), {}};
            const double h = 0.25 / fps, h2 = 0.5 / fps;
            for (int x = -2; x <= w + 2; x += 2)
            {
                const double t = timeAt(x);
                if (settings.speedStrip)
                    s.samples.push_back({t, rateOf(eval, t, h)});
                if (settings.accelStrip)
                    a.samples.push_back({t, (eval(t + h2) - 2.0 * eval(t) + eval(t - h2)) / (h2 * h2)});
            }
            speed.push_back(std::move(s));
            accel.push_back(std::move(a));
        }
        unit = same ? shared : QString();
    }
    double y = plotBottom();
    const double now = g.time();
    auto xf = [this](double t) { return xOf(t); };
    if (settings.speedStrip)
    {
        StripStyle style{.title = angular ? "Angular Speed" : "Speed", .unit = unit + "/s", .magnitude = world};
        if (settings.targetSpeed > 0.0)
            style.target = settings.targetSpeed;
        drawStrip(p, QRectF(0, y, w, kStripHeight), speed, style, xf, now);
        y += kStripHeight;
    }
    if (settings.accelStrip)
    {
        StripStyle style{.title = angular ? "Angular Acceleration" : "Acceleration", .unit = unit + "/s²", .flagSpikes = true, .magnitude = world};
        drawStrip(p, QRectF(0, y, w, kStripHeight), accel, style, xf, now);
    }
}

QRectF CurveView::regionRect() const
{
    if (settings.tool != Tool::Move || sel.size() < 2)
        return {};
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (const KeySel& k : sel)
    {
        auto it = curves.find(k.curve);
        const Channel* ch = channelOf(k.curve);
        if (it == curves.end() || !ch)
            continue;
        int i = it->second.find(k.time);
        if (i < 0)
            continue;
        const double x = xOf(k.time), y = yOfKey(*ch, it->second.keys[size_t(i)].value);
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    if (x0 > x1 || (x1 - x0 < 1.0 && y1 - y0 < 1.0))
        return {};
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

int CurveView::hitRegionHandle(QPointF pos) const
{
    const QRectF r = regionRect();
    if (r.isNull())
        return -1;
    const auto pts = regionHandles(r.adjusted(-8, -8, 8, 8));
    for (int i = 0; i < 8; ++i)
        if (regionHandleUsable(i, r) && std::abs(pts[size_t(i)].x() - pos.x()) <= 5 && std::abs(pts[size_t(i)].y() - pos.y()) <= 5)
            return i;
    return -1;
}

void CurveView::paintEvent(QPaintEvent*)
{
    LILA_ZONE();

    QPainter p(this);
    const int w = width();
    const double bottom = plotBottom();
    p.fillRect(rect(), gray(28));
    const anim::Playback& pb = anim::playback();
    double rx0 = std::max(0.0, xOf(pb.minTime)), rx1 = std::min(double(w), xOf(pb.maxTime));
    if (rx1 > rx0)
        p.fillRect(QRectF(rx0, 0, rx1 - rx0, bottom), gray(34));

    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    p.setFont(f);
    p.save();
    p.setClipRect(QRectF(0, 0, w, bottom));
    drawGrid(p);
    p.fillRect(QRectF(0, 0, w, kRulerHeight), gray(24, 200));
    audio::drawWaveform(p, QRectF(0, bottom - 52, w, 48), timeAt(0), timeAt(w), QColor(90, 160, 120, 110));

    p.setRenderHint(QPainter::Antialiasing);
    std::map<NodeId, anim::Curve> shownCurves;
    for (const Channel& ch : channels)
        if (ch.shown && curves.contains(ch.curve))
            shownCurves[ch.curve] = preview(ch.curve);

    if (settings.showBuffer)
        for (const Channel& ch : channels)
        {
            auto b = buffers.find(realCurve(ch.curve));
            if (!ch.shown || b == buffers.end())
                continue;
            const int slot = slotOf(ch.curve);
            anim::Curve keys;
            std::function<double(double)> eval;
            if (b->second.rotation && slot >= 0 && slot < 3)
            {
                anim::RotationCurve rc = b->second.rotationCurve.looped();
                keys = derivedChannel(rc, slot);
                eval = [rc, slot](double t) { return rc.evaluate(t)[slot]; };
            }
            else if (!b->second.rotation && slot < 0)
            {
                keys = b->second.curve;
                eval = [c = b->second.curve.looped()](double t) { return c.evaluate(t); };
            }
            else
                continue;
            const QColor ghost = gray(95);
            drawCurve(p, keys, ch, false, eval, &ghost);
        }

    std::vector<NodeId> active = activeSet();
    auto isActive = [&](NodeId c) { return std::find(active.begin(), active.end(), c) != active.end(); };
    // Active curves draw last so they sit on top.
    for (int pass = 0; pass < 2; ++pass)
        for (const Channel& ch : channels)
        {
            if (!ch.shown || !shownCurves.contains(ch.curve) || isActive(ch.curve) != (pass == 1))
                continue;
            drawCurve(p, shownCurves[ch.curve], ch, pass == 1, sampler(ch.curve, shownCurves[ch.curve]));
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
            QPointF pt(xOf(k.time), yOfKey(ch, k.value));
            bool selected = isSelected(ch.curve, keyTimeBeforeDrag(ch.curve, k.time));
            if (selected)
            {
                const QPointF in = handlePoint(c, i, false, ch), out = handlePoint(c, i, true, ch);
                p.setPen(QPen(gray(150), 1));
                p.drawLine(in, pt);
                p.drawLine(pt, out);
                p.setBrush(gray(170));
                // Broken handles are squares, unified ones round.
                for (QPointF h : {in, out})
                    if (k.broken)
                        p.drawRect(QRectF(h.x() - 2.5, h.y() - 2.5, 5, 5));
                    else
                        p.drawEllipse(h, 3, 3);
            }
            const double fw = selected ? 0.0 : falloffWeightAt(ch.curve, k.time);
            p.setPen(QPen(selected ? gray(255) : channelColor(ch, isActive(ch.curve) ? 230 : ch.level), 1));
            p.setBrush(selected ? gray(255) : fw > 0.0 ? falloffTint(fw) : gray(22));
            if (k.breakdown)
                p.drawPolygon(QPolygonF({pt + QPointF(0, -4), pt + QPointF(4, 0), pt + QPointF(0, 4), pt + QPointF(-4, 0)}));
            else
                p.drawRect(QRectF(pt.x() - 3, pt.y() - 3, 6, 6));
            // A loop key is ringed: it mirrors the first key.
            if (i + 1 == c.keys.size() && i > 0 && loopCurves.contains(ch.curve))
            {
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(pt, 6.5, 6.5);
            }
        }
    }

    p.setRenderHint(QPainter::Antialiasing, false);
    if (QRectF r = regionRect(); !r.isNull() && mDrag != Drag::Box && !transforming())
    {
        const QRectF o = r.adjusted(-8, -8, 8, 8);
        p.setPen(QPen(gray(150), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(o);
        p.setPen(QPen(gray(200), 1));
        p.setBrush(gray(40));
        const auto pts = regionHandles(o);
        for (int i = 0; i < 8; ++i)
            if (regionHandleUsable(i, r))
                p.drawRect(QRectF(pts[size_t(i)].x() - 3, pts[size_t(i)].y() - 3, 6, 6));
    }

    if (mDrag == Drag::Box)
    {
        p.setPen(QPen(gray(210), 1, Qt::DashLine));
        p.setBrush(gray(255, 20));
        p.drawRect(QRectF(mPress, mLast).normalized());
    }
    // The falloff radius while B is held: a band either side of the cursor.
    if (mRadiusKey)
    {
        const double cx = mRadiusDrag ? mRadiusPress.x() : mCursor.x();
        const double r = settings.falloffFrames / cvFps() * mSx;
        const QColor orange(255, 160, 60);
        p.fillRect(QRectF(cx - r, kRulerHeight, 2 * r, bottom - kRulerHeight), QColor(255, 160, 60, 22));
        p.setPen(QPen(orange, 1, Qt::DashLine));
        p.drawLine(QPointF(cx - r, kRulerHeight), QPointF(cx - r, bottom));
        p.drawLine(QPointF(cx + r, kRulerHeight), QPointF(cx + r, bottom));
        p.setPen(orange);
        p.drawText(QPointF(cx + r + 4, std::max(kRulerHeight + 14.0, mCursor.y())), formatValue(settings.falloffFrames) + " f");
    }
    p.restore();

    drawStrips(p, shownCurves);

    // Current time, over the plot and the strips.
    double tx = xOf(Graph::current().time());
    p.setPen(QPen(gray(190), 1));
    p.drawLine(QPointF(tx, 0), QPointF(tx, height()));
    QString frameLabel = QString::number(std::lround(anim::frameOf(Graph::current().time())));
    QRectF lr = QRectF(p.fontMetrics().boundingRect(frameLabel).adjusted(-4, -1, 4, 1));
    lr.moveTopLeft(QPointF(tx + 2, 1));
    p.fillRect(lr, gray(200));
    p.setPen(gray(20));
    p.drawText(lr, Qt::AlignCenter, frameLabel);
}

void CurveView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (!mFramed && !channels.empty())
    {
        mFramed = true;
        frame(false);
    }
}

void CurveView::mousePressEvent(QMouseEvent* e)
{
    mPress = mLast = e->position();
    mMoved = false;
    mPressCurve = scene::kNoNode;
    // A click during a hold-key drag places the keys there.
    if (mHold.active)
    {
        const bool dragging = mHold.dragging;
        endHold();
        if (dragging)
        {
            mouseReleaseEvent(e);
            return;
        }
    }
    if (mRadiusKey && (e->button() == Qt::LeftButton || e->button() == Qt::MiddleButton))
    {
        mRadiusDrag = mRadiusUsed = true;
        mRadiusPress = e->position();
        mRadiusStart = e->button() == Qt::MiddleButton ? 0.0 : settings.falloffFrames;
        settings.falloff = true;
        mEditor->syncToolbar();
        update();
        return;
    }
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
        mZoomAnchorV = displayAt(mPress.y());
        mZoomSx = mSx;
        mZoomSy = mSy;
        return;
    }
    if (e->button() == Qt::RightButton)
    {
        showMenu(e->globalPosition().toPoint());
        return;
    }
    const Drag toolDrag = settings.tool == Tool::Scale ? Drag::Scale : settings.tool == Tool::Rotate ? Drag::TangentRotate : Drag::Move;
    if (e->button() == Qt::MiddleButton)
    {
        if (mPress.y() < kRulerHeight || mPress.y() > plotBottom())
        {
            mDrag = Drag::Time;
            mHoldPose = true;
            scrubTo(mPress.x());
        }
        else if (!sel.empty())
            beginTransform(toolDrag);
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;

    if (mPress.y() < kRulerHeight || mPress.y() > plotBottom())
    {
        mDrag = Drag::Time;
        mHoldPose = false;
        scrubTo(mPress.x());
        return;
    }
    if (hitHandle(mPress))
    {
        mDrag = Drag::Tangent;
        mTangentAngle = mTangentWeight = std::nan("");
        return;
    }
    if (int h = hitRegionHandle(mPress); h >= 0)
    {
        mRegionHandle = h;
        mRegionStart = regionRect();
        beginTransform(Drag::Region);
        return;
    }
    if (auto hit = hitKey(mPress))
    {
        bool was = isSelected(hit->curve, hit->time);
        auto drop = [&] { std::erase_if(sel, [&](const KeySel& k) { return k.curve == hit->curve && std::abs(k.time - hit->time) <= kEps; }); };
        if (ctrl)
            drop();
        else if (shift)
        {
            if (was)
                drop();
            else
                sel.push_back(*hit);
        }
        else if (!was)
            sel = {*hit};
        activeCurve = hit->curve;
        selectionChanged();
        if (!ctrl && isSelected(hit->curve, hit->time))
            beginTransform(toolDrag);
        return;
    }
    if (QRectF r = regionRect(); !r.isNull() && r.adjusted(-8, -8, 8, 8).contains(mPress) && !shift && !ctrl)
    {
        beginTransform(Drag::Move);
        return;
    }
    // A click on a curve picks its keys; a drag from it still boxes.
    mPressCurve = hitCurve(mPress);
    mDrag = Drag::Box;
    mBoxMode = shift ? 1 : ctrl ? -1 : 0;
}

void CurveView::mouseMoveEvent(QMouseEvent* e)
{
    QPointF pos = e->position();
    mCursor = pos;
    if (mRadiusDrag)
    {
        const double framesPerPixel = cvFps() / mSx;
        settings.falloffFrames = std::clamp(mRadiusStart + (pos.x() - mRadiusPress.x()) * framesPerPixel, 0.5, 1000.0);
        mEditor->syncToolbar();
        update();
        return;
    }
    if (mRadiusKey)
    {
        update();
        return;
    }
    // A held W, E or R starts dragging once the key has been down a moment and the mouse has travelled, so a tap
    // that brushes the mouse never moves keys.
    if (mHold.active && !mHold.dragging && e->buttons() == Qt::NoButton)
    {
        constexpr qint64 kHoldMs = 150;
        constexpr double kTravel = 6.0;
        if (QDateTime::currentMSecsSinceEpoch() - mHold.since < kHoldMs)
            mHold.anchor = pos;
        else if ((pos - mHold.anchor).manhattanLength() >= kTravel && rect().contains(pos.toPoint()))
        {
            mHold.dragging = true;
            mPress = mLast = pos;
            mMoved = false;
            beginTransform(mHold.kind);
        }
        return;
    }
    QPointF d = pos - mLast;
    if ((pos - mPress).manhattanLength() > 3)
        mMoved = true;
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    QPointF total = pos - mPress;
    // Shift locks to the axis the drag mostly follows.
    const bool lockValue = shift && std::abs(total.x()) > std::abs(total.y());
    const bool lockTime = shift && !lockValue;
    switch (mDrag)
    {
    case Drag::Pan:
        mT0 -= d.x() / mSx;
        mV0 += d.y() / mSy;
        break;
    case Drag::Zoom:
        mSx = std::clamp(mZoomSx * std::exp(total.x() * 0.01), 1e-3, 1e7);
        mSy = std::clamp(mZoomSy * std::exp(-total.y() * 0.01), 1e-6, 1e9);
        mT0 = mZoomAnchorT - mPress.x() / mSx;
        mV0 = mZoomAnchorV - (plotBottom() - mPress.y()) / mSy;
        break;
    case Drag::Time: scrubTo(pos.x()); break;
    case Drag::Move:
    {
        if (!mMoved)
            break;
        double fps = cvFps();
        mXform.timeOffset = lockTime ? 0.0 : std::round((timeAt(pos.x()) - timeAt(mPress.x())) * fps) / fps;
        mDisplayOffset = lockValue ? 0.0 : displayAt(pos.y()) - displayAt(mPress.y());
        break;
    }
    case Drag::Scale:
        if (!mMoved)
            break;
        mXform.timeScale = lockTime ? 1.0 : std::exp(total.x() / 200.0);
        mXform.valueScale = lockValue ? 1.0 : std::exp(-total.y() / 200.0);
        break;
    case Drag::Region:
    {
        if (!mMoved)
            break;
        const QRectF r = mRegionStart;
        const int h = mRegionHandle;
        const bool left = h == 0 || h == 3 || h == 7, right = h == 1 || h == 2 || h == 5;
        const bool top = h == 0 || h == 1 || h == 4, bot = h == 2 || h == 3 || h == 6;
        if ((left || right) && r.width() >= 1.0)
            mXform.timeScale = right ? (r.width() + total.x()) / r.width() : (r.width() - total.x()) / r.width();
        if ((top || bot) && r.height() >= 1.0)
            mXform.valueScale = top ? (r.height() - total.y()) / r.height() : (r.height() + total.y()) / r.height();
        break;
    }
    case Drag::Tangent:
    {
        double weight = std::nan("");
        mTangentAngle = angleFor(pos, &weight);
        mTangentWeight = weight;
        break;
    }
    case Drag::TangentRotate: mRotateDelta = -total.x() * 0.5; break;
    default: break;
    }
    mLast = pos;
    if (mDrag == Drag::Move || mDrag == Drag::Scale || mDrag == Drag::Region || mDrag == Drag::Tangent || mDrag == Drag::TangentRotate)
        pushPreviews();
    update();
}

void CurveView::mouseReleaseEvent(QMouseEvent*)
{
    if (mRadiusDrag)
    {
        mRadiusDrag = false;
        mEditor->saveSettings();
        update();
        return;
    }
    dropPreviews();
    Drag drag = mDrag;
    switch (drag)
    {
    case Drag::Box:
        mDrag = Drag::None;
        finishBox();
        break;
    case Drag::Move:
    case Drag::Scale:
    case Drag::Region: commitTransform(); break;
    case Drag::Tangent: commitTangent(); break;
    case Drag::TangentRotate: commitTangentRotate(); break;
    default: break;
    }
    mDrag = Drag::None;
    update();
}

void CurveView::wheelEvent(QWheelEvent* e)
{
    double f = std::pow(1.0015, e->angleDelta().y());
    QPointF pos = e->position();
    double t = timeAt(pos.x()), v = displayAt(pos.y());
    mSx = std::clamp(mSx * f, 1e-3, 1e7);
    mSy = std::clamp(mSy * f, 1e-6, 1e9);
    mT0 = t - pos.x() / mSx;
    mV0 = v - (plotBottom() - pos.y()) / mSy;
    update();
}

std::optional<KeySel> CurveView::hitKey(QPointF pos) const
{
    std::optional<KeySel> best;
    double bestD = 7.0;
    for (const Channel& ch : channels)
    {
        if (!ch.shown || !curves.contains(ch.curve))
            continue;
        for (const anim::Key& k : curves.at(ch.curve).keys)
        {
            double d = std::hypot(xOf(k.time) - pos.x(), yOfKey(ch, k.value) - pos.y());
            if (d < bestD)
            {
                bestD = d;
                best = KeySel{ch.curve, k.time};
            }
        }
    }
    return best;
}

bool CurveView::hitHandle(QPointF pos)
{
    for (const KeySel& s : sel)
    {
        auto it = curves.find(s.curve);
        const Channel* ch = channelOf(s.curve);
        if (it == curves.end() || !ch)
            continue;
        const anim::Curve& c = it->second;
        int i = c.find(s.time);
        if (i < 0)
            continue;
        for (int side : {-1, 1})
        {
            QPointF h = handlePoint(c, size_t(i), side > 0, *ch);
            if (std::hypot(h.x() - pos.x(), h.y() - pos.y()) < 6.0)
            {
                mTangent = s;
                mTangentSide = side;
                return true;
            }
        }
    }
    return false;
}

NodeId CurveView::hitCurve(QPointF pos) const
{
    double t = timeAt(pos.x());
    for (const Channel& ch : channels)
        if (ch.shown && curves.contains(ch.curve) && std::abs(yOfKey(ch, sampler(ch.curve, curves.at(ch.curve))(t)) - pos.y()) < 5.0)
            return ch.curve;
    return scene::kNoNode;
}

double CurveView::angleFor(QPointF pos, double* weight) const
{
    const anim::Curve& c = curves.at(mTangent.curve);
    const Channel* ch = channelOf(mTangent.curve);
    int i = c.find(mTangent.time);
    if (i < 0 || !ch)
        return 0.0;
    const anim::Key& k = c.keys[size_t(i)];
    QPointF pt(xOf(mTangent.time), yOfKey(*ch, k.value));
    double dx = pos.x() - pt.x(), dy = pos.y() - pt.y();
    // The in handle sits left of the key; dragging it past the key flips nothing.
    if (mTangentSide > 0)
        dx = std::max(dx, 1.0);
    else
        dx = std::min(dx, -1.0);
    const bool neighbour = mTangentSide > 0 ? size_t(i) + 1 < c.keys.size() : i > 0;
    if (weight && c.weighted && neighbour)
    {
        const double dt = mTangentSide > 0 ? c.keys[size_t(i) + 1].time - k.time : k.time - c.keys[size_t(i) - 1].time;
        *weight = std::clamp(std::abs(dx) / mSx / (dt / 3.0), 0.05, 3.0);
    }
    double slope = (-dy / (mSy * ch->mapScale)) / (dx / mSx);
    return std::clamp(cvDegrees(slope), -89.9, 89.9);
}

void CurveView::scrubTo(double x)
{
    double t = anim::snapToFrame(timeAt(x));
    if (std::abs(t - Graph::current().time()) < kEps)
        return;
    ops::execute("currentTime", {{"args", {t}}, {"update", !mHoldPose}});
    notifyTimeChanged();
}

void CurveView::setPivots(QPointF press)
{
    mValuePivots.clear();
    double tMin = 1e300, tMax = -1e300;
    for (const KeySel& k : sel)
    {
        tMin = std::min(tMin, k.time);
        tMax = std::max(tMax, k.time);
    }
    const double now = Graph::current().time();
    switch (settings.pivot)
    {
    case Pivot::Selection: mXform.timePivot = 0.5 * (tMin + tMax); break;
    case Pivot::Click: mXform.timePivot = timeAt(press.x()); break;
    case Pivot::CurrentFrame: mXform.timePivot = now; break;
    case Pivot::FirstKey: mXform.timePivot = tMin; break;
    case Pivot::LastKey: mXform.timePivot = tMax; break;
    }
    std::map<NodeId, std::vector<const KeySel*>> byCurve;
    for (const KeySel& k : sel)
        byCurve[k.curve].push_back(&k);
    for (auto& [id, keys] : byCurve)
    {
        const Channel* ch = channelOf(id);
        auto it = curves.find(id);
        if (!ch || it == curves.end())
            continue;
        const anim::Curve& c = it->second;
        double lo = 1e300, hi = -1e300, first = 0.0, last = 0.0, tf = 1e300, tl = -1e300;
        for (const KeySel* k : keys)
            if (int i = c.find(k->time); i >= 0)
            {
                const double v = c.keys[size_t(i)].value;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                if (k->time < tf)
                {
                    tf = k->time;
                    first = v;
                }
                if (k->time > tl)
                {
                    tl = k->time;
                    last = v;
                }
            }
        double pivot = 0.5 * (lo + hi);
        switch (settings.pivot)
        {
        case Pivot::Selection: break;
        case Pivot::Click: pivot = ch->value(displayAt(press.y())); break;
        case Pivot::CurrentFrame: pivot = sampler(id, c)(now); break;
        case Pivot::FirstKey: pivot = first; break;
        case Pivot::LastKey: pivot = last; break;
        }
        mValuePivots[id] = pivot;
    }
}

void CurveView::beginTransform(Drag kind)
{
    mDrag = kind;
    mXform = {};
    mDisplayOffset = 0.0;
    mRotateDelta = 0.0;
    if (kind == Drag::Scale)
        setPivots(mPress);
    if (kind == Drag::Region)
    {
        const QRectF r = mRegionStart;
        const int h = mRegionHandle;
        const bool left = h == 0 || h == 3 || h == 7, top = h == 0 || h == 1 || h == 4;
        mXform.timePivot = timeAt(left ? r.right() : r.left());
        const double pivotDisplay = displayAt(top ? r.bottom() : r.top());
        mValuePivots.clear();
        for (const KeySel& k : sel)
            if (const Channel* ch = channelOf(k.curve))
                mValuePivots[k.curve] = ch->value(pivotDisplay);
    }
}

void CurveView::finishBox()
{
    if (!mMoved && mPressCurve && channelOf(mPressCurve))
    {
        const NodeId c = mPressCurve;
        activeCurve = c;
        if (mBoxMode == 0)
            sel.clear();
        if (auto it = curves.find(c); it != curves.end())
            for (const anim::Key& k : it->second.keys)
            {
                const bool was = isSelected(c, k.time);
                if (mBoxMode < 0)
                    std::erase_if(sel, [&](const KeySel& s) { return s.curve == c && std::abs(s.time - k.time) <= kEps; });
                else if (!was)
                    sel.push_back({c, k.time});
            }
        selectionChanged();
        return;
    }
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
            if (!box.contains(QPointF(xOf(k.time), yOfKey(ch, k.value))))
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

void CurveView::commitTransform()
{
    const bool moved = mMoved;
    const anim::KeyTransform base = mXform;
    const double displayOffset = mDisplayOffset;
    // Snapshot the per-channel transforms before the drag state resets.
    std::map<NodeId, anim::KeyTransform> perCurve;
    for (const KeySel& k : sel)
        perCurve.try_emplace(k.curve, transformFor(k.curve));
    mDrag = Drag::None;
    if (!moved)
        return;
    const bool any = base.timeOffset != 0.0 || std::abs(base.timeScale - 1.0) > 1e-12 || std::abs(base.valueScale - 1.0) > 1e-12 ||
                     displayOffset != 0.0;
    if (!any)
        return;
    std::map<NodeId, std::vector<double>> byCurve;
    for (const KeySel& k : sel)
        byCurve[k.curve].push_back(k.time);
    json keys = json::array();
    for (auto& [id, times] : byCurve)
    {
        std::string target = commandTarget(id);
        if (target.empty())
            continue;
        const anim::KeyTransform& x = perCurve[id];
        json entry = {{"curve", target}, {"times", times}};
        if (x.valueOffset != 0.0)
            entry["valueOffset"] = x.valueOffset;
        if (std::abs(x.valueScale - 1.0) > 1e-12)
            entry["valuePivot"] = x.valuePivot;
        keys.push_back(std::move(entry));
    }
    if (keys.empty())
        return;
    json call = {{"keys", keys}};
    if (base.timeOffset != 0.0)
        call["timeOffset"] = base.timeOffset;
    if (std::abs(base.timeScale - 1.0) > 1e-12)
    {
        call["timeScale"] = base.timeScale;
        call["timePivot"] = base.timePivot;
        call["snap"] = true;
    }
    if (std::abs(base.valueScale - 1.0) > 1e-12)
        call["valueScale"] = base.valueScale;
    if (settings.falloff)
        call["falloff"] = settings.falloffFrames / cvFps();
    try
    {
        ops::execute("transformKeys", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return;
    }
    for (KeySel& k : sel)
        k.time = perCurve[k.curve].time(k.time, cvFps());
    selectionChanged();
}

void CurveView::commitTangent()
{
    if (!mMoved || (std::isnan(mTangentAngle) && std::isnan(mTangentWeight)))
        return;
    std::string name = commandTarget(mTangent.curve);
    auto it = curves.find(mTangent.curve);
    if (name.empty() || it == curves.end())
        return;
    int i = it->second.find(mTangent.time);
    if (i < 0)
        return;
    const anim::Key& k = it->second.keys[size_t(i)];
    const bool out = mTangentSide > 0;
    json call = {{"args", {name}}, {"time", mTangent.time}};
    if (!std::isnan(mTangentAngle))
    {
        if (k.broken)
            call[out ? "outAngle" : "inAngle"] = mTangentAngle;
        else
            call["inAngle"] = call["outAngle"] = mTangentAngle;
    }
    if (!std::isnan(mTangentWeight))
        call[out ? "outWeight" : "inWeight"] = mTangentWeight;
    try
    {
        ops::execute("keyTangent", call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    mTangentAngle = mTangentWeight = std::nan("");
}

void CurveView::commitTangentRotate()
{
    if (!mMoved || std::abs(mRotateDelta) < 1e-6)
        return;
    int count = 0;
    {
        UndoChunk chunk("Rotate Tangents");
        for (const KeySel& k : sel)
        {
            auto it = curves.find(k.curve);
            const Channel* ch = channelOf(k.curve);
            std::string name = commandTarget(k.curve);
            if (it == curves.end() || !ch || name.empty())
                continue;
            int i = it->second.find(k.time);
            if (i < 0)
                continue;
            const double in = it->second.inSlope(size_t(i)), out = it->second.outSlope(size_t(i));
            const double inAngle = cvDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, in) + mRotateDelta));
            const double outAngle = cvDegrees(slopeOfScreenAngle(*ch, screenAngleOf(*ch, out) + mRotateDelta));
            count += runUnechoed("keyTangent", {{"args", {name}}, {"time", k.time}, {"inAngle", inAngle}, {"outAngle", outAngle}}) ? 1 : 0;
        }
    }
    log::write(LogLevel::Echo, "# rotated " + std::to_string(count) + " tangents\n");
    mRotateDelta = 0.0;
}

void CurveView::selectionChanged()
{
    mEditor->refreshKeyFields();
    update();
}

void CurveView::showMenu(QPoint at)
{
    QMenu menu(this);
    mEditor->fillContextMenu(&menu);
    menu.exec(at);
}
void CurveView::pushPreviews()
{
    Graph& g = Graph::current();
    std::set<NodeId> rotationsDone;
    for (const KeySel& k : sel)
    {
        const NodeId real = realCurve(k.curve);
        if (!g.node(real))
            continue;
        if (slotOf(k.curve) < 0)
        {
            if (curves.contains(k.curve))
                anim::setPreview(g, real, preview(k.curve));
            continue;
        }
        // Tangent drags on a rotation channel edit only the drawn Euler channel; the scene follows on release.
        if (mDrag != Drag::Tangent && mDrag != Drag::TangentRotate && rotations.contains(real) && rotationsDone.insert(real).second)
            anim::setPreview(g, real, previewRotation(real));
    }
}

void CurveView::dropPreviews()
{
    anim::clearPreviews(Graph::current());
}

void CurveView::toolKeyDown(Tool tool, bool holdToDrag)
{
    settings.tool = tool;
    // Key repeat sends more presses while the key is down.
    if (mHold.active || mDrag != Drag::None || !holdToDrag || sel.empty())
        return;
    mHold = {};
    mHold.active = true;
    mHold.kind = tool == Tool::Scale ? Drag::Scale : tool == Tool::Rotate ? Drag::TangentRotate : Drag::Move;
    mHold.anchor = mapFromGlobal(QCursor::pos());
    mHold.since = QDateTime::currentMSecsSinceEpoch();
    setMouseTracking(true);
}

void CurveView::toolKeyUp()
{
    if (mHold.dragging)
    {
        QMouseEvent release(QEvent::MouseButtonRelease, mLast, mapToGlobal(mLast), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        mouseReleaseEvent(&release);
    }
    endHold();
}

void CurveView::endHold()
{
    mHold = {};
    if (!mRadiusKey)
        setMouseTracking(false);
    update();
}

void CurveView::falloffKeyDown()
{
    if (mRadiusKey)
        return;
    mRadiusKey = true;
    mRadiusUsed = false;
    mCursor = mapFromGlobal(QCursor::pos());
    setMouseTracking(true);
    update();
}

bool CurveView::falloffKeyUp()
{
    mRadiusKey = false;
    mRadiusDrag = false;
    if (!mHold.active)
        setMouseTracking(false);
    update();
    return mRadiusUsed;
}

} // namespace lila::ui::graphed
