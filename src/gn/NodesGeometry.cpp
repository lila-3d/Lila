#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "geo/Ops.h"
#include "geo/Primitives.h"

#include <algorithm>
#include <numeric>

namespace lila::gn::nodes
{
namespace
{
using D3 = glm::dvec3;

std::vector<uint8_t> maskOf(const FieldPtr& sel, const FieldContext& ctx, bool invert)
{
    std::vector<uint8_t> m = expand<uint8_t>(evaluate(sel, ctx, T::Bool), ctx.size);
    if (invert)
        for (uint8_t& v : m)
            v = v ? 0 : 1;
    return m;
}

std::vector<Index> indicesOf(const std::vector<uint8_t>& mask)
{
    std::vector<Index> out;
    for (size_t k = 0; k < mask.size(); ++k)
        if (mask[k])
            out.push_back(Index(k));
    return out;
}

geo::Mesh keepPoints(const geo::Mesh& m, const std::vector<uint8_t>& keep)
{
    geo::Mesh out;
    out.pointAttrs().mergeLayout(m.pointAttrs());
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && (p >= keep.size() || keep[p]))
        {
            Index np = out.addPoint(m.position(p));
            out.pointAttrs().copyFrom(m.pointAttrs(), np, p);
        }
    return out;
}

CurveSet keepCurves(const CurveSet& c, const std::vector<uint8_t>* keepCurve, const std::vector<uint8_t>* keepPoint)
{
    CurveSet out;
    out.pointAttrs.mergeLayout(c.pointAttrs);
    out.curveAttrs.mergeLayout(c.curveAttrs);
    for (size_t cu = 0; cu < c.numCurves(); ++cu)
    {
        if (keepCurve && !(*keepCurve)[cu])
            continue;
        std::vector<Index> pts;
        for (Index p = c.offsets[cu]; p < c.offsets[cu + 1]; ++p)
            if (!keepPoint || (*keepPoint)[p])
                pts.push_back(p);
        if (pts.empty())
            continue;
        size_t first = out.numPoints();
        for (Index p : pts)
            out.positions.push_back(c.positions[p]);
        out.offsets.push_back(Index(out.positions.size()));
        out.cyclic.push_back(c.cyclic[cu]);
        out.syncSizes();
        for (size_t k = 0; k < pts.size(); ++k)
            out.pointAttrs.copyFrom(c.pointAttrs, first + k, pts[k]);
        out.curveAttrs.copyFrom(c.curveAttrs, out.numCurves() - 1, cu);
    }
    return out;
}

Instances keepInstances(const Instances& in, const std::vector<uint8_t>& keep)
{
    Instances out;
    out.references = in.references;
    out.attrs.mergeLayout(in.attrs);
    for (size_t k = 0; k < in.size(); ++k)
        if (keep[k])
        {
            out.add(in.transforms[k], in.reference[k]);
            out.attrs.copyFrom(in.attrs, out.size() - 1, k);
        }
    return out;
}

/** Removes the selected elements (or the unselected ones with invert) on a domain, from every component that has it. */
GeometryPtr deleteSelected(const GeometryPtr& g, const FieldPtr& sel, Domain d, bool invert)
{
    LILA_ZONE();

    ComponentEdits e;
    if (d <= Domain::Corner)
    {
        e.mesh = [&](geo::Mesh& m) {
            Domain dd = d == Domain::Corner ? Domain::Face : d;
            FieldContext ctx = FieldContext::forMesh(m, dd);
            std::vector<Index> ids = indicesOf(maskOf(sel, ctx, invert));
            if (dd == Domain::Point)
                geo::deletePoints(m, ids);
            else if (dd == Domain::Edge)
                geo::deleteEdges(m, ids);
            else
                geo::deleteFaces(m, ids);
        };
        e.points = [&](geo::Mesh& m) {
            std::vector<uint8_t> drop = maskOf(sel, FieldContext::forPoints(m), invert);
            for (uint8_t& v : drop)
                v = v ? 0 : 1;
            m = keepPoints(m, drop);
        };
    }
    if (d == Domain::Point || d == Domain::Curve)
        e.curves = [&](CurveSet& c) {
            std::vector<uint8_t> drop = maskOf(sel, FieldContext::forCurves(c, d), invert);
            for (uint8_t& v : drop)
                v = v ? 0 : 1;
            c = d == Domain::Curve ? keepCurves(c, &drop, nullptr) : keepCurves(c, nullptr, &drop);
        };
    if (d == Domain::Instance)
        e.instances = [&](Instances& i) {
            std::vector<uint8_t> drop = maskOf(sel, FieldContext::forInstances(i), invert);
            for (uint8_t& v : drop)
                v = v ? 0 : 1;
            i = keepInstances(i, drop);
        };
    return editComponents(g, e);
}

const std::vector<std::string> kDeleteDomains = {"point", "edge", "face", "curve", "instance"};

void deleteAndSeparate()
{
    registerDecl({
        .type = "DeleteGeometry",
        .label = "Delete Geometry",
        .category = "Geometry",
        .doc = "Removes the selected elements. Deleting points or edges also removes the faces using them.",
        .props = {prop("domain", kDeleteDomains, "point")},
        .sockets = {.inputs = {geoIn(), selectionIn()}, .outputs = {geoOut()}},
        .exec = [](ExecContext& c) { c.out("geometry", deleteSelected(c.geometry("geometry"), c.field("selection"), domainOf(c.prop("domain")), false)); },
    });
    registerDecl({
        .type = "SeparateGeometry",
        .label = "Separate Geometry",
        .category = "Geometry",
        .doc = "Splits geometry into the selected part and the rest.",
        .props = {prop("domain", kDeleteDomains, "point")},
        .sockets = {.inputs = {geoIn(), selectionIn()}, .outputs = {geoOut("selection"), geoOut("inverted")}},
        .exec =
            [](ExecContext& c) {
                Domain d = domainOf(c.prop("domain"));
                GeometryPtr g = c.geometry("geometry");
                c.out("selection", deleteSelected(g, c.field("selection"), d, true));
                c.out("inverted", deleteSelected(g, c.field("selection"), d, false));
            },
    });
}

void transformAndJoin()
{
    registerDecl({
        .type = "TransformGeometry",
        .label = "Transform Geometry",
        .category = "Geometry",
        .doc = "Moves, rotates and scales all of the geometry.",
        .sockets = {.inputs = {geoIn(), in("translation", T::Vector, vec(0, 0, 0)), in("rotation", T::Rotation, vec(0, 0, 0)),
                               in("scale", T::Vector, vec(1, 1, 1))},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                glm::dmat4 m = composeTransform(c.v("translation"), c.v("rotation"), c.v("scale"));
                c.out("geometry", makeGeometry(transformed(*c.geometry("geometry"), m)));
            },
    });
    registerDecl({
        .type = "JoinGeometry",
        .label = "Join Geometry",
        .category = "Geometry",
        .doc = "Merges every connected geometry into one, component by component.",
        .sockets = {.inputs = {multiIn()}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                std::vector<GeometryPtr> parts = c.geometries("geometry");
                c.out("geometry", parts.size() == 1 ? parts[0] : makeGeometry(join(parts)));
            },
    });
    registerDecl({
        .type = "RealizeInstances",
        .label = "Realize Instances",
        .category = "Instances",
        .doc = "Turns instances into real geometry.",
        .sockets = {.inputs = {geoIn()}, .outputs = {geoOut()}},
        .exec = [](ExecContext& c) { c.out("geometry", makeGeometry(realize(*c.geometry("geometry")))); },
    });
}

void setPosition()
{
    registerDecl({
        .type = "SetPosition",
        .label = "Set Position",
        .category = "Geometry",
        .doc = "Moves points (and instances) to a position plus an offset.",
        .sockets = {.inputs = {geoIn(), selectionIn(), fin("position", T::Vector, nullptr, Implicit::Position), fin("offset", T::Vector, vec(0, 0, 0))},
                    .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                FieldPtr sel = c.field("selection"), pos = c.field("position"), off = c.field("offset");
                auto apply = [&](const FieldContext& ctx, auto&& set) {
                    std::vector<uint8_t> s = expand<uint8_t>(evaluate(sel, ctx, T::Bool), ctx.size);
                    std::vector<D3> p = evaluateAll<D3>(pos, ctx), o = evaluateAll<D3>(off, ctx);
                    for (size_t k = 0; k < ctx.size; ++k)
                        if (s[k])
                            set(k, p[k] + o[k]);
                };
                auto meshEdit = [&](geo::Mesh& m, bool cloud) {
                    FieldContext ctx = cloud ? FieldContext::forPoints(m) : FieldContext::forMesh(m, Domain::Point);
                    apply(ctx, [&](size_t k, D3 v) { m.setPosition(Index(k), v); });
                };
                c.out("geometry", editComponents(c.geometry("geometry"),
                                                 {.mesh = [&](geo::Mesh& m) { meshEdit(m, false); },
                                                  .points = [&](geo::Mesh& m) { meshEdit(m, true); },
                                                  .curves =
                                                      [&](CurveSet& cs) {
                                                          FieldContext ctx = FieldContext::forCurves(cs, Domain::Point);
                                                          std::vector<D3> next = cs.positions;
                                                          apply(ctx, [&](size_t k, D3 v) { next[k] = v; });
                                                          cs.positions = std::move(next);
                                                      },
                                                  .instances =
                                                      [&](Instances& in) {
                                                          FieldContext ctx = FieldContext::forInstances(in);
                                                          std::vector<D3> next(in.size());
                                                          std::vector<uint8_t> moved(in.size(), 0);
                                                          apply(ctx, [&](size_t k, D3 v) { next[k] = v; moved[k] = 1; });
                                                          for (size_t k = 0; k < in.size(); ++k)
                                                              if (moved[k])
                                                                  in.transforms[k][3] = glm::dvec4(next[k], 1.0);
                                                      }}));
            },
    });
}

void setRadius()
{
    registerDecl({
        .type = "SetRadius",
        .label = "Set Radius",
        .category = "Geometry",
        .doc = "Sets the \"radius\" attribute of points and curve points (curve radius scales the profile in Curve to Mesh).",
        .sockets = {.inputs = {geoIn(), selectionIn(), fin("radius", T::Float, 1.0)}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                FieldPtr sel = c.field("selection"), rad = c.field("radius");
                auto write = [&](const FieldContext& ctx, auto& target) {
                    std::vector<uint8_t> s = expand<uint8_t>(evaluate(sel, ctx, T::Bool), ctx.size);
                    std::vector<double> r = evaluateAll<double>(rad, ctx), old = evaluateAll<double>(radiusField(), ctx);
                    for (size_t k = 0; k < ctx.size; ++k)
                        if (!s[k])
                            r[k] = old[k];
                    writeAttribute(target, "radius", Domain::Point, Column::of(std::move(r)), T::Float);
                };
                c.out("geometry", editComponents(c.geometry("geometry"), {.points = [&](geo::Mesh& m) { write(FieldContext::forPoints(m), m); },
                                                                         .curves = [&](CurveSet& cs) { write(FieldContext::forCurves(cs, Domain::Point), cs); }}));
            },
    });
}

void duplicate()
{
    registerDecl({
        .type = "DuplicateElements",
        .label = "Duplicate Elements",
        .category = "Geometry",
        .doc = "Copies each selected element amount times; faces become separate pieces. Duplicate Index counts the copies from 0.",
        .props = {prop("domain", {"point", "face", "curve", "instance"}, "point")},
        .sockets = {.inputs = {geoIn(), selectionIn(), fin("amount", T::Int, 1)}, .outputs = {geoOut(), fout("duplicateIndex", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                Domain d = domainOf(c.prop("domain"));
                GeometryPtr g = c.geometry("geometry");
                std::string dupName = c.anonymous("duplicateIndex");
                FieldPtr sel = c.field("selection"), amount = c.field("amount");
                auto counts = [&](const FieldContext& ctx) {
                    std::vector<uint8_t> s = expand<uint8_t>(evaluate(sel, ctx, T::Bool), ctx.size);
                    std::vector<int64_t> a = evaluateAll<int64_t>(amount, ctx);
                    for (size_t k = 0; k < ctx.size; ++k)
                        a[k] = s[k] ? std::clamp<int64_t>(a[k], 0, 100000) : 0;
                    return a;
                };
                Geometry out;
                if (d == Domain::Face && g->mesh)
                {
                    const geo::Mesh& m = *g->mesh;
                    std::vector<int64_t> n = counts(FieldContext::forMesh(m, Domain::Face));
                    std::vector<glm::dvec3> pts;
                    std::vector<Index> sizes, idx, srcPoint, srcFace, srcCorner;
                    std::vector<int32_t> dupOfFace;
                    for (Index f = 0; f < m.faceCapacity(); ++f)
                        for (int64_t k = 0; k < n[f]; ++k)
                        {
                            m.forEachFaceCorner(f, [&](Index cc) {
                                idx.push_back(Index(pts.size()));
                                srcPoint.push_back(m.cornerPoint(cc));
                                srcCorner.push_back(cc);
                                pts.push_back(m.position(m.cornerPoint(cc)));
                            });
                            sizes.push_back(m.faceSize(f));
                            srcFace.push_back(f);
                            dupOfFace.push_back(int32_t(k));
                        }
                    geo::Mesh r = geo::Mesh::fromPolygons(pts, sizes, idx);
                    for (int dom = 0; dom < 4; ++dom)
                        r.attributes(geo::Domain(dom)).mergeLayout(m.attributes(geo::Domain(dom)));
                    for (Index p = 0; p < r.pointCapacity(); ++p)
                        r.pointAttrs().copyFrom(m.pointAttrs(), p, srcPoint[p]);
                    r.faceAttrs().add(dupName, geo::AttrType::Int);
                    for (Index f = 0; f < r.faceCapacity(); ++f)
                    {
                        r.faceAttrs().copyFrom(m.faceAttrs(), f, srcFace[f]);
                        r.faceAttrs().get<int32_t>(dupName)[f] = dupOfFace[f];
                    }
                    // Faces were built in order, so their corners follow the source corners in sequence.
                    Index running = 0;
                    for (Index f = 0; f < r.faceCapacity(); ++f)
                    {
                        Index k = 0;
                        r.forEachFaceCorner(f, [&](Index cc) { r.cornerAttrs().copyFrom(m.cornerAttrs(), cc, srcCorner[running + k++]); });
                        running += r.faceSize(f);
                    }
                    r.compact();
                    out.mesh = std::make_shared<const geo::Mesh>(std::move(r));
                }
                else if (d == Domain::Point && (g->points || g->mesh))
                {
                    const geo::Mesh& m = g->points ? *g->points : *g->mesh;
                    std::vector<int64_t> n = counts(g->points ? FieldContext::forPoints(m) : FieldContext::forMesh(m, Domain::Point));
                    geo::Mesh r;
                    r.pointAttrs().mergeLayout(m.pointAttrs());
                    std::vector<int32_t> dupIdx;
                    for (Index p = 0; p < m.pointCapacity(); ++p)
                        for (int64_t k = 0; k < n[p]; ++k)
                        {
                            Index np = r.addPoint(m.position(p));
                            r.pointAttrs().copyFrom(m.pointAttrs(), np, p);
                            dupIdx.push_back(int32_t(k));
                        }
                    auto dup = r.pointAttrs().getOrAdd<int32_t>(dupName);
                    std::copy(dupIdx.begin(), dupIdx.end(), dup.begin());
                    out.points = std::make_shared<const geo::Mesh>(std::move(r));
                }
                else if (d == Domain::Curve && g->curves)
                {
                    const CurveSet& cs = *g->curves;
                    std::vector<int64_t> n = counts(FieldContext::forCurves(cs, Domain::Curve));
                    CurveSet r;
                    std::vector<int32_t> dupIdx;
                    for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                        for (int64_t k = 0; k < n[cu]; ++k)
                        {
                            std::vector<uint8_t> only(cs.numCurves(), 0);
                            only[cu] = 1;
                            r.append(keepCurves(cs, &only, nullptr));
                            dupIdx.push_back(int32_t(k));
                        }
                    auto dup = r.curveAttrs.getOrAdd<int32_t>(dupName);
                    std::copy(dupIdx.begin(), dupIdx.end(), dup.begin());
                    out.curves = std::make_shared<const CurveSet>(std::move(r));
                }
                else if (d == Domain::Instance && g->instances)
                {
                    const Instances& in = *g->instances;
                    std::vector<int64_t> n = counts(FieldContext::forInstances(in));
                    Instances r;
                    r.references = in.references;
                    r.attrs.mergeLayout(in.attrs);
                    std::vector<int32_t> dupIdx;
                    for (size_t i = 0; i < in.size(); ++i)
                        for (int64_t k = 0; k < n[i]; ++k)
                        {
                            r.add(in.transforms[i], in.reference[i]);
                            r.attrs.copyFrom(in.attrs, r.size() - 1, i);
                            dupIdx.push_back(int32_t(k));
                        }
                    auto dup = r.attrs.getOrAdd<int32_t>(dupName);
                    std::copy(dupIdx.begin(), dupIdx.end(), dup.begin());
                    out.instances = std::make_shared<const Instances>(std::move(r));
                }
                c.out("geometry", makeGeometry(std::move(out)));
                c.out("duplicateIndex", attributeField(dupName, T::Int));
            },
    });
}

/** Points to instance on: point clouds first, then mesh vertices, then curve points. */
struct PointSource
{
    FieldContext ctx;
    const geo::AttributeSet* attrs = nullptr;
};

std::vector<PointSource> pointSources(const Geometry& g)
{
    std::vector<PointSource> out;
    if (g.points)
        out.push_back({FieldContext::forPoints(*g.points), &g.points->pointAttrs()});
    if (g.mesh)
        out.push_back({FieldContext::forMesh(*g.mesh, Domain::Point), &g.mesh->pointAttrs()});
    if (g.curves)
        out.push_back({FieldContext::forCurves(*g.curves, Domain::Point), &g.curves->pointAttrs});
    return out;
}

void instanceOnPoints()
{
    registerDecl({
        .type = "InstanceOnPoints",
        .label = "Instance on Points",
        .category = "Instances",
        .doc = "Places a copy of the instance geometry on every selected point. Point attributes carry over to the instances.",
        .sockets = {.inputs = {geoIn("points"), selectionIn(), geoIn("instance"), in("pickInstance", T::Bool, false, "Pick one of the instance geometry's own instances per point."),
                               fin("instanceIndex", T::Int, nullptr, Implicit::Id), fin("rotation", T::Rotation, vec(0, 0, 0)), fin("scale", T::Vector, vec(1, 1, 1))},
                    .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr pts = c.geometry("points"), inst = c.geometry("instance");
                bool pick = c.b("pickInstance") && inst->instances && inst->instances->size() > 0;
                Instances out;
                if (!pick)
                    out.references.push_back(inst);
                else
                    for (size_t k = 0; k < inst->instances->size(); ++k)
                    {
                        int r = inst->instances->reference[k];
                        GeometryPtr ref = r >= 0 && size_t(r) < inst->instances->references.size() ? inst->instances->references[r] : nullptr;
                        out.references.push_back(ref ? makeGeometry(transformed(*ref, inst->instances->transforms[k])) : makeGeometry({}));
                    }
                for (const PointSource& src : pointSources(*pts))
                {
                    const FieldContext& ctx = src.ctx;
                    std::vector<uint8_t> sel = expand<uint8_t>(evaluate(c.field("selection"), ctx, T::Bool), ctx.size);
                    std::vector<D3> pos = evaluateAll<D3>(positionField(), ctx);
                    std::vector<D3> rot = expand<D3>(evaluate(c.field("rotation"), ctx, T::Rotation), ctx.size);
                    std::vector<D3> scl = evaluateAll<D3>(c.field("scale"), ctx);
                    std::vector<int64_t> pickIdx = pick ? evaluateAll<int64_t>(c.field("instanceIndex"), ctx) : std::vector<int64_t>();
                    out.attrs.mergeLayout(*src.attrs);
                    for (size_t k = 0; k < ctx.size; ++k)
                    {
                        if (!sel[k])
                            continue;
                        int ref = 0;
                        if (pick)
                        {
                            int64_t n = int64_t(out.references.size());
                            ref = int(((pickIdx[k] % n) + n) % n);
                        }
                        out.add(composeTransform(pos[k], rot[k], scl[k]), ref);
                        out.attrs.copyFrom(*src.attrs, out.size() - 1, k);
                    }
                }
                Geometry g;
                // Instances already in the input stay, the points are replaced.
                std::vector<GeometryPtr> parts;
                if (pts->instances)
                {
                    Geometry keep;
                    keep.instances = pts->instances;
                    parts.push_back(makeGeometry(keep));
                }
                g.instances = std::make_shared<const Instances>(std::move(out));
                parts.push_back(makeGeometry(g));
                c.out("instances", parts.size() == 1 ? parts[0] : makeGeometry(join(parts)));
            },
    });
}

/** Evaluates a field on a domain of each component that has it, and hands back the values. */
template <class Fn> void forDomain(const Geometry& g, Domain d, Fn&& fn)
{
    if (g.mesh && d <= Domain::Corner)
        fn(FieldContext::forMesh(*g.mesh, d));
    if (g.points && d == Domain::Point)
        fn(FieldContext::forPoints(*g.points));
    if (g.curves && (d == Domain::Point || d == Domain::Curve))
        fn(FieldContext::forCurves(*g.curves, d));
    if (g.instances && d == Domain::Instance)
        fn(FieldContext::forInstances(*g.instances));
}

/** Writes an attribute on the domain of every component that has it. Unselected elements keep their value. */
GeometryPtr storeAttribute(const GeometryPtr& g, const std::string& name, Domain d, SocketType t, const FieldPtr& value, const FieldPtr& sel)
{
    auto values = [&](const FieldContext& ctx) {
        Column v = evaluate(value, ctx, t);
        if (sel && !(sel->constant() && evaluateSingle<uint8_t>(sel)))
        {
            std::vector<uint8_t> s = expand<uint8_t>(evaluate(sel, ctx, T::Bool), ctx.size);
            Column old = evaluate(attributeField(name, t), ctx, t);
            std::visit(
                [&](auto& nv) {
                    using V = typename std::decay_t<decltype(nv)>::value_type;
                    std::vector<V> full = expand<V>(v, ctx.size);
                    for (size_t k = 0; k < ctx.size; ++k)
                        if (!s[k])
                            full[k] = old.at<V>(k);
                    v = Column::of(std::move(full));
                },
                v.data);
        }
        return v;
    };
    ComponentEdits e;
    if (d <= Domain::Corner)
    {
        e.mesh = [&](geo::Mesh& m) { writeAttribute(m, name, d, values(FieldContext::forMesh(m, d)), t); };
        if (d == Domain::Point)
            e.points = [&](geo::Mesh& m) { writeAttribute(m, name, Domain::Point, values(FieldContext::forPoints(m)), t); };
    }
    if (d == Domain::Point || d == Domain::Curve)
        e.curves = [&](CurveSet& c) { writeAttribute(c, name, d, values(FieldContext::forCurves(c, d)), t); };
    if (d == Domain::Instance)
        e.instances = [&](Instances& i) { writeAttribute(i, name, values(FieldContext::forInstances(i)), t); };
    return editComponents(g, e);
}

void attributes()
{
    registerDecl({
        .type = "NamedAttribute",
        .label = "Named Attribute",
        .category = "Attribute",
        .doc = "Reads an attribute by name (uv0, color, material, or anything stored). \"position\" reads point positions.",
        .props = {prop("dataType", dataTypes(), "float")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("NamedAttribute")->propValue(n, "dataType"));
                return Sockets{{in("name", T::String, "")}, {fout("attribute", t), fout("exists", T::Bool)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                c.out("attribute", attributeField(c.s("name"), t));
                c.out("exists", attributeExistsField(c.s("name")));
            },
    });
    registerDecl({
        .type = "StoreNamedAttribute",
        .label = "Store Named Attribute",
        .category = "Attribute",
        .doc = "Writes a field into a named attribute on a domain. Vector values written to an existing uv or colour layer keep its layout.",
        .props = {prop("dataType", dataTypes(), "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("StoreNamedAttribute")->propValue(n, "dataType"));
                json def = t == T::Vector || t == T::Rotation ? vec(0, 0, 0) : json(0);
                return Sockets{{geoIn(), selectionIn(), in("name", T::String, ""), fin("value", t, def)}, {geoOut()}};
            },
        .exec =
            [](ExecContext& c) {
                std::string name = c.s("name");
                if (name.empty())
                {
                    c.out("geometry", c.geometry("geometry"));
                    return;
                }
                SocketType t = dataTypeOf(c.prop("dataType"));
                c.out("geometry", storeAttribute(c.geometry("geometry"), name, domainOf(c.prop("domain")), t, c.field("value"), c.field("selection")));
            },
    });
    registerDecl({
        .type = "CaptureAttribute",
        .label = "Capture Attribute",
        .category = "Attribute",
        .doc = "Evaluates a field on this geometry now and keeps the result with its elements, so later changes to the geometry don't "
               "change it.",
        .props = {prop("dataType", dataTypes(), "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("CaptureAttribute")->propValue(n, "dataType"));
                json def = t == T::Vector || t == T::Rotation ? vec(0, 0, 0) : json(0);
                return Sockets{{geoIn(), fin("value", t, def)}, {geoOut(), fout("attribute", t)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                std::string name = c.anonymous("attribute");
                c.out("geometry", storeAttribute(c.geometry("geometry"), name, domainOf(c.prop("domain")), t, c.field("value"), nullptr));
                c.out("attribute", attributeField(name, t));
            },
    });
    registerDecl({
        .type = "AttributeStatistic",
        .label = "Attribute Statistic",
        .category = "Attribute",
        .doc = "Statistics of a field over the selected elements of a domain.",
        .props = {prop("dataType", {"float", "vector"}, "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("AttributeStatistic")->propValue(n, "dataType"));
                Sockets s{{geoIn(), selectionIn(), fin("attribute", t, t == T::Float ? json(0.0) : vec(0, 0, 0))}, {}};
                for (const char* o : {"mean", "median", "sum", "min", "max", "range", "standardDeviation", "variance"})
                    s.outputs.push_back(out(o, t));
                return s;
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                std::vector<D3> vals;
                forDomain(*c.geometry("geometry"), domainOf(c.prop("domain")), [&](const FieldContext& ctx) {
                    std::vector<uint8_t> s = expand<uint8_t>(evaluate(c.field("selection"), ctx, T::Bool), ctx.size);
                    std::vector<D3> v = evaluateAll<D3>(convertField(c.field("attribute"), T::Vector), ctx);
                    if (t == T::Float)
                    {
                        std::vector<double> f = evaluateAll<double>(c.field("attribute"), ctx);
                        for (size_t k = 0; k < f.size(); ++k)
                            v[k] = D3(f[k]);
                    }
                    for (size_t k = 0; k < ctx.size; ++k)
                        if (s[k])
                            vals.push_back(v[k]);
                });
                D3 mean(0.0), sum(0.0), mn(0.0), mx(0.0), median(0.0), var(0.0);
                if (!vals.empty())
                {
                    mn = mx = vals[0];
                    for (const D3& v : vals)
                    {
                        sum += v;
                        mn = glm::min(mn, v);
                        mx = glm::max(mx, v);
                    }
                    mean = sum / double(vals.size());
                    for (const D3& v : vals)
                        var += (v - mean) * (v - mean);
                    var /= double(vals.size());
                    for (int k = 0; k < 3; ++k)
                    {
                        std::vector<double> col(vals.size());
                        for (size_t i = 0; i < vals.size(); ++i)
                            col[i] = vals[i][k];
                        std::nth_element(col.begin(), col.begin() + col.size() / 2, col.end());
                        median[k] = col[col.size() / 2];
                    }
                }
                auto emit = [&](const char* id, D3 v) {
                    if (t == T::Float)
                        c.out(id, constantFloat(v.x));
                    else
                        c.out(id, constantVector(v));
                };
                emit("mean", mean);
                emit("median", median);
                emit("sum", sum);
                emit("min", mn);
                emit("max", mx);
                emit("range", mx - mn);
                emit("standardDeviation", glm::sqrt(var));
                emit("variance", var);
            },
    });
}

void info()
{
    registerDecl({
        .type = "BoundingBox",
        .label = "Bounding Box",
        .category = "Geometry",
        .doc = "A box mesh around everything, instances included.",
        .sockets = {.inputs = {geoIn()}, .outputs = {geoOut("boundingBox"), out("min", T::Vector), out("max", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                Geometry r = realize(*c.geometry("geometry"));
                geo::Bounds b;
                if (r.mesh)
                    b.extend(r.mesh->bounds());
                if (r.points)
                    b.extend(r.points->bounds());
                if (r.curves)
                    for (const D3& p : r.curves->positions)
                        b.extend(p);
                if (!b.valid())
                    return;
                D3 size = b.size();
                geo::Mesh box = geo::makeCube({std::max(size.x, 1e-6), std::max(size.y, 1e-6), std::max(size.z, 1e-6)});
                glm::dmat4 xf(1.0);
                xf[3] = glm::dvec4(b.center(), 1.0);
                transformMesh(box, xf);
                c.out("boundingBox", meshGeometry(std::move(box)));
                c.out("min", constantVector(b.min));
                c.out("max", constantVector(b.max));
            },
    });
    registerDecl({
        .type = "DomainSize",
        .label = "Domain Size",
        .category = "Attribute",
        .doc = "Element counts of a component.",
        .props = {prop("component", {"mesh", "points", "curve", "instances"}, "mesh")},
        .sockets = {.inputs = {geoIn()},
                    .outputs = {out("pointCount", T::Int), out("edgeCount", T::Int), out("faceCount", T::Int), out("faceCornerCount", T::Int),
                                out("curveCount", T::Int), out("instanceCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("geometry");
                std::string comp = c.prop("component");
                int64_t p = 0, e = 0, f = 0, cc = 0, cu = 0, in = 0;
                if (comp == "mesh" && g->mesh)
                {
                    p = g->mesh->numPoints();
                    e = g->mesh->numEdges();
                    f = g->mesh->numFaces();
                    cc = g->mesh->numCorners();
                }
                else if (comp == "points" && g->points)
                    p = g->points->numPoints();
                else if (comp == "curve" && g->curves)
                {
                    p = int64_t(g->curves->numPoints());
                    cu = int64_t(g->curves->numCurves());
                }
                else if (comp == "instances" && g->instances)
                    in = int64_t(g->instances->size());
                c.out("pointCount", constantInt(p));
                c.out("edgeCount", constantInt(e));
                c.out("faceCount", constantInt(f));
                c.out("faceCornerCount", constantInt(cc));
                c.out("curveCount", constantInt(cu));
                c.out("instanceCount", constantInt(in));
            },
    });
}
} // namespace

void registerGeometryNodes()
{
    deleteAndSeparate();
    transformAndJoin();
    setPosition();
    setRadius();
    duplicate();
    instanceOnPoints();
    attributes();
    info();
}
} // namespace lila::gn::nodes
