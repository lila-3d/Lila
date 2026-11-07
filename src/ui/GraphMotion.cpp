#include "ui/GraphMotion.h"

#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/Transform.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/GraphEditorCommon.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace lila::ui::graphed
{
namespace
{
constexpr size_t kMaxSamples = 4000;

glm::dquat rotationOf(const glm::dmat4& m)
{
    glm::dmat3 r(m);
    for (int i = 0; i < 3; ++i)
    {
        const double len = glm::length(r[i]);
        if (len > 1e-12)
            r[i] /= len;
    }
    return glm::normalize(glm::quat_cast(r));
}

/** Rotation from a to b as a world-space rotation vector, degrees. */
glm::dvec3 turnBetween(const glm::dquat& a, glm::dquat b)
{
    if (glm::dot(a, b) < 0.0)
        b = -b;
    glm::dquat d = b * glm::inverse(a);
    const double s = glm::length(glm::dvec3(d.x, d.y, d.z));
    if (s < 1e-12)
        return glm::dvec3(0.0);
    const double angle = 2.0 * std::atan2(s, d.w);
    return glm::dvec3(d.x, d.y, d.z) / s * glm::degrees(angle);
}

/**
 * Samples standing far above their neighbourhood: four times the median of the six samples either side, and a fifth of
 * the strip's peak. Smooth peaks have neighbours of a similar size, so only pops and broken tangents qualify.
 */
std::vector<double> spikeTimes(const std::vector<MotionSeries>& series)
{
    double peak = 0.0;
    for (const MotionSeries& s : series)
        for (auto [t, v] : s.samples)
            if (std::isfinite(v))
                peak = std::max(peak, std::abs(v));
    std::vector<double> out;
    constexpr size_t kWindow = 6;
    for (const MotionSeries& s : series)
        for (size_t i = 0; i < s.samples.size(); ++i)
        {
            const double v = std::abs(s.samples[i].second);
            if (!std::isfinite(v) || v < 0.2 * peak)
                continue;
            std::vector<double> around;
            for (size_t j = i > kWindow ? i - kWindow : 0; j < std::min(s.samples.size(), i + kWindow + 1); ++j)
                if (j != i && std::isfinite(s.samples[j].second))
                    around.push_back(std::abs(s.samples[j].second));
            if (around.size() < 4)
                continue;
            std::nth_element(around.begin(), around.begin() + std::ptrdiff_t(around.size() / 2), around.end());
            if (v > 4.0 * around[around.size() / 2])
                out.push_back(s.samples[i].first);
        }
    return out;
}

/** A robust range: the 99th percentile magnitude, so one step key doesn't flatten everything else. */
double rangeOf(const std::vector<MotionSeries>& series, double target)
{
    std::vector<double> mags;
    for (const MotionSeries& s : series)
        for (auto [t, v] : s.samples)
            if (std::isfinite(v))
                mags.push_back(std::abs(v));
    double r = 0.0;
    if (!mags.empty())
    {
        const size_t k = std::min(mags.size() - 1, size_t(double(mags.size()) * 0.99));
        std::nth_element(mags.begin(), mags.begin() + std::ptrdiff_t(k), mags.end());
        r = mags[k];
    }
    if (std::isfinite(target))
        r = std::max(r, std::abs(target));
    return r > 1e-9 ? r * 1.15 : 1.0;
}
} // namespace

double rateOf(const std::function<double(double)>& f, double t, double h)
{
    return (f(t + h) - f(t - h)) / (2.0 * h);
}

void WorldMotion::update(scene::Graph& g, const std::vector<uint64_t>& nodes, double t0, double t1, double fps)
{
    if (mValid && nodes == mNodes && std::abs(t0 - mT0) < 1e-9 && std::abs(t1 - mT1) < 1e-9 && fps == mFps)
        return;
    LILA_ZONE();
    mNodes = nodes;
    mT0 = t0;
    mT1 = t1;
    mFps = fps;
    mValid = true;
    mTracks.clear();
    std::vector<scene::NodeId> ids;
    for (uint64_t id : nodes)
        if (scene::Node* n = g.node(id); n && n->type().isA("transform"))
        {
            ids.push_back(id);
            mTracks.push_back({QString::fromStdString(n->name()), {}, {}, {}});
        }
    if (ids.empty() || t1 <= t0 || fps <= 0.0)
        return;
    double step = 1.0 / fps;
    while ((t1 - t0) / step > double(kMaxSamples))
        step *= 2.0;
    const double was = g.time();
    for (double t = std::floor(t0 / step) * step - step; t <= t1 + step * 1.5; t += step)
    {
        g.setTime(t, false);
        g.evaluateUpstream(ids);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            const glm::dmat4& m = g.node(ids[i])->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            mTracks[i].times.push_back(t);
            mTracks[i].positions.push_back(glm::dvec3(m[3]));
            mTracks[i].rotations.push_back(rotationOf(m));
        }
    }
    g.setTime(was, false);
    g.evaluateUpstream(ids);
}

