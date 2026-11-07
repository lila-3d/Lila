#pragma once

#include <QColor>
#include <QRectF>
#include <QString>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

class QPainter;

namespace lila::scene
{
class Graph;
}

namespace lila::ui::graphed
{
/** One line on a motion strip: samples of (seconds, value). */
struct MotionSeries
{
    QString label;
    QColor color;
    std::vector<std::pair<double, double>> samples;
};

/**
 * Speed and acceleration of transforms in world space, sampled a frame apart by evaluating only what the transforms
 * depend on at each time, then restoring the current time. Parent motion is included.
 */
class WorldMotion
{
public:
    /** Samples again when the nodes, range or frame rate differ from the last call, or after invalidate(). */
    void update(scene::Graph& g, const std::vector<uint64_t>& nodes, double t0, double t1, double fps);
    void invalidate() { mValid = false; }

    /** cm/s, or degrees/s when angular. */
    std::vector<MotionSeries> speed(bool angular) const;
    /** cm/s², or degrees/s² when angular. */
    std::vector<MotionSeries> acceleration(bool angular) const;

private:
    struct Track
    {
        QString label;
        std::vector<double> times;
        std::vector<glm::dvec3> positions;
        std::vector<glm::dquat> rotations;
    };
    std::vector<Track> mTracks;
    std::vector<uint64_t> mNodes;
    double mT0 = 0.0, mT1 = 0.0, mFps = 0.0;
    bool mValid = false;
};

/** The rate of change of f by central differences, h seconds apart. */
double rateOf(const std::function<double(double)>& f, double t, double h);

struct StripStyle
{
    QString title;
    QString unit;
    /** A dashed reference line (target speed); NaN for none. */
    double target = std::numeric_limits<double>::quiet_NaN();
    /** Mark samples far above the strip's typical magnitude. */
    bool flagSpikes = false;
    /** Values are magnitudes: the strip runs 0..max instead of -max..max. */
    bool magnitude = false;
};

/** Draws a strip of series across rect, sharing the curve view's time axis (xOf). Readouts at currentTime. */
void drawStrip(QPainter& p, const QRectF& rect, const std::vector<MotionSeries>& series, const StripStyle& style,
               const std::function<double(double)>& xOf, double currentTime);
} // namespace lila::ui::graphed
