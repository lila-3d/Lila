#include "ui/SketchUi.h"

#include "core/Profile.h"
#include "curve/CurveNodes.h"
#include "curve/SketchNode.h"
#include "scene/Node.h"
#include "ui/Theme.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <map>

namespace lila::ui::sketchUi
{
namespace sk = curve::sketch;

namespace
{
struct Entry
{
    const void* data = nullptr;
    std::string text;
    bool empty = true;
    Display display;
};

std::map<scene::NodeId, Entry>& cache()
{
    static std::map<scene::NodeId, Entry> c;
    return c;
}
} // namespace

const Display* display(const scene::Node& n)
{
    LILA_ZONE();

    const int attr = n.type().attrIndex("sketch");
    if (attr < 0)
        return nullptr;
    const std::string& text = n.as<std::string>(attr);
    const scene::PlugValue& v = n.value(curve::CurveShape::kLocal);
    const scene::DataPtr* d = std::get_if<scene::DataPtr>(&v);
    const void* key = d ? d->get() : nullptr;
    Entry& e = cache()[n.id()];
    if (e.data != key || e.text != text)
    {
        e.data = key;
        e.text = text;
        e.display = {};
        e.display.sketch = curve::sketchOf(n);
        e.empty = e.display.sketch.empty();
        if (!e.empty)
        {
            const curve::Splines& s = curve::splinesOf(v);
            for (const curve::Spline& sp : s)
                e.display.fits.push_back(sk::circleOf(sp, e.display.sketch.plane));
            e.display.status = sk::analyze(s, e.display.sketch);
            curve::Splines check = s;
            sk::SolveResult r = sk::solve(check, e.display.sketch, {});
            e.display.ok = r.ok;
            e.display.conflicting = r.conflicting;
            e.display.invalid = r.invalid;
        }
    }
    return e.empty ? nullptr : &e.display;
}

glm::vec4 constrainedColor() { return themeLinear("curveConstrained", glm::vec4(0.012f, 0.012f, 0.012f, 1.0f)); }

glm::vec4 constrainedInk() { return themeSrgb("curveConstrained", glm::vec4(0.1f, 0.1f, 0.1f, 1.0f)); }

bool spanFixed(const Display& d, const curve::Splines& s, size_t si, double u)
{
    if (si >= s.size() || si >= d.status.splineFixed.size())
        return false;
    if (d.status.splineFixed[si])
        return true;
    const curve::Spline& sp = s[si];
    // Poly and Bezier spans are the legs between points; NURBS spans aren't, so only the whole curve counts there.
    if (sp.kind == curve::Kind::Nurbs && !(sp.degree == 1 && sp.knots.empty()))
        return false;
    if (si < d.fits.size() && d.fits[si].shape != sk::Shape::Points)
        return false;
    const size_t k = size_t(std::max(0.0, std::floor(u + 1e-9)));
    return d.status.lineFixed(si, std::min(k, sp.size() ? sp.size() - 1 : 0));
}

const char* label(sk::Type t)
{
    switch (t)
    {
    case sk::Type::Coincident: return "Coincident";
    case sk::Type::Horizontal: return "Horizontal";
    case sk::Type::Vertical: return "Vertical";
    case sk::Type::Parallel: return "Parallel";
    case sk::Type::Perpendicular: return "Perpendicular";
    case sk::Type::Tangent: return "Tangent";
    case sk::Type::Equal: return "Equal";
    case sk::Type::Concentric: return "Concentric";
    case sk::Type::Midpoint: return "Midpoint";
    case sk::Type::Symmetric: return "Symmetric";
    case sk::Type::Fixed: return "Fix";
    case sk::Type::PointOnCurve: return "Point on Curve";
    case sk::Type::Collinear: return "Collinear";
    case sk::Type::Distance: return "Distance";
    case sk::Type::HorizontalDistance: return "Horizontal Distance";
    case sk::Type::VerticalDistance: return "Vertical Distance";
    case sk::Type::Length: return "Length";
    case sk::Type::Radius: return "Radius";
    case sk::Type::Diameter: return "Diameter";
    case sk::Type::Angle: return "Angle";
    default: return "?";
    }
}

void drawGlyph(OverlayPainter& p, sk::Type t, QPointF c, const glm::vec4& col, float size)
{
    const double h = size * 0.5;
    const float w = std::max(1.2f, size / 9.0f);
    auto L = [&](double x0, double y0, double x1, double y1) { p.line(c + QPointF(x0 * h, y0 * h), c + QPointF(x1 * h, y1 * h), w, col); };
    auto ring = [&](double r, double cx = 0.0, double cy = 0.0) {
        std::vector<QPointF> pts;
        for (int k = 0; k < 20; ++k)
        {
            const double a = glm::two_pi<double>() * k / 20.0;
            pts.push_back(c + QPointF((cx + std::cos(a) * r) * h, (cy + std::sin(a) * r) * h));
        }
        p.polyline(pts, w, col, true);
    };
    switch (t)
    {
    case sk::Type::Coincident:
        p.rect(QRectF(c - QPointF(h * 0.45, h * 0.45), QSizeF(h * 0.9, h * 0.9)), col);
        break;
    case sk::Type::Horizontal: L(-0.9, 0.0, 0.9, 0.0); break;
    case sk::Type::Vertical: L(0.0, -0.9, 0.0, 0.9); break;
    case sk::Type::Parallel:
        L(-0.7, 0.8, -0.1, -0.8);
        L(0.1, 0.8, 0.7, -0.8);
        break;
    case sk::Type::Perpendicular:
        L(-0.8, 0.8, 0.8, 0.8);
        L(0.0, 0.8, 0.0, -0.8);
        break;
    case sk::Type::Tangent:
        ring(0.5, 0.0, -0.2);
        L(-0.9, 0.3, 0.9, 0.3);
        break;
    case sk::Type::Equal:
        L(-0.8, -0.35, 0.8, -0.35);
        L(-0.8, 0.35, 0.8, 0.35);
        break;
    case sk::Type::Concentric:
        ring(0.85);
        ring(0.4);
        break;
    case sk::Type::Midpoint:
        L(-0.9, 0.6, 0.9, 0.6);
        p.triangle(c + QPointF(0.0, -0.5 * h), c + QPointF(-0.5 * h, 0.4 * h), c + QPointF(0.5 * h, 0.4 * h), col);
        break;
    case sk::Type::Symmetric:
        L(0.0, -0.9, 0.0, 0.9);
        L(-0.8, -0.6, -0.4, 0.0);
        L(-0.4, 0.0, -0.8, 0.6);
        L(0.8, -0.6, 0.4, 0.0);
        L(0.4, 0.0, 0.8, 0.6);
        break;
    case sk::Type::Fixed:
        // A padlock: shackle and body.
        L(-0.4, -0.1, -0.4, -0.5);
        L(0.4, -0.1, 0.4, -0.5);
        L(-0.4, -0.5, 0.0, -0.85);
        L(0.0, -0.85, 0.4, -0.5);
        p.rect(QRectF(c + QPointF(-0.65 * h, -0.1 * h), QSizeF(1.3 * h, 0.95 * h)), col);
        break;
    case sk::Type::PointOnCurve:
    {
        std::vector<QPointF> arcPts;
        for (int k = 0; k <= 10; ++k)
        {
            const double a = glm::pi<double>() * (1.1 + 0.8 * k / 10.0);
            arcPts.push_back(c + QPointF(std::cos(a) * 0.95 * h, (0.75 + std::sin(a) * 0.95) * h));
        }
        p.polyline(arcPts, w, col);
        p.disc(c + QPointF(0.0, -0.2 * h), float(h * 0.32), col, 10);
        break;
    }
    case sk::Type::Collinear:
        L(-0.9, 0.5, 0.9, -0.5);
        p.disc(c + QPointF(-0.6 * h, 0.33 * h), float(h * 0.22), col, 8);
        p.disc(c + QPointF(0.6 * h, -0.33 * h), float(h * 0.22), col, 8);
        break;
    case sk::Type::Distance:
    case sk::Type::Length:
        L(-0.9, 0.0, 0.9, 0.0);
        L(-0.9, -0.5, -0.9, 0.5);
        L(0.9, -0.5, 0.9, 0.5);
        break;
    case sk::Type::HorizontalDistance:
        L(-0.9, 0.3, 0.9, 0.3);
        L(-0.9, -0.6, -0.9, 0.6);
        L(0.9, -0.6, 0.9, 0.6);
        break;
    case sk::Type::VerticalDistance:
        L(0.3, -0.9, 0.3, 0.9);
        L(-0.6, -0.9, 0.6, -0.9);
        L(-0.6, 0.9, 0.6, 0.9);
        break;
    case sk::Type::Radius:
        ring(0.85);
        L(0.0, 0.0, 0.6, -0.6);
        break;
    case sk::Type::Diameter:
        ring(0.85);
        L(-0.6, 0.6, 0.6, -0.6);
        break;
    case sk::Type::Angle:
        L(-0.8, 0.7, 0.8, 0.7);
        L(-0.8, 0.7, 0.5, -0.8);
        break;
    default: break;
    }
}
} // namespace lila::ui::sketchUi
