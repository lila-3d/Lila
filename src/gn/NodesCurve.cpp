#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "curve/Along.h"
#include "curve/Ops.h"
#include "geo/Ops.h"

namespace lila::gn::nodes
{
namespace
{
using D3 = glm::dvec3;

void curvePrimitive(const char* type, const char* label, std::vector<SocketDecl> inputs, std::vector<PropDecl> props, std::function<CurveSet(ExecContext&)> build,
                    const char* doc)
{
    registerDecl({
        .type = type,
        .label = label,
        .category = "Curve Primitives",
        .doc = doc,
        .props = std::move(props),
        .sockets = {.inputs = std::move(inputs), .outputs = {geoOut("curve")}},
        .exec = [build](ExecContext& c) { c.out("curve", curveGeometry(build(c))); },
    });
}

void primitives()
{
    curvePrimitive(
        "CurveLine", "Curve Line", {in("start", T::Vector, vec(0, 0, 0)), in("end", T::Vector, vec(0, 100, 0))}, {},
        [](ExecContext& c) {
            CurveSet cs;
            D3 p[2] = {c.v("start"), c.v("end")};
            cs.addCurve(p, false);
            return cs;
        },
        "A straight two-point curve.");
    curvePrimitive(
        "CurveCircle", "Curve Circle", {bounded(in("resolution", T::Int, 32), 3, 100000), bounded(in("radius", T::Float, 50.0), 0.0)}, {},
        [](ExecContext& c) {
            int n = int(std::clamp<int64_t>(c.i("resolution"), 3, 100000));
            double r = c.f("radius");
            std::vector<D3> pts(n);
            for (int k = 0; k < n; ++k)
            {
                double a = 2.0 * kPi * k / n;
                pts[k] = {std::cos(a) * r, 0.0, -std::sin(a) * r};
            }
            CurveSet cs;
            cs.addCurve(pts, true);
            return cs;
        },
        "A closed circle in the ground plane. As a Curve to Mesh profile its X and Z map across the path.");
    curvePrimitive(
        "BezierSegment", "Bezier Segment",
        {bounded(in("resolution", T::Int, 16), 1, 100000), in("start", T::Vector, vec(-50, 0, 0)), in("startHandle", T::Vector, vec(-25, 50, 0)),
         in("endHandle", T::Vector, vec(25, 50, 0)), in("end", T::Vector, vec(50, 0, 0))},
        {},
        [](ExecContext& c) {
            int n = int(std::clamp<int64_t>(c.i("resolution"), 1, 100000));
            D3 p0 = c.v("start"), p1 = c.v("startHandle"), p2 = c.v("endHandle"), p3 = c.v("end");
            std::vector<D3> pts(n + 1);
            for (int k = 0; k <= n; ++k)
            {
                double t = double(k) / n, u = 1.0 - t;
                pts[k] = u * u * u * p0 + 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t * p3;
            }
            CurveSet cs;
            cs.addCurve(pts, false);
            return cs;
        },
        "A cubic Bezier between two points, evaluated to a poly curve.");
    curvePrimitive(
        "CurveSpiral", "Spiral",
        {bounded(in("resolution", T::Int, 32, "Points per turn."), 1, 10000), bounded(in("rotations", T::Float, 2.0), 0.0), in("startRadius", T::Float, 50.0),
         in("endRadius", T::Float, 100.0),
         in("height", T::Float, 100.0), in("reverse", T::Bool, false)},
        {},
        [](ExecContext& c) {
            int res = int(std::clamp<int64_t>(c.i("resolution"), 1, 10000));
            double turns = std::max(0.0, c.f("rotations"));
            int n = std::max(2, int(std::ceil(turns * res)) + 1);
            double r0 = c.f("startRadius"), r1 = c.f("endRadius"), h = c.f("height");
            double dir = c.b("reverse") ? -1.0 : 1.0;
            std::vector<D3> pts(n);
            for (int k = 0; k < n; ++k)
            {
                double t = double(k) / (n - 1);
                double a = dir * t * turns * 2.0 * kPi;
                double r = r0 + (r1 - r0) * t;
                pts[k] = {std::cos(a) * r, t * h, -std::sin(a) * r};
            }
            CurveSet cs;
            cs.addCurve(pts, false);
            return cs;
        },
        "A spiral rising up the Y axis.");
}

double polyLength(std::span<const D3> p, bool cyclic)
{
    double l = 0.0;
    for (size_t k = 1; k < p.size(); ++k)
        l += glm::distance(p[k - 1], p[k]);
    if (cyclic && p.size() > 1)
        l += glm::distance(p.back(), p.front());
    return l;
}

/** n points evenly spaced by arc length; closed curves don't repeat the start. */
std::vector<D3> resample(std::span<const D3> p, bool cyclic, int n)
{
    std::vector<D3> out;
    if (p.empty() || n < 1)
        return out;
    std::vector<D3> pts(p.begin(), p.end());
    if (cyclic)
        pts.push_back(p.front());
    std::vector<double> acc(pts.size(), 0.0);
    for (size_t k = 1; k < pts.size(); ++k)
        acc[k] = acc[k - 1] + glm::distance(pts[k - 1], pts[k]);
    double total = acc.back();
    if (total <= 0.0 || pts.size() < 2)
        return std::vector<D3>(n, p.front());
    size_t seg = 0;
    for (int k = 0; k < n; ++k)
    {
        double s = cyclic ? total * k / n : (n == 1 ? 0.0 : total * k / (n - 1));
        while (seg + 2 < acc.size() && acc[seg + 1] < s)
            ++seg;
        double len = acc[seg + 1] - acc[seg];
        double t = len > 0.0 ? (s - acc[seg]) / len : 0.0;
        out.push_back(pts[seg] + (pts[seg + 1] - pts[seg]) * std::clamp(t, 0.0, 1.0));
    }
    return out;
}

void curveOps()
{
    registerDecl({
        .type = "ResampleCurve",
        .label = "Resample Curve",
        .category = "Curve",
        .doc = "Rebuilds curves with evenly spaced points, by count or by segment length.",
        .props = {prop("mode", {"count", "length"}, "count")},
        .sockets = {.inputs = {geoIn("curve"), bounded(in("count", T::Int, 10), 1, 1000000), bounded(in("length", T::Float, 10.0, "Segment length, cm (length mode)."), 1e-6)},
                    .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                bool byLength = c.prop("mode") == "length";
                int count = int(std::clamp<int64_t>(c.i("count"), 1, 1000000));
                double seg = std::max(1e-6, c.f("length"));
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        cs = cs.evaluated();
                                                                        CurveSet out;
                                                                        out.curveAttrs.mergeLayout(cs.curveAttrs);
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                        {
                                                                            bool cyc = cs.cyclic[cu] != 0;
                                                                            int n = count;
                                                                            if (byLength)
                                                                            {
                                                                                double l = polyLength(cs.curvePoints(cu), cyc);
                                                                                n = std::max(cyc ? 3 : 2, int(std::round(l / seg)) + (cyc ? 0 : 1));
                                                                            }
                                                                            out.addCurve(resample(cs.curvePoints(cu), cyc, n), cyc);
                                                                            out.curveAttrs.copyFrom(cs.curveAttrs, out.numCurves() - 1, cu);
                                                                        }
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });

    registerDecl({
        .type = "CurveLength",
        .label = "Curve Length",
        .category = "Curve",
        .doc = "Total length of all curves.",
        .sockets = {.inputs = {geoIn("curve")}, .outputs = {out("length", T::Float)}},
        .exec =
            [](ExecContext& c) {
                double l = 0.0;
                if (GeometryPtr g = c.geometry("curve"); g->curves)
                {
                    CurveSet e = g->curves->evaluated();
                    for (size_t cu = 0; cu < e.numCurves(); ++cu)
                        l += polyLength(e.curvePoints(cu), e.cyclic[cu] != 0);
                }
                c.out("length", constantFloat(l));
            },
    });

    registerDecl({
        .type = "CurveToMesh",
        .label = "Curve to Mesh",
        .category = "Curve",
        .doc = "Sweeps the profile curve along each curve (scaled by the \"radius\" attribute). Without a profile, gives edges. The "
               "profile's X and Z run across the path, so Curve Circle makes a tube.",
        .sockets = {.inputs = {geoIn("curve"), geoIn("profileCurve"), in("fillCaps", T::Bool, false)}, .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("curve"), prof = c.geometry("profileCurve");
                if (!g->curves)
                {
                    c.out("mesh", makeGeometry({}));
                    return;
                }
                const CurveSet cs = g->curves->evaluated();
                const CurveSet profile = prof->curves ? prof->curves->evaluated() : CurveSet();
                std::vector<double> radius = evaluateAll<double>(radiusField(), FieldContext::forCurves(cs, Domain::Point));
                bool caps = c.b("fillCaps");
                geo::Mesh m;
                auto uv = [&]() { return m.cornerAttrs().getOrAdd<glm::vec2>("uv0"); };
                for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                {
                    std::span<const D3> path = cs.curvePoints(cu);
                    bool pathCyc = cs.cyclic[cu] != 0;
                    size_t n = path.size();
                    if (n < 2)
                        continue;
                    if (profile.numCurves() == 0)
                    {
                        Index base = m.pointCapacity();
                        for (const D3& p : path)
                            m.addPoint(p);
                        for (size_t k = 0; k + 1 < n; ++k)
                            m.addEdge(base + Index(k), base + Index(k + 1));
                        if (pathCyc)
                            m.addEdge(base + Index(n - 1), base);
                        continue;
                    }
                    std::vector<D3> tan, nrm;
                    curveFrames(path, pathCyc, tan, nrm);
                    for (size_t pc = 0; pc < profile.numCurves(); ++pc)
                    {
                        std::span<const D3> pr = profile.curvePoints(pc);
                        bool profCyc = profile.cyclic[pc] != 0;
                        size_t np = pr.size();
                        if (np < 2)
                            continue;
                        Index base = m.pointCapacity();
                        for (size_t k = 0; k < n; ++k)
                        {
                            D3 b = glm::cross(tan[k], nrm[k]);
                            double r = radius[cs.offsets[cu] + k];
                            for (const D3& q : pr)
                                m.addPoint(path[k] + (nrm[k] * q.x + b * q.z + tan[k] * q.y) * r);
                        }
                        size_t rings = pathCyc ? n : n - 1, around = profCyc ? np : np - 1;
                        for (size_t k = 0; k < rings; ++k)
                            for (size_t j = 0; j < around; ++j)
                            {
                                size_t k1 = (k + 1) % n, j1 = (j + 1) % np;
                                Index q[4] = {base + Index(k * np + j), base + Index(k1 * np + j), base + Index(k1 * np + j1), base + Index(k * np + j1)};
                                Index f = m.addFace(q);
                                if (f == geo::kInvalid)
                                    continue;
                                auto t = uv();
                                double u0 = double(j) / around, u1 = double(j + 1) / around, v0 = double(k) / rings, v1 = double(k + 1) / rings;
                                glm::vec2 uvs[4] = {{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}};
                                int i = 0;
                                m.forEachFaceCorner(f, [&](Index cc) { t[cc] = uvs[i++ % 4]; });
                            }
                        if (caps && profCyc && !pathCyc)
                        {
                            std::vector<Index> start, end;
                            for (size_t j = 0; j < np; ++j)
                            {
                                start.push_back(base + Index(np - 1 - j));
                                end.push_back(base + Index((n - 1) * np + j));
                            }
                            m.addFace(start);
                            m.addFace(end);
                        }
                    }
                }
                c.out("mesh", meshGeometry(std::move(m)));
            },
    });

    registerDecl({
        .type = "FillCurve",
        .label = "Fill Curve",
        .category = "Curve",
        .doc = "Fills each curve with a face (open curves are closed first).",
        .props = {prop("mode", {"ngons", "triangles"}, "ngons")},
        .sockets = {.inputs = {geoIn("curve")}, .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("curve");
                geo::Mesh m;
                CurveSet e = g->curves ? g->curves->evaluated() : CurveSet();
                if (g->curves)
                    for (size_t cu = 0; cu < e.numCurves(); ++cu)
                    {
                        std::span<const D3> pts = e.curvePoints(cu);
                        if (pts.size() < 3)
                            continue;
                        std::vector<Index> ids;
                        for (const D3& p : pts)
                            ids.push_back(m.addPoint(p));
                        m.addFace(ids);
                    }
                if (c.prop("mode") == "triangles")
                {
                    std::vector<Index> all;
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        if (m.faceValid(f))
                            all.push_back(f);
                    geo::triangulateFaces(m, all);
                }
                c.out("mesh", meshGeometry(std::move(m)));
            },
    });

    registerDecl({
        .type = "MeshToCurve",
        .label = "Mesh to Curve",
        .category = "Curve",
        .doc = "Chains the selected edges into poly curves.",
        .sockets = {.inputs = {geoIn("mesh"), fin("selection", T::Bool, true)}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("mesh");
                CurveSet cs;
                if (g->mesh)
                {
                    const geo::Mesh& m = *g->mesh;
                    std::vector<Index> edges = selected(c.field("selection"), FieldContext::forMesh(m, Domain::Edge));
                    std::vector<uint8_t> used(m.edgeCapacity(), 0);
                    for (Index e : edges)
                        used[e] = 1;
                    std::vector<uint8_t> done(m.edgeCapacity(), 0);
                    auto degree = [&](Index p) {
                        int d = 0;
                        m.forEachPointEdge(p, [&](Index e) { d += used[e] ? 1 : 0; });
                        return d;
                    };
                    auto walk = [&](Index startPoint, Index startEdge) {
                        std::vector<D3> pts{m.position(startPoint)};
                        Index p = startPoint, e = startEdge;
                        bool closed = false;
                        while (e != geo::kInvalid && !done[e])
                        {
                            done[e] = 1;
                            p = m.edgeOtherPoint(e, p);
                            if (p == startPoint)
                            {
                                closed = true;
                                break;
                            }
                            pts.push_back(m.position(p));
                            Index next = geo::kInvalid;
                            if (degree(p) == 2)
                                m.forEachPointEdge(p, [&](Index x) {
                                    if (used[x] && !done[x])
                                        next = x;
                                });
                            e = next;
                        }
                        cs.addCurve(pts, closed);
                    };
                    // Open chains start at ends and branch points; whatever is left is closed loops.
                    for (Index p = 0; p < m.pointCapacity(); ++p)
                        if (degree(p) != 2)
                            m.forEachPointEdge(p, [&](Index e) {
                                if (used[e] && !done[e])
                                    walk(p, e);
                            });
                    for (Index e : edges)
                        if (!done[e])
                            walk(m.edgePoint(e, 0), e);
                }
                c.out("curve", curveGeometry(std::move(cs)));
            },
    });
}

void splineNodes()
{
    registerDecl({
        .type = "SetSplineType",
        .label = "Set Spline Type",
        .category = "Curve",
        .doc = "Converts curves to poly, Bezier or NURBS, keeping their shape (fitted where it can't be exact).",
        .props = {prop("type", {"poly", "bezier", "nurbs"}, "bezier")},
        .sockets = {.inputs = {geoIn("curve")}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                curve::Kind k = curve::Kind::Bezier;
                curve::parseKind(c.prop("type"), &k);
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        CurveSet out;
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                            out.addSpline(curve::convert(cs.spline(cu), k));
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });

    registerDecl({
        .type = "SetHandleType",
        .label = "Set Handle Type",
        .category = "Curve",
        .doc = "Sets the handle type of every Bezier point: auto, vector, aligned or free.",
        .props = {prop("type", {"auto", "vector", "aligned", "free"}, "auto")},
        .sockets = {.inputs = {geoIn("curve")}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                curve::HandleType t = curve::HandleType::Auto;
                curve::parseHandleType(c.prop("type"), &t);
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        CurveSet out;
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                        {
                                                                            curve::Spline sp = cs.spline(cu);
                                                                            for (size_t i = 0; i < sp.size(); ++i)
                                                                                curve::setHandleType(sp, i, t);
                                                                            out.addSpline(sp);
                                                                        }
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });

    registerDecl({
        .type = "FilletCurve",
        .label = "Fillet Curve",
        .category = "Curve",
        .doc = "Rounds (arc), cuts (chamfer) or blends (g2) curve corners by radius; a point's cornerRadius attribute wins where set.",
        .props = {prop("mode", {"arc", "chamfer", "g2"}, "arc")},
        .sockets = {.inputs = {geoIn("curve"), bounded(in("radius", T::Float, 10.0, "cm."), 0.0)}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                curve::FilletOptions o;
                curve::parseCornerMode(c.prop("mode"), &o.mode);
                o.radius = std::max(0.0, c.f("radius"));
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        CurveSet out;
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                            out.addSpline(curve::fillet(cs.spline(cu), o));
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });
}
const std::vector<std::string> kAlongAxes = {"+x", "-x", "+y", "-y", "+z", "-z"};

curve::AlongAxis alongAxisOf(const std::string& s)
{
    auto it = std::ranges::find(kAlongAxes, s);
    return curve::AlongAxis(it == kAlongAxes.end() ? 0 : it - kAlongAxes.begin());
}

/** The rail along the set's first curve (curves after the first are ignored). */
curve::Rail firstRail(const GeometryPtr& g, const std::string& upMode)
{
    if (!g->curves || g->curves->numCurves() == 0)
        return {};
    curve::RailOptions ro;
    ro.up = upMode == "autoNormals" ? curve::UpMode::AutoNormals : curve::UpMode::CurveFrames;
    return curve::Rail(g->curves->spline(0), ro);
}

void alongNodes()
{
    registerDecl({
        .type = "DeformAlongCurve",
        .label = "Deform Along Curve",
        .category = "Curve",
        .doc = "Bends the mesh along the first curve: its forward axis runs along the curve from the start, the cross-section rides the "
               "curve's frame (tilt and radius included). Stretch fits the mesh to the curve's length.",
        .props = {prop("forwardAxis", kAlongAxes, "+x"), prop("upMode", {"curveFrames", "autoNormals"}, "curveFrames")},
        .sockets = {.inputs = {geoIn("geometry"), geoIn("curve"), in("offset", T::Float, 0.0, "Where the mesh starts along the curve, cm."),
                               in("stretch", T::Bool, false), in("twist", T::Float, 0.0, "Twist from start to end, radians."),
                               in("roll", T::Float, 0.0, "Turn about the curve, radians.")},
                    .outputs = {geoOut("geometry")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("geometry");
                curve::Rail rail = firstRail(c.geometry("curve"), c.prop("upMode"));
                if (!rail.valid() || !g->mesh)
                {
                    c.out("geometry", g);
                    return;
                }
                curve::DeformAlongOptions o;
                o.forward = alongAxisOf(c.prop("forwardAxis"));
                o.offset = c.f("offset");
                o.stretch = c.b("stretch");
                o.roll = c.f("roll");
                double twist = c.f("twist");
                if (twist != 0.0)
                    o.twist = [twist](double t) { return twist * t; };
                c.out("geometry", editComponents(g, {.mesh = [&](geo::Mesh& m) { curve::deformAlongCurve(m, rail, o); }}));
            },
    });

    registerDecl({
        .type = "InstanceOnCurve",
        .label = "Instance on Curve",
        .category = "Instances",
        .doc = "Places copies of the instance geometry along the first curve, by count or by spacing (0 spacing is the instance's own "
               "length along the forward axis), each turned onto the curve's frame.",
        .props = {prop("fill", {"count", "spacing"}, "count"), prop("forwardAxis", kAlongAxes, "+x"),
                  prop("upMode", {"curveFrames", "autoNormals"}, "curveFrames")},
        .sockets = {.inputs = {geoIn("curve"), geoIn("instance"), bounded(in("count", T::Int, 5), 1, 100000), bounded(in("spacing", T::Float, 0.0, "Step between copies, cm."), 0.0),
                               in("fitToLength", T::Bool, true, "Spacing: even the step so copies run end to end."),
                               bounded(in("startOffset", T::Float, 0.0, "cm."), 0.0), bounded(in("endOffset", T::Float, 0.0, "cm."), 0.0), in("align", T::Bool, true)},
                    .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr inst = c.geometry("instance");
                curve::Rail rail = firstRail(c.geometry("curve"), c.prop("upMode"));
                Instances out;
                out.references.push_back(inst);
                if (rail.valid())
                {
                    curve::ArrayAlongOptions o;
                    o.fill = c.prop("fill") == "spacing" ? curve::ArrayFill::Spacing : curve::ArrayFill::Count;
                    o.count = int(std::clamp<int64_t>(c.i("count"), 1, 100000));
                    o.spacing = std::max(0.0, c.f("spacing"));
                    o.fitToLength = c.b("fitToLength");
                    o.startOffset = std::max(0.0, c.f("startOffset"));
                    o.endOffset = std::max(0.0, c.f("endOffset"));
                    o.forward = alongAxisOf(c.prop("forwardAxis"));
                    o.align = c.b("align");
                    auto [lo, hi] = inst->mesh ? curve::forwardExtent(*inst->mesh, o.forward) : std::pair{0.0, 0.0};
                    try
                    {
                        for (const curve::ArrayPlacement& p : curve::arrayPlacements(rail, o, hi - lo))
                            out.add(p.transform, 0);
                    }
                    catch (const std::invalid_argument&)
                    {
                        // Nothing fits (no room, or a flat instance with no spacing): no instances.
                    }
                }
                Geometry g;
                g.instances = std::make_shared<const Instances>(std::move(out));
                c.out("instances", makeGeometry(std::move(g)));
            },
    });
}
} // namespace

void registerCurveNodes()
{
    splineNodes();
    primitives();
    curveOps();
    alongNodes();
}
} // namespace lila::gn::nodes
