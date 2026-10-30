#include "gn/NodesCommon.h"

namespace lila::gn::nodes::instancenodes
{
using D3 = glm::dvec3;

D3 scaleOf(const glm::dmat4& m) { return {glm::length(D3(m[0])), glm::length(D3(m[1])), glm::length(D3(m[2]))}; }

D3 rotationOf(const glm::dmat4& m)
{
    D3 s = scaleOf(m);
    glm::dmat3 r(D3(m[0]) / std::max(s.x, 1e-12), D3(m[1]) / std::max(s.y, 1e-12), D3(m[2]) / std::max(s.z, 1e-12));
    return matrixToEuler(r);
}

glm::dmat4 translation(const D3& t)
{
    glm::dmat4 m(1.0);
    m[3] = glm::dvec4(t, 1.0);
    return m;
}

glm::dmat4 linear(const glm::dmat3& r)
{
    glm::dmat4 m(1.0);
    for (int c = 0; c < 3; ++c)
        m[c] = glm::dvec4(r[c], 0.0);
    return m;
}

/** A change about a pivot, in the instance's own space (local) or the world's. */
glm::dmat4 applyAbout(const glm::dmat4& xf, const glm::dmat3& change, const D3& pivot, bool local)
{
    glm::dmat4 about = translation(pivot) * linear(change) * translation(-pivot);
    return local ? xf * about : about * xf;
}

class InstanceTransformField final : public Field
{
public:
    explicit InstanceTransformField(bool scale) : Field(scale ? SocketType::Vector : SocketType::Rotation), mScale(scale) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Instances)
            return Column::of(mScale ? D3(1.0) : D3(0.0));
        std::vector<D3> v(ctx.instances->size());
        for (size_t k = 0; k < v.size(); ++k)
            v[k] = mScale ? scaleOf(ctx.instances->transforms[k]) : rotationOf(ctx.instances->transforms[k]);
        return Column::of(std::move(v));
    }

private:
    bool mScale;
};

/** Edits each selected instance's transform with fn(transform, index). */
template <class Fn> GeometryPtr editInstances(ExecContext& c, Fn&& fn)
{
    return editComponents(c.geometry("instances"), {.instances = [&](Instances& in) {
                                                        FieldContext ctx = FieldContext::forInstances(in);
                                                        std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                        fn(in, ctx, sel);
                                                    }});
}