std::vector<MotionSeries> WorldMotion::speed(bool angular) const
{
    std::vector<MotionSeries> out;
    int index = 0;
    for (const Track& tr : mTracks)
    {
        MotionSeries s{tr.label, gray(index++ % 2 ? 150 : 210), {}};
        for (size_t i = 1; i + 1 < tr.times.size(); ++i)
        {
            const double dt = tr.times[i + 1] - tr.times[i - 1];
            const double v = angular ? glm::length(turnBetween(tr.rotations[i - 1], tr.rotations[i + 1])) / dt
                                     : glm::length(tr.positions[i + 1] - tr.positions[i - 1]) / dt;
            s.samples.push_back({tr.times[i], v});
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<MotionSeries> WorldMotion::acceleration(bool angular) const
{
    std::vector<MotionSeries> out;
    int index = 0;
    for (const Track& tr : mTracks)
    {
        MotionSeries s{tr.label, gray(index++ % 2 ? 150 : 210), {}};
        for (size_t i = 1; i + 1 < tr.times.size(); ++i)
        {
            const double h = 0.5 * (tr.times[i + 1] - tr.times[i - 1]);
            double a;
            if (angular)
                a = glm::length(turnBetween(tr.rotations[i], tr.rotations[i + 1]) - turnBetween(tr.rotations[i - 1], tr.rotations[i])) / (h * h);
            else
                a = glm::length(tr.positions[i + 1] - 2.0 * tr.positions[i] + tr.positions[i - 1]) / (h * h);
            s.samples.push_back({tr.times[i], a});
        }
        out.push_back(std::move(s));
    }
    return out;
}

void drawStrip(QPainter& p, const QRectF& rect, const std::vector<MotionSeries>& series, const StripStyle& style,
               const std::function<double(double)>& xOf, double currentTime)
{
    p.save();
    p.setClipRect(rect);
    p.fillRect(rect, gray(24));
    p.setPen(gray(48));
    p.drawLine(rect.topLeft(), rect.topRight());

    const double range = rangeOf(series, style.target);
    const double top = rect.top() + 14.0, bottom = rect.bottom() - 4.0;
    const double lo = style.magnitude ? 0.0 : -range, hi = range;
    auto yOf = [&](double v) { return bottom - (std::clamp(v, lo, hi) - lo) / (hi - lo) * (bottom - top); };

    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(gray(40));
    p.drawLine(QPointF(rect.left(), yOf(0.0)), QPointF(rect.right(), yOf(0.0)));
    if (std::isfinite(style.target))
    {
        p.setPen(QPen(QColor(120, 190, 140), 1, Qt::DashLine));
        p.drawLine(QPointF(rect.left(), yOf(style.target)), QPointF(rect.right(), yOf(style.target)));
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    for (const MotionSeries& s : series)
    {
        QPainterPath path;
        for (auto [t, v] : s.samples)
        {
            if (!std::isfinite(v))
                continue;
            QPointF pt(xOf(t), yOf(v));
            if (pt.x() < rect.left() - 4 || pt.x() > rect.right() + 4)
            {
                if (path.elementCount() && pt.x() > rect.right())
                {
                    path.lineTo(pt);
                    break;
                }
                if (pt.x() < rect.left())
                {
                    path = QPainterPath();
                    path.moveTo(pt);
                }
                continue;
            }
            if (path.elementCount() == 0)
                path.moveTo(pt);
            else
                path.lineTo(pt);
        }
        p.setPen(QPen(s.color, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    if (style.flagSpikes)
    {
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QPen(QColor(232, 128, 64), 2));
        for (double t : spikeTimes(series))
        {
            const double x = xOf(t);
            if (x >= rect.left() && x <= rect.right())
                p.drawLine(QPointF(x, rect.top() + 1), QPointF(x, rect.top() + 6));
        }
    }

    // Title and scale at the left, readouts of every series at the current time beside the time line.
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    p.setFont(f);
    p.setPen(gray(140));
    QString head = style.title + "  " + formatValue(hi) + (style.unit.isEmpty() ? QString() : " " + style.unit);
    p.drawText(QPointF(rect.left() + 4, rect.top() + 11), head);
    const double tx = xOf(currentTime);
    if (tx >= rect.left() && tx <= rect.right())
    {
        // Under the title when the time line runs through it.
        const bool clash = tx < rect.left() + 8 + p.fontMetrics().horizontalAdvance(head);
        double y = rect.top() + (clash ? 22 : 11);
        for (const MotionSeries& s : series)
        {
            auto it = std::min_element(s.samples.begin(), s.samples.end(), [&](const auto& a, const auto& b) {
                return std::abs(a.first - currentTime) < std::abs(b.first - currentTime);
            });
            if (it == s.samples.end() || !std::isfinite(it->second))
                continue;
            p.setPen(s.color);
            QString text = (series.size() > 1 ? s.label + " " : QString()) + formatValue(it->second);
            p.drawText(QPointF(tx + 5, y), text);
            y += 11;
            if (y > rect.bottom())
                break;
        }
    }
    p.restore();
}
} // namespace lila::ui::graphed
