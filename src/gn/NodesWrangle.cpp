#include "gn/Expression.h"
#include "gn/NodesCommon.h"

namespace lila::gn::nodes
{
namespace wrangle
{
using D4 = glm::dvec4;
using expr::AttrKind;
using expr::AttrRef;

SocketType socketFor(expr::Type t)
{
    switch (t)
    {
    case expr::Type::Float: return T::Float;
    case expr::Type::Int: return T::Int;
    case expr::Type::Vector: return T::Vector;
    default: return T::Color;
    }
}

/** P is the point position; every other name is stored as written. */
std::string storageName(const std::string& name) { return name == "P" ? "position" : name; }

FieldPtr readField(const AttrRef& r)
{
    if (r.name == "P")
        return positionField();
    if (r.name == "N")
        return normalField();
    if (r.name == "id")
        return idField();
    return attributeField(r.name, socketFor(r.type));
}

std::vector<D4> toLanes(const Column& c, size_t n)
{
    std::vector<D4> out(c.single ? n : c.size());
    std::visit(
        [&](const auto& v) {
            using V = typename std::decay_t<decltype(v)>::value_type;
            for (size_t k = 0; k < out.size(); ++k)
            {
                const V& x = v.empty() ? V{} : v[c.single ? 0 : k];
                if constexpr (std::is_same_v<V, glm::dvec3>)
                    out[k] = D4(x, 0.0);
                else if constexpr (std::is_same_v<V, glm::dvec4>)
                    out[k] = x;
                else if constexpr (std::is_same_v<V, glm::dmat4>)
                    out[k] = x[3];
                else
                    out[k] = D4(double(x), 0.0, 0.0, 0.0);
            }
        },
        c.data);
    return out;
}

Column fromLanes(const std::vector<D4>& lanes, expr::Type t)
{
    switch (t)
    {
    case expr::Type::Float:
    {
        std::vector<double> v(lanes.size());
        for (size_t k = 0; k < v.size(); ++k)
            v[k] = lanes[k].x;
        return Column::of(std::move(v));
    }
    case expr::Type::Int:
    {
        std::vector<int64_t> v(lanes.size());
        for (size_t k = 0; k < v.size(); ++k)
            v[k] = int64_t(lanes[k].x);
        return Column::of(std::move(v));
    }
    case expr::Type::Vector:
    {
        std::vector<glm::dvec3> v(lanes.size());
        for (size_t k = 0; k < v.size(); ++k)
            v[k] = glm::dvec3(lanes[k]);
        return Column::of(std::move(v));
    }
    default: return Column::of(lanes);
    }
}

expr::ProgramPtr compileFor(const ExecContext& c, const expr::Options& o)
{
    try
    {
        return expr::compile(c.prop("code"), o);
    }
    catch (const expr::CompileError& e)
    {
        throw ExecError(c.node.name + ": " + e.what());
    }
}

/** Reads the program's attributes and inputs on ctx and runs it there. */
expr::Bindings runOn(const expr::Program& prog, const FieldContext& ctx, size_t n, const std::array<FieldPtr, 4>& inputs, const FieldPtr& selection)
{
    expr::Bindings b;
    b.count = n;
    const std::vector<AttrRef>& refs = prog.attributes();
    b.attrs.resize(refs.size());
    for (size_t a = 0; a < refs.size(); ++a)
        if (refs[a].kind == AttrKind::Attribute)
            b.attrs[a] = toLanes(evaluate(readField(refs[a]), ctx, socketFor(refs[a].type)), n);
    for (int k = 0; k < 4; ++k)
        if (prog.usesInput(k) && inputs[size_t(k)])
            b.inputs[size_t(k)] = toLanes(evaluate(inputs[size_t(k)], ctx, k == 3 ? T::Vector : T::Float), n);
    if (selection && !(selection->constant() && evaluateSingle<uint8_t>(selection)))
        b.selection = expand<uint8_t>(evaluate(selection, ctx, T::Bool), n);
    prog.run(b);
    return b;
}

class ExpressionField final : public Field
{
public:
    ExpressionField(SocketType type, expr::ProgramPtr prog, std::array<FieldPtr, 4> inputs)
        : Field(type), mProg(std::move(prog)), mInputs(std::move(inputs))
    {
        mConstant = mProg->attributes().empty();
        for (int k = 0; k < 4; ++k)
            if (mProg->usesInput(k) && mInputs[size_t(k)] && !mInputs[size_t(k)]->constant())
                mConstant = false;
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        size_t n = mConstant || ctx.component == Component::None ? 1 : ctx.size;
        expr::Bindings b = runOn(*mProg, ctx, n, mInputs, nullptr);
        Column c = convert(fromLanes(b.result, mProg->resultType()), type());
        c.single = n == 1 && (mConstant || ctx.component == Component::None);
        return c;
    }
    bool constant() const override { return mConstant; }

private:
    expr::ProgramPtr mProg;
    std::array<FieldPtr, 4> mInputs;
    bool mConstant = false;
};

const char* kLanguageDoc =
    "Statements end in ';'. Types: float, int, vector, vector4 (declare with 'float x = 1;' or assign). Operators + - * / % "
    "^ (power), comparisons, && || !, ?:, x.y components, += -= *= /= ++ --; if/else, for, while, break, continue. @name reads "
    "or writes an attribute: @P position, @N normal (read-only), @Cd colour, @id, @ptnum (element index), @numelem; f@ i@ v@ "
    "p@ give a type, float otherwise. a, b, c (floats) and d (vector) or ch(\"a\") read the inputs. Functions: sin cos tan asin "
    "acos atan atan2 sqrt pow exp log abs floor ceil frac sign radians degrees min max clamp lerp fit smooth length normalize "
    "dot cross distance rand noise set int float; PI. Division by zero gives 0.";

std::array<FieldPtr, 4> inputFields(const ExecContext& c) { return {c.field("a"), c.field("b"), c.field("c"), c.field("d")}; }

std::vector<SocketDecl> valueInputs()
{
    return {fin("a", T::Float, 0.0), fin("b", T::Float, 0.0), fin("c", T::Float, 0.0), fin("d", T::Vector, vec(0, 0, 0))};
}

void wrangleNode()
{
    std::vector<SocketDecl> inputs = {geoIn(), selectionIn()};
    for (SocketDecl& s : valueInputs())
        inputs.push_back(std::move(s));
    registerDecl({
        .type = "Wrangle",
        .label = "Wrangle",
        .category = "Utilities",
        .doc = std::string("Runs code on every selected element of a domain, reading and writing attributes. Writing @P moves points "
                           "(and instances). ") +
               kLanguageDoc,
        .props = {prop("domain", meshDomains(), "point"), textProp("code", "code", "@P.y += a;", "The code run per element.")},
        .sockets = {.inputs = std::move(inputs), .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                expr::ProgramPtr prog = compileFor(c, {});
                const Domain d = domainOf(c.prop("domain"));
                const std::vector<AttrRef>& refs = prog->attributes();
                for (const AttrRef& r : refs)
                    if (r.written && r.name == "P" && d != Domain::Point && d != Domain::Instance)
                        throw ExecError(c.node.name + ": @P can only be written on points or instances");
                std::array<FieldPtr, 4> in = inputFields(c);
                FieldPtr sel = c.field("selection");
                auto written = [&](const expr::Bindings& b, auto&& write) {
                    for (size_t a = 0; a < refs.size(); ++a)
                        if (refs[a].written)
                            write(refs[a], fromLanes(b.attrs[a], refs[a].type));
                };
                ComponentEdits e;
                if (d <= Domain::Corner)
                {
                    e.mesh = [&](geo::Mesh& m) {
                        FieldContext ctx = FieldContext::forMesh(m, d);
                        expr::Bindings b = runOn(*prog, ctx, ctx.size, in, sel);
                        written(b, [&](const AttrRef& r, const Column& col) { writeAttribute(m, storageName(r.name), d, col, socketFor(r.type)); });
                    };
                    if (d == Domain::Point)
                        e.points = [&](geo::Mesh& m) {
                            FieldContext ctx = FieldContext::forPoints(m);
                            expr::Bindings b = runOn(*prog, ctx, ctx.size, in, sel);
                            written(b, [&](const AttrRef& r, const Column& col) {
                                writeAttribute(m, storageName(r.name), Domain::Point, col, socketFor(r.type));
                            });
                        };
                }
                if (d == Domain::Point || d == Domain::Curve)
                    e.curves = [&](CurveSet& cs) {
                        FieldContext ctx = FieldContext::forCurves(cs, d);
                        expr::Bindings b = runOn(*prog, ctx, ctx.size, in, sel);
                        written(b, [&](const AttrRef& r, const Column& col) { writeAttribute(cs, storageName(r.name), d, col, socketFor(r.type)); });
                    };
                if (d == Domain::Instance)
                    e.instances = [&](Instances& inst) {
                        FieldContext ctx = FieldContext::forInstances(inst);
                        expr::Bindings b = runOn(*prog, ctx, ctx.size, in, sel);
                        written(b, [&](const AttrRef& r, const Column& col) {
                            if (r.name != "P")
                            {
                                writeAttribute(inst, r.name, col, socketFor(r.type));
                                return;
                            }
                            const std::vector<glm::dvec3>& p = col.get<glm::dvec3>();
                            for (size_t k = 0; k < inst.size(); ++k)
                                inst.transforms[k][3] = glm::dvec4(p[k], 1.0);
                        });
                    };
                c.out("geometry", editComponents(c.geometry("geometry"), e));
            },
    });
}

void expressionNode()
{
    registerDecl({
        .type = "Expression",
        .label = "Expression",
        .category = "Utilities",
        .doc = std::string("A field computed by code: the value of its last expression, such as 'a * sin(@P.x) + b'. It can read "
                           "attributes but not write them. ") +
               kLanguageDoc,
        .props = {prop("dataType", {"float", "int", "vector", "color", "bool"}, "float"), textProp("code", "code", "a", "The code; its last expression is the value.")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("Expression")->propValue(n, "dataType"));
                return Sockets{valueInputs(), {fout("value", t)}};
            },
        .exec =
            [](ExecContext& c) {
                expr::ProgramPtr prog = compileFor(c, {.allowWrites = false, .needResult = true});
                c.out("value", std::make_shared<ExpressionField>(dataTypeOf(c.prop("dataType")), prog, inputFields(c)));
            },
    });
}
} // namespace wrangle

void registerWrangleNode()
{
    wrangle::wrangleNode();
    wrangle::expressionNode();
}
} // namespace lila::gn::nodes