void transformNodes()
{
    registerDecl({
        .type = "TranslateInstances",
        .label = "Translate Instances",
        .category = "Instances",
        .doc = "Moves instances. Local moves along each instance's own axes (its rotation and scale apply).",
        .sockets = {.inputs = {geoIn("instances"), selectionIn(), fin("translation", T::Vector, vec(0, 0, 0)), in("local", T::Bool, true)},
                    .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                bool local = c.b("local");
                c.out("instances", editInstances(c, [&](Instances& in, const FieldContext& ctx, const std::vector<uint8_t>& sel) {
                          std::vector<D3> t = evaluateAll<D3>(c.field("translation"), ctx);
                          for (size_t k = 0; k < in.size(); ++k)
                              if (sel[k])
                              {
                                  glm::dmat4& m = in.transforms[k];
                                  D3 d = local ? glm::dmat3(m) * t[k] : t[k];
                                  m[3] += glm::dvec4(d, 0.0);
                              }
                      }));
            },
    });
    registerDecl({
        .type = "RotateInstances",
        .label = "Rotate Instances",
        .category = "Instances",
        .doc = "Turns instances about a pivot. Local turns about each instance's own axes with the pivot in its own space; otherwise "
               "about the world axes with the pivot in world space.",
        .sockets = {.inputs = {geoIn("instances"), selectionIn(), fin("rotation", T::Rotation, vec(0, 0, 0)), fin("pivotPoint", T::Vector, vec(0, 0, 0)),
                               in("local", T::Bool, true)},
                    .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                bool local = c.b("local");
                c.out("instances", editInstances(c, [&](Instances& in, const FieldContext& ctx, const std::vector<uint8_t>& sel) {
                          std::vector<D3> r = expand<D3>(evaluate(c.field("rotation"), ctx, T::Rotation), ctx.size);
                          std::vector<D3> p = evaluateAll<D3>(c.field("pivotPoint"), ctx);
                          for (size_t k = 0; k < in.size(); ++k)
                              if (sel[k])
                                  in.transforms[k] = applyAbout(in.transforms[k], eulerToMatrix(r[k]), p[k], local);
                      }));
            },
    });
    registerDecl({
        .type = "ScaleInstances",
        .label = "Scale Instances",
        .category = "Instances",
        .doc = "Scales instances about a centre. Local scales along each instance's own axes with the centre in its own space.",
        .sockets = {.inputs = {geoIn("instances"), selectionIn(), fin("scale", T::Vector, vec(1, 1, 1)), fin("center", T::Vector, vec(0, 0, 0)),
                               in("local", T::Bool, true)},
                    .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                bool local = c.b("local");
                c.out("instances", editInstances(c, [&](Instances& in, const FieldContext& ctx, const std::vector<uint8_t>& sel) {
                          std::vector<D3> s = evaluateAll<D3>(c.field("scale"), ctx);
                          std::vector<D3> p = evaluateAll<D3>(c.field("center"), ctx);
                          for (size_t k = 0; k < in.size(); ++k)
                              if (sel[k])
                              {
                                  glm::dmat3 sm(1.0);
                                  sm[0][0] = s[k].x;
                                  sm[1][1] = s[k].y;
                                  sm[2][2] = s[k].z;
                                  in.transforms[k] = applyAbout(in.transforms[k], sm, p[k], local);
                              }
                      }));
            },
    });
    registerDecl({
        .type = "SetInstanceTransform",
        .label = "Set Instance Transform",
        .category = "Instances",
        .doc = "Sets instances' translation, rotation and scale. Rotation and scale change only when linked.",
        .sockets = {.inputs = {geoIn("instances"), selectionIn(), fin("translation", T::Vector, nullptr, Implicit::Position), fin("rotation", T::Rotation, vec(0, 0, 0)),
                               fin("scale", T::Vector, vec(1, 1, 1))},
                    .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                bool setRot = !c.group.linksInto(c.node.name, "rotation").empty() || c.node.values.contains("rotation");
                bool setScale = !c.group.linksInto(c.node.name, "scale").empty() || c.node.values.contains("scale");
                c.out("instances", editInstances(c, [&](Instances& in, const FieldContext& ctx, const std::vector<uint8_t>& sel) {
                          std::vector<D3> t = evaluateAll<D3>(c.field("translation"), ctx);
                          std::vector<D3> r = expand<D3>(evaluate(c.field("rotation"), ctx, T::Rotation), ctx.size);
                          std::vector<D3> s = evaluateAll<D3>(c.field("scale"), ctx);
                          for (size_t k = 0; k < in.size(); ++k)
                              if (sel[k])
                              {
                                  const glm::dmat4& m = in.transforms[k];
                                  in.transforms[k] = composeTransform(t[k], setRot ? r[k] : rotationOf(m), setScale ? s[k] : scaleOf(m));
                              }
                      }));
            },
    });
}

void readNodes()
{
    registerDecl({
        .type = "InstanceRotation",
        .label = "Instance Rotation",
        .category = "Instances",
        .doc = "Each instance's rotation.",
        .sockets = {.outputs = {fout("rotation", T::Rotation)}},
        .exec = [](ExecContext& c) { c.out("rotation", std::make_shared<InstanceTransformField>(false)); },
    });
    registerDecl({
        .type = "InstanceScale",
        .label = "Instance Scale",
        .category = "Instances",
        .doc = "Each instance's scale.",
        .sockets = {.outputs = {fout("scale", T::Vector)}},
        .exec = [](ExecContext& c) { c.out("scale", std::make_shared<InstanceTransformField>(true)); },
    });
}

