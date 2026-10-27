#include "debug/DebugDraw.h"
#include "math/Math.h"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/operators.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <optional>

namespace nb = nanobind;
using namespace nb::literals;

namespace lila::python
{
namespace
{
using debugDraw::Channel;
using debugDraw::Style;
using math::LinearColor;
using math::Quat;
using math::Rotator;
using math::Vector;

Style style(const LinearColor& color, std::optional<double> duration, float thickness, bool onTop, bool persistent,
            const std::optional<Channel>& channel)
{
    if (persistent && (!channel || !channel->id()))
        throw nb::value_error("persistent draws need a channel: ch = lila.debug_draw.channel('my_tool'), then channel=ch (and "
                              "ch.clear() when done)");
    return {.color = color,
            .duration = duration ? *duration : debugDraw::kDefaultDuration,
            .thickness = thickness,
            .onTop = onTop,
            .persistent = persistent,
            .channel = channel ? *channel : Channel()};
}

/** None, a Quat, a Rotator, or a sequence of 3 (pitch, yaw, roll) or 4 (x, y, z, w). */
Quat rotation(nb::handle h)
{
    if (h.is_none())
        return {};
    if (nb::isinstance<Rotator>(h))
        return nb::cast<Rotator>(h).quaternion();
    Quat q;
    if (nb::try_cast<Quat>(h, q))
        return q;
    Rotator r;
    if (nb::try_cast<Rotator>(h, r))
        return r.quaternion();
    throw nb::type_error("rotation must be a Rotator, a Quat, (pitch, yaw, roll) or (x, y, z, w)");
}

using Points = nb::ndarray<const double, nb::ndim<2>, nb::device::cpu, nb::c_contig>;

/** A list of Vectors (or tuples), or an (N, 3) float64 array. */
std::vector<Vector> points(nb::handle h)
{
    Points a;
    if (nb::try_cast<Points>(h, a))
    {
        if (a.shape(1) != 3)
            throw nb::value_error("points must be an (N, 3) array");
        std::vector<Vector> out(a.shape(0));
        const double* d = a.data();
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = Vector(d[3 * i], d[3 * i + 1], d[3 * i + 2]);
        return out;
    }
    std::vector<Vector> out;
    if (nb::try_cast<std::vector<Vector>>(h, out))
        return out;
    throw nb::type_error("points must be a list of Vectors or (x, y, z) tuples, or an (N, 3) float64 array");
}

#define LILA_STYLE_PARAMS                                                                                                              \
    const LinearColor &color, std::optional<double> duration, float thickness, bool onTop, bool persistent,                          \
        const std::optional<Channel>&channel
#define LILA_STYLE_ARGS style(color, duration, thickness, onTop, persistent, channel)
#define LILA_STYLE_KW(colorDefault)                                                                                                    \
    "color"_a = colorDefault, "duration"_a = nb::none(), "thickness"_a = 0.0f, nb::kw_only(), "on_top"_a = false, "persistent"_a = false, \
    "channel"_a = nb::none()
} // namespace

/** _lila.debug_draw: lila.debug_draw's native half. */
void bindDebugDraw(nb::module_& top)
{
    nb::module_ m = top.def_submodule("debug_draw", "Debug drawing in the viewports. Use lila.debug_draw instead.");
    const LinearColor white = LinearColor::white();

    nb::class_<Channel>(m, "Channel",
                        "A named group of debug draws with an owner. Draws with persistent=True need one and stay until it is cleared. "
                        "Lila clears every channel on File > New and Open, and removes an extension's when it unloads.")
        .def_prop_ro("valid", &Channel::valid, "False once the channel was removed.")
        .def_prop_ro("name", &Channel::name)
        .def_prop_ro("owner", &Channel::owner)
        .def_prop_rw("visible", &Channel::visible, &Channel::setVisible, "Hides its draws without removing them.")
        .def_prop_ro("primitive_count", &Channel::primitiveCount, "Lines, triangles, points and texts it holds now.")
        .def("clear", &Channel::clear, "Removes everything drawn into it. The channel stays usable.")
        .def("remove", &Channel::remove, "Clears it for good: later draws into it are dropped.")
        .def(nb::self == nb::self)
        .def("__hash__", [](const Channel& c) { return c.id(); })
        .def("__repr__", [](const Channel& c) { return "Channel(" + c.owner() + "/" + c.name() + (c.valid() ? ")" : ", removed)"); });

    m.def("_channel", [](const std::string& name, const std::string& owner) { return debugDraw::channel(name, owner); }, "name"_a, "owner"_a);
    m.def("channels", &debugDraw::channels, "Every channel, Lila's and extensions' included.");

    m.def(
        "draw_debug_point",
        [](const Vector& position, float size, const LinearColor& color, std::optional<double> duration, bool persistent,
           const std::optional<Channel>& channel) { debugDraw::point(position, size, style(color, duration, 0.0f, true, persistent, channel)); },
        "position"_a, "size"_a = 8.0f, "color"_a = white, "duration"_a = nb::none(), nb::kw_only(), "persistent"_a = false,
        "channel"_a = nb::none(), "A square dot size pixels wide, drawn over every surface.");
    m.def(
        "draw_debug_line", [](const Vector& start, const Vector& end, LILA_STYLE_PARAMS) { debugDraw::line(start, end, LILA_STYLE_ARGS); },
        "start"_a, "end"_a, LILA_STYLE_KW(white), "A line from start to end. thickness is in pixels; 0 is a hairline.");
    m.def(
        "draw_debug_lines",
        [](nb::handle pts, LILA_STYLE_PARAMS, bool closed) {
            std::vector<Vector> p = points(pts);
            Style st = LILA_STYLE_ARGS;
            nb::gil_scoped_release release;
            debugDraw::lines(p, closed, st);
        },
        "points"_a, LILA_STYLE_KW(white), "closed"_a = false,
        "A polyline through points (Vectors, tuples or an (N, 3) array), back to the first when closed. Far cheaper than one "
        "draw_debug_line per segment.");
    m.def(
        "draw_debug_segments",
        [](nb::handle pts, LILA_STYLE_PARAMS) {
            std::vector<Vector> p = points(pts);
            Style st = LILA_STYLE_ARGS;
            nb::gil_scoped_release release;
            debugDraw::segments(p, st);
        },
        "points"_a, LILA_STYLE_KW(white), "Independent lines between points 0-1, 2-3, ...");
    m.def(
        "draw_debug_directional_arrow",
        [](const Vector& start, const Vector& end, double arrowSize, LILA_STYLE_PARAMS) {
            debugDraw::arrow(start, end, arrowSize, LILA_STYLE_ARGS);
        },
        "start"_a, "end"_a, "arrow_size"_a = 10.0, LILA_STYLE_KW(white), "A line with an arrow head arrow_size long at end.");
    m.attr("draw_debug_arrow") = m.attr("draw_debug_directional_arrow");
    m.def(
        "draw_debug_box",
        [](const Vector& center, const Vector& extent, LILA_STYLE_PARAMS, nb::handle rot) {
            debugDraw::box(center, extent, rotation(rot), LILA_STYLE_ARGS);
        },
        "center"_a, "extent"_a, LILA_STYLE_KW(white), "rotation"_a = nb::none(),
        "A wire box; extent is half its size. rotation is a Rotator, a Quat or (pitch, yaw, roll).");
    m.def(
        "draw_debug_solid_box",
        [](const Vector& center, const Vector& extent, LILA_STYLE_PARAMS, nb::handle rot) {
            debugDraw::solidBox(center, extent, rotation(rot), LILA_STYLE_ARGS);
        },
        "center"_a, "extent"_a, LILA_STYLE_KW(LinearColor(1.0f, 1.0f, 1.0f, 0.35f)), "rotation"_a = nb::none(),
        "A filled box, blended by the color's alpha and depth tested (on_top is ignored).");
    m.def(
        "draw_debug_sphere",
        [](const Vector& center, double radius, int segments, LILA_STYLE_PARAMS) { debugDraw::sphere(center, radius, segments, LILA_STYLE_ARGS); },
        "center"_a, "radius"_a, "segments"_a = 16, LILA_STYLE_KW(white), "A wire sphere of rings and meridians.");
    m.def(
        "draw_debug_capsule",
        [](const Vector& center, double halfHeight, double radius, nb::handle rot, LILA_STYLE_PARAMS) {
            debugDraw::capsule(center, halfHeight, radius, rotation(rot), LILA_STYLE_ARGS);
        },
        "center"_a, "half_height"_a, "radius"_a, "rotation"_a = nb::none(), LILA_STYLE_KW(white),
        "A wire capsule along the rotated up axis. half_height runs from the center to a tip, as in Unreal.");
    m.def(
        "draw_debug_cylinder",
        [](const Vector& start, const Vector& end, double radius, int segments, LILA_STYLE_PARAMS) {
            debugDraw::cylinder(start, end, radius, segments, LILA_STYLE_ARGS);
        },
        "start"_a, "end"_a, "radius"_a, "segments"_a = 16, LILA_STYLE_KW(white), "A wire cylinder from start to end.");
    m.def(
        "draw_debug_circle",
        [](const Vector& center, double radius, int segments, LILA_STYLE_PARAMS, std::optional<Vector> normal, std::optional<Vector> yAxis,
           std::optional<Vector> zAxis) {
            Vector n = normal ? *normal : yAxis && zAxis ? (*yAxis ^ *zAxis) : Vector();
            debugDraw::circle(center, radius, segments, n, LILA_STYLE_ARGS);
        },
        "center"_a, "radius"_a, "segments"_a = 32, LILA_STYLE_KW(white), "normal"_a = nb::none(), "y_axis"_a = nb::none(), "z_axis"_a = nb::none(),
        "A circle facing normal (the up axis by default), or in the plane of y_axis and z_axis as in Unreal.");
    m.def(
        "draw_debug_cone",
        [](const Vector& origin, const Vector& direction, double length, double angleWidth, double angleHeight, int sides, LILA_STYLE_PARAMS) {
            debugDraw::cone(origin, direction, length, angleWidth, angleHeight, sides, LILA_STYLE_ARGS);
        },
        "origin"_a, "direction"_a, "length"_a, "angle_width"_a, "angle_height"_a, "num_sides"_a = 16, LILA_STYLE_KW(white),
        "A wire cone from origin along direction. The angles are in degrees off the axis (Unreal takes radians).");
    m.def(
        "draw_debug_coordinate_system",
        [](const Vector& location, nb::handle rot, double scale, std::optional<double> duration, float thickness, bool onTop, bool persistent,
           const std::optional<Channel>& channel) {
            debugDraw::coordinateSystem(location, rotation(rot), scale, style(LinearColor::white(), duration, thickness, onTop, persistent, channel));
        },
        "location"_a, "rotation"_a = nb::none(), "scale"_a = 50.0, "duration"_a = nb::none(), "thickness"_a = 0.0f, nb::kw_only(),
        "on_top"_a = false, "persistent"_a = false, "channel"_a = nb::none(), "The rotated X, Y and Z axes in red, green and blue, scale long.");
    m.def(
        "draw_debug_frustum",
        [](const math::Transform& transform, double fov, double aspect, double nearDist, double farDist, LILA_STYLE_PARAMS) {
            debugDraw::frustum(transform, fov, aspect, nearDist, farDist, LILA_STYLE_ARGS);
        },
        "transform"_a, "fov"_a = 90.0, "aspect"_a = 16.0 / 9.0, "near"_a = 10.0, "far"_a = 1000.0, LILA_STYLE_KW(white),
        "A camera's view volume. transform places the camera looking along its forward vector; fov is horizontal, in degrees.");
    m.def(
        "draw_debug_plane",
        [](const math::Plane& plane, const Vector& location, double size, LILA_STYLE_PARAMS) {
            debugDraw::solidPlane(plane.projectPoint(location), plane.getNormal(), size, LILA_STYLE_ARGS);
        },
        "plane"_a, "location"_a, "size"_a = 100.0, LILA_STYLE_KW(LinearColor(1.0f, 1.0f, 1.0f, 0.35f)),
        "A filled square size half wide on plane, centered where location projects onto it. Blended by alpha, depth tested.");
    m.def(
        "draw_debug_string",
        [](const Vector& location, const std::string& text, const LinearColor& color, std::optional<double> duration, bool persistent,
           const std::optional<Channel>& channel) { debugDraw::text(location, text, style(color, duration, 0.0f, true, persistent, channel)); },
        "location"_a, "text"_a, "color"_a = white, "duration"_a = nb::none(), nb::kw_only(), "persistent"_a = false, "channel"_a = nb::none(),
        "Text centered on a world position, facing the screen, over every surface.");

    m.def("flush_debug_draw", [](const std::optional<Channel>& channel) { debugDraw::flush(channel ? *channel : Channel()); },
          "channel"_a = nb::none(), "Clears channel, or everything (every channel included) when None.");
    m.def("flush_persistent_debug_lines", [] { debugDraw::flush(); }, "Clears everything, as Unreal's FlushPersistentDebugLines.");
    m.def(
        "stats",
        [] {
            debugDraw::Stats s = debugDraw::stats();
            nb::dict d;
            d["lines"] = s.lines;
            d["triangles"] = s.triangles;
            d["points"] = s.points;
            d["texts"] = s.texts;
            d["channels"] = s.channels;
            d["dropped"] = s.dropped;
            d["rejected"] = s.rejected;
            return d;
        },
        "What is held now, and how many draws were dropped for the caps or rejected as invalid.");
    m.def(
        "limits",
        [] {
            debugDraw::Limits l = debugDraw::limits();
            nb::dict d;
            d["max_primitives"] = l.maxPrimitives;
            d["max_points"] = l.maxPoints;
            d["max_texts"] = l.maxTexts;
            d["default_duration"] = l.defaultDuration;
            d["max_duration"] = l.maxDuration;
            return d;
        },
        "The caps and durations in force (the debugDraw prefs).");
}
} // namespace lila::python
