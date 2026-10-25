#include "ui/UvProjectTool.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Graph.h"
#include "scene/LastOp.h"
#include "ui/GizmoMath.h"
#include "ui/Tools.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "uv/UvCommands.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QTimer>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <optional>

namespace lila::ui
{
namespace
{
namespace lo = scene::lastOp;
using json = nlohmann::json;
using uv::Projection;

constexpr const char* kCommand = "uvProject";
constexpr const char* kToolName = "uvProject";
const glm::vec4 kGray(0.70f, 0.70f, 0.70f, 1.0f);
const glm::vec4 kDim(0.55f, 0.55f, 0.55f, 0.8f);
const glm::vec4 kHot(1.0f, 1.0f, 1.0f, 1.0f);
constexpr double kPick = 8.0;

std::optional<glm::dvec3> gViewDir;

double handlePixels() { return PrefStore::get().value<double>("general", "/manipulator/size", 100.0); }

bool sessionLive()
{
    const lo::Op* op = lo::current();
    return op && op->command == kCommand && lo::live() && !uv::lastProjections().empty() &&
           uv::lastProjections().front().type != Projection::Camera;
}

double segmentDistance(QPointF p, QPointF a, QPointF b)
{
    QPointF d = b - a;
    double len2 = QPointF::dotProduct(d, d);
    double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
    QPointF q = a + d * t;
    return std::hypot(p.x() - q.x(), p.y() - q.y());
}

double pixelDistance(QPointF a, QPointF b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

/** The first projected mesh's frame, placed in the world. */
struct Shape
{
    uv::ProjectionRecord rec;
    glm::dmat3 w3{1.0};
    glm::dmat4 inv{1.0};

    explicit Shape(const uv::ProjectionRecord& r) : rec(r), w3(r.world), inv(glm::inverse(r.world)) {}
    const uv::ProjectFrame& fr() const { return rec.frame; }
    /** A point in frame coordinates (object units) to world. */
    glm::dvec3 world(const glm::dvec3& l) const { return glm::dvec3(rec.world * glm::dvec4(fr().center + fr().axes * l, 1.0)); }
    /** A frame direction to world, unnormalized: its length is world units per object unit. */
    glm::dvec3 dir(const glm::dvec3& l) const { return w3 * (fr().axes * l); }
    glm::dvec3 center() const { return world(glm::dvec3(0.0)); }
    /** A world point to frame coordinates. */
    glm::dvec3 local(const glm::dvec3& p) const { return glm::transpose(fr().axes) * (glm::dvec3(inv * glm::dvec4(p, 1.0)) - fr().center); }
    double seam() const { return fr().seamAngle; }
    glm::dvec3 around(double angle, double r, double z) const { return {r * std::cos(angle), r * std::sin(angle), z}; }
};

enum class Kind
{
    None,
    Center,
    Ring,   // rotate about axis
    Size,   // width, height or depth along axis, from sign's side
    Radius,
    Seam,
};

struct Handle
{
    Kind kind = Kind::None;
    int axis = 0;
    int sign = 1;
    bool operator==(const Handle&) const = default;
};

struct Marker
{
    Handle h;
    glm::dvec3 local;
};

/** The square and disc handles of a shape, in frame coordinates. */
std::vector<Marker> markers(const Shape& s)
{
    const uv::ProjectFrame& f = s.fr();
    std::vector<Marker> out;
    switch (s.rec.type)
    {
    case Projection::Planar:
        for (int sg : {1, -1})
        {
            out.push_back({{Kind::Size, 0, sg}, {sg * f.width * 0.5, 0, 0}});
            out.push_back({{Kind::Size, 1, sg}, {0, sg * f.height * 0.5, 0}});
        }
        break;
    case Projection::Box:
        for (int sg : {1, -1})
        {
            out.push_back({{Kind::Size, 0, sg}, {sg * f.width * 0.5, 0, 0}});
            out.push_back({{Kind::Size, 1, sg}, {0, sg * f.height * 0.5, 0}});
            out.push_back({{Kind::Size, 2, sg}, {0, 0, sg * f.depth * 0.5}});
        }
        break;
    case Projection::Cylindrical:
        for (int sg : {1, -1})
            out.push_back({{Kind::Size, 2, sg}, {0, 0, sg * f.height * 0.5}});
        out.push_back({{Kind::Radius}, s.around(s.seam() + glm::half_pi<double>(), f.radius, 0.0)});
        out.push_back({{Kind::Seam}, s.around(s.seam(), f.radius, f.height * 0.5)});
        break;
    case Projection::Spherical:
        out.push_back({{Kind::Radius}, s.around(s.seam() + glm::half_pi<double>(), f.radius, 0.0)});
        out.push_back({{Kind::Seam}, s.around(s.seam(), f.radius, 0.0)});
        break;
    default: break;
    }
    return out;
}

const char* sizeFlag(Projection t, int axis)
{
    if (t == Projection::Cylindrical)
        return "height";
    return axis == 0 ? "width" : axis == 1 ? "height" : "depth";
}

/** Draws a world polyline, broken where it goes behind the camera. */
void wire(OverlayPainter& p, const ViewInfo& view, const std::vector<glm::dvec3>& pts, float width, const glm::vec4& color, bool closed = false)
{
    std::vector<QPointF> run;
    auto flush = [&] {
        if (run.size() > 1)
            p.polyline(run, width, color);
        run.clear();
    };
    for (size_t i = 0; i < pts.size() + (closed ? 1 : 0); ++i)
    {
        if (auto q = view.project(pts[i % pts.size()]))
            run.push_back(*q);
        else
            flush();
    }
    flush();
}

std::vector<glm::dvec3> circle(const Shape& s, double r, double z, int n = 64)
{
    std::vector<glm::dvec3> out;
    for (int i = 0; i < n; ++i)
        out.push_back(s.world(s.around(glm::two_pi<double>() * i / n, r, z)));
    return out;
}

/** A world ring around c in the plane perpendicular to n. */
std::vector<glm::dvec3> ring(const glm::dvec3& c, const glm::dvec3& n, double r, int count = 64)
{
    glm::dvec3 a = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
    glm::dvec3 b = glm::cross(n, a);
    std::vector<glm::dvec3> out;
    for (int i = 0; i < count; ++i)
    {
        double t = glm::two_pi<double>() * i / count;
        out.push_back(c + (a * std::cos(t) + b * std::sin(t)) * r);
    }
    return out;
}

class UvProjectTool final : public ViewportTool
{
public:
    const char* name() const override { return kToolName; }

    void deactivate() override
    {
        if (mDrag)
            finishDrag();
    }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        std::optional<Shape> s = shape();
        Handle h = s ? pick(*s, e->position(), view) : Handle{};
        if (h.kind == Kind::None)
        {
            // A click away from the handles ends the session and goes on to select as usual; nothing touches this after.
            viewportTools::set(nullptr);
            return false;
        }
        beginDrag(*s, h, e, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        if (!mDrag)
        {
            std::optional<Shape> s = shape();
            Handle h = s ? pick(*s, e->position(), view) : Handle{};
            if (!(h == mHover))
            {
                mHover = h;
                viewportTools::requestRedraw();
            }
            return false;
        }
        dragTo(e->position(), e->modifiers(), view);
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (e->button() != Qt::LeftButton || !mDrag)
            return false;
        finishDrag();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Escape:
            if (mDrag)
            {
                cancelDrag();
                return true;
            }
            [[fallthrough]];
        case Qt::Key_Return:
        case Qt::Key_Enter: QTimer::singleShot(0, [] { endSession(); }); return true;
        default: return false;
        }
    }

    void draw(render::FrameState&, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        std::optional<Shape> s = shape();
        if (!s)
        {
            if (!mDrag)
                QTimer::singleShot(0, [] { endSession(); });
            return;
        }
        Handle hot = mDrag ? mDrag->h : mHover;
        auto colorOf = [&](const Handle& h) { return h == hot ? kHot : kGray; };
        drawShape(*s, p, view, hot);

        glm::dvec3 c = s->center();
        double upp = view.unitsPerPixel(c);
        for (int i = 0; i < 3; ++i)
        {
            Handle h{Kind::Ring, i};
            glm::vec4 col(viewportDisplay::axisColor(i), h == hot ? 1.0f : 0.55f);
            wire(p, view, ring(c, glm::normalize(s->dir(axisVector(i))), ringPixels() * upp), h == hot ? 3.0f : 1.5f, col, true);
        }
        if (auto cp = view.project(c))
            square(p, *cp, colorOf({Kind::Center}));
        for (const Marker& m : markers(*s))
            if (auto q = view.project(s->world(m.local)))
            {
                if (m.h.kind == Kind::Seam)
                    p.disc(*q, m.h == hot ? 7.0f : 5.5f, colorOf(m.h));
                else
                    square(p, *q, colorOf(m.h));
            }

        if (mDrag)
            p.label(mDrag->lastPx + QPointF(18, -14), mDrag->readout);
        else if (hot.kind != Kind::None)
            if (auto at = handleScreen(*s, hot, view))
                p.text(*at + QPointF(14, 4), hint(s->rec.type, hot), kGray);
        drawToolMessage(p, view, summary(*s));
        drawToolMessage(p, view, "Drag the handles; Shift scales U/V instead, Ctrl+centre offsets U/V. Enter or Esc to finish.", kDim);
    }

private:
    struct Drag
    {
        Handle h;
        Shape start;
        json base;
        QPointF startPx, lastPx;
        glm::dvec3 startHit{0.0};
        double startParam = 0.0;
        double angle = 0.0; // accumulated ring turn, radians
        double lastAngle = 0.0;
        json last;
        QString readout;
    };

    static glm::dvec3 axisVector(int i)
    {
        glm::dvec3 v(0.0);
        v[i] = 1.0;
        return v;
    }

    static double ringPixels() { return handlePixels() * 0.75; }

    static void endSession()
    {
        if (ViewportTool* t = viewportTools::active(); t && std::string_view(t->name()) == kToolName)
            viewportTools::set(nullptr);
    }

    static std::optional<Shape> shape()
    {
        if (!sessionLive() || !scene::Graph::current().node(uv::lastProjections().front().node))
            return std::nullopt;
        return Shape(uv::lastProjections().front());
    }

    static void square(OverlayPainter& p, QPointF at, const glm::vec4& c)
    {
        QRectF box(at.x() - 5, at.y() - 5, 10, 10);
        std::vector<QPointF> pts = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
        p.fill(pts, {c.r, c.g, c.b, c == kHot ? 0.6f : 0.25f});
        p.polyline(pts, c == kHot ? 2.0f : 1.5f, c, true);
    }

    static QString hint(Projection t, const Handle& h)
    {
        switch (h.kind)
        {
        case Kind::Center: return "move";
        case Kind::Ring: return "rotate";
        case Kind::Size: return sizeFlag(t, h.axis);
        case Kind::Radius: return "radius";
        case Kind::Seam: return "seam";
        default: return {};
        }
    }

    QString summary(const Shape& s) const
    {
        const uv::ProjectFrame& f = s.fr();
        const lo::Op* op = lo::current();
        auto flag = [&](const char* name, double fallback) {
            return op && op->call.contains(name) && op->call[name].is_number() ? op->call[name].get<double>() : fallback;
        };
        QString shape;
        switch (s.rec.type)
        {
        case Projection::Planar: shape = QString("Planar  %1 x %2").arg(f.width, 0, 'f', 2).arg(f.height, 0, 'f', 2); break;
        case Projection::Box: shape = QString("Box  %1 x %2 x %3").arg(f.width, 0, 'f', 2).arg(f.height, 0, 'f', 2).arg(f.depth, 0, 'f', 2); break;
        case Projection::Cylindrical:
            shape = QString("Cylindrical  r %1  h %2  seam %3°").arg(f.radius, 0, 'f', 2).arg(f.height, 0, 'f', 2).arg(glm::degrees(f.seamAngle), 0, 'f', 1);
            break;
        case Projection::Spherical: shape = QString("Spherical  r %1  seam %2°").arg(f.radius, 0, 'f', 2).arg(glm::degrees(f.seamAngle), 0, 'f', 1); break;
        default: break;
        }
        return QString("%1    U x%2 %3%4    V x%5 %6%7")
            .arg(shape)
            .arg(flag("scaleU", 1.0), 0, 'f', 2)
            .arg(flag("offsetU", 0.0) < 0 ? "" : "+")
            .arg(flag("offsetU", 0.0), 0, 'f', 2)
            .arg(flag("scaleV", 1.0), 0, 'f', 2)
            .arg(flag("offsetV", 0.0) < 0 ? "" : "+")
            .arg(flag("offsetV", 0.0), 0, 'f', 2);
    }

    void drawShape(const Shape& s, OverlayPainter& p, const ViewInfo& view, const Handle& hot) const
    {
        const uv::ProjectFrame& f = s.fr();
        auto edge = [&](const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& c, float w = 1.5f) { wire(p, view, {s.world(a), s.world(b)}, w, c); };
        switch (s.rec.type)
        {
        case Projection::Planar:
        {
            double x = f.width * 0.5, y = f.height * 0.5;
            wire(p, view, {s.world({-x, -y, 0}), s.world({x, -y, 0}), s.world({x, y, 0}), s.world({-x, y, 0})}, 1.5f, kGray, true);
            // The uv origin corner, and the normal the projection looks along.
            edge({-x, -y, 0}, {-x + f.width * 0.15, -y, 0}, kHot, 2.5f);
            edge({-x, -y, 0}, {-x, -y + f.height * 0.15, 0}, kHot, 2.5f);
            edge({0, 0, 0}, {0, 0, std::max(f.width, f.height) * 0.2}, kDim);
            break;
        }
        case Projection::Box:
        {
            glm::dvec3 h(f.width * 0.5, f.height * 0.5, f.depth * 0.5);
            for (int i = 0; i < 3; ++i)
                for (int a : {-1, 1})
                    for (int b : {-1, 1})
                    {
                        glm::dvec3 p0(0.0), p1(0.0);
                        int j = (i + 1) % 3, k = (i + 2) % 3;
                        p0[i] = -h[i], p1[i] = h[i];
                        p0[j] = p1[j] = a * h[j];
                        p0[k] = p1[k] = b * h[k];
                        edge(p0, p1, kGray);
                    }
            break;
        }
        case Projection::Cylindrical:
        {
            double z = f.height * 0.5;
            wire(p, view, circle(s, f.radius, z), 1.5f, kGray, true);
            wire(p, view, circle(s, f.radius, -z), 1.5f, kGray, true);
            for (int i = 1; i < 8; ++i)
            {
                double a = s.seam() + glm::two_pi<double>() * i / 8.0;
                edge(s.around(a, f.radius, -z), s.around(a, f.radius, z), kDim, 1.0f);
            }
            edge(s.around(s.seam(), f.radius, -z), s.around(s.seam(), f.radius, z), hot.kind == Kind::Seam ? kHot : kGray, 2.5f);
            edge({0, 0, -z}, {0, 0, z}, kDim, 1.0f);
            break;
        }
        case Projection::Spherical:
        {
            double r = f.radius;
            wire(p, view, circle(s, r, 0.0), 1.5f, kGray, true);
            for (double a : {s.seam(), s.seam() + glm::half_pi<double>()})
            {
                std::vector<glm::dvec3> m;
                for (int i = 0; i <= 32; ++i)
                {
                    double t = -glm::half_pi<double>() + glm::pi<double>() * i / 32.0;
                    m.push_back(s.world(s.around(a, r * std::cos(t), r * std::sin(t))));
                }
                bool seam = a == s.seam();
                wire(p, view, m, seam ? 2.5f : 1.0f, seam ? (hot.kind == Kind::Seam ? kHot : kGray) : kDim);
                if (!seam)
                {
                    for (glm::dvec3& q : m)
                        q = s.world(glm::dvec3(-1, -1, 1) * s.local(q));
                    wire(p, view, m, 1.0f, kDim);
                }
            }
            edge({0, 0, r}, {0, 0, r * 1.25}, kGray, 2.0f);
            break;
        }
        default: break;
        }
    }

    std::optional<QPointF> handleScreen(const Shape& s, const Handle& h, const ViewInfo& view) const
    {
        if (h.kind == Kind::Center || h.kind == Kind::Ring)
            return view.project(s.center());
        for (const Marker& m : markers(s))
            if (m.h == h)
                return view.project(s.world(m.local));
        return std::nullopt;
    }

    Handle pick(const Shape& s, QPointF px, const ViewInfo& view) const
    {
        Handle best;
        double bestD = kPick;
        for (const Marker& m : markers(s))
            if (auto q = view.project(s.world(m.local)); q && pixelDistance(*q, px) <= bestD)
                bestD = pixelDistance(*q, px), best = m.h;
        glm::dvec3 c = s.center();
        if (auto q = view.project(c); q && pixelDistance(*q, px) <= std::min(bestD + 0.01, kPick))
            return Handle{Kind::Center};
        if (best.kind != Kind::None)
            return best;
        double upp = view.unitsPerPixel(c);
        double ringD = 6.0;
        for (int i = 0; i < 3; ++i)
        {
            std::vector<glm::dvec3> pts = ring(c, glm::normalize(s.dir(axisVector(i))), ringPixels() * upp);
            for (size_t k = 0; k < pts.size(); ++k)
            {
                auto a = view.project(pts[k]), b = view.project(pts[(k + 1) % pts.size()]);
                if (a && b && segmentDistance(px, *a, *b) < ringD)
                    ringD = segmentDistance(px, *a, *b), best = {Kind::Ring, i};
            }
        }
        return best;
    }

    /** The full frame as flags, so the re-run reproduces it exactly whatever the fit would do. */
    static json frameFlags(const Shape& s)
    {
        const uv::ProjectFrame& f = s.fr();
        return {{"center", vec(f.center)},
                {"rotation", vec(uv::frameEuler(f.axes))},
                {"width", f.width},
                {"height", f.height},
                {"depth", f.depth},
                {"radius", f.radius},
                {"seamAngle", glm::degrees(f.seamAngle)},
                {"scaleU", s.rec.scale.x},
                {"scaleV", s.rec.scale.y},
                {"offsetU", s.rec.offset.x},
                {"offsetV", s.rec.offset.y}};
    }

    void beginDrag(const Shape& s, const Handle& h, QMouseEvent* e, const ViewInfo& view)
    {
        lo::endDrag();
        Drag d{.h = h, .start = s, .base = frameFlags(s)};
        d.startPx = d.lastPx = e->position();
        glm::dvec3 o, dir;
        view.ray(e->position(), o, dir);
        glm::dvec3 c = s.center();
        switch (h.kind)
        {
        case Kind::Center: gizmo::rayPlane(o, dir, c, view.viewDir(c), d.startHit); break;
        case Kind::Ring:
        {
            glm::dvec3 n = glm::normalize(s.dir(axisVector(h.axis))), hit;
            if (gizmo::rayPlane(o, dir, c, n, hit))
                d.lastAngle = ringAngle(n, hit - c);
            break;
        }
        default: break;
        }
        d.readout = hint(s.rec.type, h);
        d.last = d.base;
        mDrag = std::move(d);
    }

    static double ringAngle(const glm::dvec3& n, const glm::dvec3& r)
    {
        glm::dvec3 a = glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
        glm::dvec3 b = glm::cross(n, a);
        return std::atan2(glm::dot(r, b), glm::dot(r, a));
    }

    static double snapDegrees(double deg)
    {
        IncrementSnap inc = tools::incrementSnap(Tool::Rotate);
        return inc.enabled ? gizmo::snapValue(deg, inc.step) : deg;
    }

    void dragTo(QPointF px, Qt::KeyboardModifiers mods, const ViewInfo& view)
    {
        Drag& d = *mDrag;
        d.lastPx = px;
        const Shape& s = d.start;
        const uv::ProjectFrame& f = s.fr();
        glm::dvec3 o, dir;
        view.ray(px, o, dir);
        glm::dvec3 c = s.center();
        bool shift = mods & Qt::ShiftModifier;
        json flags = d.base;
        double tiny = 1e-6 * std::max({f.width, f.height, f.depth, f.radius, 1e-6});
        auto lengthAlong = [&](const glm::dvec3& from, const glm::dvec3& localDir) {
            glm::dvec3 w = s.dir(localDir);
            double len = glm::length(w);
            return len > 0.0 ? gizmo::lineParam(from, w / len, o, dir) / len : 0.0;
        };
        auto scaleChannel = [&](const char* channel, double from, double to) {
            double v = d.base[channel].get<double>() * (from > 0.0 ? to / from : 1.0);
            flags[channel] = v;
            d.readout = QString("%1 x%2").arg(channel[5] == 'U' ? "U scale" : "V scale").arg(v, 0, 'f', 3);
        };

        switch (d.h.kind)
        {
        case Kind::Center:
        {
            if (mods & Qt::ControlModifier)
            {
                QPointF delta = px - d.startPx;
                double u = d.base["offsetU"].get<double>() + delta.x() / 250.0, v = d.base["offsetV"].get<double>() - delta.y() / 250.0;
                flags["offsetU"] = u, flags["offsetV"] = v;
                d.readout = QString("U/V offset %1, %2").arg(u, 0, 'f', 3).arg(v, 0, 'f', 3);
                break;
            }
            glm::dvec3 hit;
            if (!gizmo::rayPlane(o, dir, c, view.viewDir(c), hit))
                return;
            glm::dvec3 moved = glm::dvec3(s.inv * glm::dvec4(c + hit - d.startHit, 1.0));
            flags["center"] = vec(moved);
            d.readout = QString("centre %1, %2, %3").arg(moved.x, 0, 'f', 2).arg(moved.y, 0, 'f', 2).arg(moved.z, 0, 'f', 2);
            break;
        }
        case Kind::Ring:
        {
            glm::dvec3 n = glm::normalize(s.dir(axisVector(d.h.axis))), hit;
            if (!gizmo::rayPlane(o, dir, c, n, hit))
                return;
            double a = ringAngle(n, hit - c);
            d.angle += std::remainder(a - d.lastAngle, glm::two_pi<double>());
            d.lastAngle = a;
            double deg = snapDegrees(glm::degrees(d.angle));
            // The world turn about the world axis is the same turn about the frame axis in object space when the
            // object's scale is uniform; a mirrored object turns the other way.
            double sign = glm::determinant(s.w3) < 0.0 ? -1.0 : 1.0;
            glm::dmat3 turn(glm::rotate(glm::dmat4(1.0), glm::radians(deg) * sign, f.axes[d.h.axis]));
            flags["rotation"] = vec(uv::frameEuler(turn * f.axes));
            d.readout = QString("rotate %1°").arg(deg, 0, 'f', 1);
            break;
        }
        case Kind::Size:
        {
            const double from = s.rec.type == Projection::Cylindrical || d.h.axis == 1 ? f.height : d.h.axis == 0 ? f.width : f.depth;
            // Sizes stay centred on the frame, as the handle on either side pulls both.
            double size = 2.0 * std::max(d.h.sign * lengthAlong(c, axisVector(d.h.axis)), tiny);
            if (shift)
                scaleChannel(d.h.axis == 0 ? "scaleU" : "scaleV", from, size);
            else
            {
                const char* name = sizeFlag(s.rec.type, d.h.axis);
                flags[name] = size;
                d.readout = QString("%1 %2").arg(name).arg(size, 0, 'f', 2);
            }
            break;
        }
        case Kind::Radius:
        {
            double r = std::max(lengthAlong(c, s.around(s.seam() + glm::half_pi<double>(), 1.0, 0.0)), tiny);
            if (shift)
                scaleChannel("scaleU", f.radius, r);
            else
            {
                flags["radius"] = r;
                d.readout = QString("radius %1").arg(r, 0, 'f', 2);
            }
            break;
        }
        case Kind::Seam:
        {
            glm::dvec3 capCenter = s.world({0, 0, s.rec.type == Projection::Cylindrical ? f.height * 0.5 : 0.0}), hit;
            glm::dvec3 n = s.dir(axisVector(2));
            if (glm::length(n) <= 0.0 || !gizmo::rayPlane(o, dir, capCenter, glm::normalize(n), hit))
                return;
            glm::dvec3 l = s.local(hit);
            if (glm::length(glm::dvec2(l)) <= tiny)
                return;
            double deg = snapDegrees(glm::degrees(std::atan2(l.y, l.x)));
            flags["seamAngle"] = deg;
            d.readout = QString("seam %1°").arg(deg, 0, 'f', 1);
            break;
        }
        default: return;
        }
        if (flags == d.last)
            return;
        try
        {
            lo::rerun(flags, CallSource::Script, true);
            d.last = flags;
        }
        catch (const CommandError& e)
        {
            log::warn("uvProject: {}", e.what());
        }
        viewportTools::requestRedraw();
    }

    void finishDrag()
    {
        mDrag.reset();
        lo::endDrag();
        if (const lo::Op* op = lo::current(); op && op->command == kCommand)
            log::write(LogLevel::Echo, ops::toPython(op->command, op->call) + "\n");
        viewportTools::requestRedraw();
    }

    void cancelDrag()
    {
        mDrag.reset();
        lo::endDrag(false);
        viewportTools::requestRedraw();
    }

    std::optional<Drag> mDrag;
    Handle mHover;
};

void onLastOp(lo::Change change)
{
    bool ours = viewportTools::active() && std::string_view(viewportTools::active()->name()) == kToolName;
    if ((change == lo::Change::Recorded || change == lo::Change::Rerun || change == lo::Change::Restored) && sessionLive())
    {
        if (!ours)
            QTimer::singleShot(0, [] {
                ViewportTool* t = viewportTools::active();
                if (sessionLive() && !(t && std::string_view(t->name()) == kToolName))
                    viewportTools::set(std::make_unique<UvProjectTool>());
            });
        return;
    }
    if (ours && !sessionLive())
        QTimer::singleShot(0, [] {
            ViewportTool* t = viewportTools::active();
            if (t && std::string_view(t->name()) == kToolName && !sessionLive())
                viewportTools::set(nullptr);
        });
}
} // namespace

void installUvProjectTool()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    lo::install();
    lo::addListener(onLastOp);
    // The seam goes behind the last perspective view drawn (any view when there is none).
    viewportHooks::addDrawer("lila.uvProject.view", [](render::FrameState&, OverlayPainter&, const ViewInfo& view) {
        if (view.ortho && gViewDir)
            return;
        glm::dvec3 forward = -glm::dvec3(view.view[0][2], view.view[1][2], view.view[2][2]);
        if (glm::length(forward) > 1e-12)
            gViewDir = glm::normalize(forward);
    });
    uv::projectionViewProvider() = [] { return gViewDir; };
}
} // namespace lila::ui