void conversionNodes()
{
    registerDecl({
        .type = "InstancesToPoints",
        .label = "Instances to Points",
        .category = "Instances",
        .doc = "A point for each selected instance, at its position by default, carrying the instance attributes.",
        .sockets = {.inputs = {geoIn("instances"), selectionIn(), fin("position", T::Vector, nullptr, Implicit::Position), bounded(fin("radius", T::Float, 5.0), 0.0)},
                    .outputs = {geoOut("points")}},
        .exec =
            [](ExecContext& c) {
                GeometryPtr g = c.geometry("instances");
                geo::Mesh m;
                if (g->instances)
                {
                    const Instances& in = *g->instances;
                    FieldContext ctx = FieldContext::forInstances(in);
                    std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                    std::vector<D3> pos = evaluateAll<D3>(c.field("position"), ctx);
                    std::vector<double> rad = evaluateAll<double>(c.field("radius"), ctx);
                    std::vector<size_t> src;
                    for (size_t k = 0; k < in.size(); ++k)
                        if (sel[k])
                        {
                            m.addPoint(pos[k]);
                            src.push_back(k);
                        }
                    m.pointAttrs().mergeLayout(in.attrs);
                    std::vector<double> r(src.size());
                    for (size_t k = 0; k < src.size(); ++k)
                    {
                        m.pointAttrs().copyFrom(in.attrs, k, src[k]);
                        r[k] = rad[src[k]];
                    }
                    writeAttribute(m, "radius", Domain::Point, Column::of(std::move(r)), T::Float);
                }
                c.out("points", pointGeometry(std::move(m)));
            },
    });
    registerDecl({
        .type = "GeometryToInstance",
        .label = "Geometry to Instance",
        .category = "Instances",
        .doc = "Turns each linked geometry into one instance at the origin, in link order, so Pick Instance can choose between them.",
        .sockets = {.inputs = {multiIn()}, .outputs = {geoOut("instances")}},
        .exec =
            [](ExecContext& c) {
                Instances out;
                for (const GeometryPtr& part : c.geometries("geometry"))
                {
                    out.references.push_back(part);
                    out.add(glm::dmat4(1.0), int(out.references.size()) - 1);
                }
                Geometry g;
                g.instances = std::make_shared<const Instances>(std::move(out));
                c.out("instances", makeGeometry(std::move(g)));
            },
    });
    registerDecl({
        .type = "Points",
        .label = "Points",
        .category = "Point",
        .doc = "A point cloud of count points, placed by the position field (Index tells them apart).",
        .sockets = {.inputs = {bounded(in("count", T::Int, 1), 0, 100000000), fin("position", T::Vector, vec(0, 0, 0)), bounded(fin("radius", T::Float, 10.0), 0.0)},
                    .outputs = {geoOut("points")}},
        .exec =
            [](ExecContext& c) {
                int64_t n = std::clamp<int64_t>(c.i("count"), 0, 100000000);
                geo::Mesh m;
                for (int64_t k = 0; k < n; ++k)
                    m.addPoint(D3(0.0));
                FieldContext ctx = FieldContext::forPoints(m);
                std::vector<D3> pos = evaluateAll<D3>(c.field("position"), ctx);
                Column rad = evaluate(c.field("radius"), ctx, T::Float);
                for (size_t k = 0; k < pos.size(); ++k)
                    m.setPosition(Index(k), pos[k]);
                writeAttribute(m, "radius", Domain::Point, rad, T::Float);
                c.out("points", pointGeometry(std::move(m)));
            },
    });
}
} // namespace lila::gn::nodes::instancenodes

namespace lila::gn::nodes
{
void registerInstanceNodes()
{
    instancenodes::transformNodes();
    instancenodes::readNodes();
    instancenodes::conversionNodes();
}
} // namespace lila::gn::nodes
