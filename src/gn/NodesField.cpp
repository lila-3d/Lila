#include "core/Log.h"
#include "gn/NodesCommon.h"
#include "gn/Texture.h"

#include <glm/gtc/quaternion.hpp>

#include <bit>
#include <format>
#include <map>
#include <numeric>

namespace lila::gn::nodes::fieldnodes
{
using D4 = glm::dvec4;

/** The context for the same component on another domain; components with one domain keep theirs. */
FieldContext contextOn(const FieldContext& ctx, Domain d)
{
    switch (ctx.component)
    {
    case Component::Mesh: return FieldContext::forMesh(*ctx.mesh, d > Domain::Corner ? Domain::Point : d);
    case Component::Curve: return FieldContext::forCurves(*ctx.curves, d);
    default: return ctx;
    }
}

Column toContext(const FieldContext& ctx, const Column& c, Domain from)
{
    if (ctx.component == Component::Mesh)
        return adaptDomain(*ctx.mesh, c, from, ctx.domain);
    if (ctx.component == Component::Curve)
        return adaptCurveDomain(*ctx.curves, c, from, ctx.domain);
    return c;
}

/** A zero of the column storage for t, to dispatch std::visit on. */
ColumnData storageOf(SocketType t) { return convert(Column::of(0.0), t).data; }

class OnDomainField final : public Field
{
public:
    OnDomainField(SocketType t, FieldPtr value, Domain d) : Field(t), mValue(std::move(value)), mDomain(d) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        FieldContext src = contextOn(ctx, mDomain);
        return toContext(ctx, gn::evaluate(mValue, src, type()), src.domain);
    }
    bool constant() const override { return mValue->constant(); }

private:
    FieldPtr mValue;
    Domain mDomain;
};

class AtIndexField final : public Field
{
public:
    AtIndexField(SocketType t, FieldPtr value, FieldPtr index, Domain d) : Field(t), mValue(std::move(value)), mIndex(std::move(index)), mDomain(d) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        Column values = gn::evaluate(mValue, contextOn(ctx, mDomain), type());
        if (values.single)
            return values;
        std::vector<int64_t> index = evaluateAll<int64_t>(mIndex, ctx);
        return std::visit(
            [&](const auto& v) -> Column {
                using V = typename std::decay_t<decltype(v)>::value_type;
                std::vector<V> out(index.size(), V(0));
                for (size_t k = 0; k < index.size(); ++k)
                    if (index[k] >= 0 && size_t(index[k]) < v.size())
                        out[k] = v[size_t(index[k])];
                return Column::of(std::move(out));
            },
            values.data);
    }

private:
    FieldPtr mValue, mIndex;
    Domain mDomain;
};

enum class AccumulateMode : uint8_t
{
    Leading,
    Trailing,
    Total,
};

class AccumulateField final : public Field
{
public:
    AccumulateField(SocketType t, FieldPtr value, FieldPtr group, Domain d, AccumulateMode mode)
        : Field(t), mValue(std::move(value)), mGroup(std::move(group)), mDomain(d), mMode(mode)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        FieldContext src = contextOn(ctx, mDomain);
        size_t n = src.size;
        Column values = gn::evaluate(mValue, src, type());
        std::vector<int64_t> groups = evaluateAll<int64_t>(mGroup, src);
        Column out = std::visit(
            [&](const auto& proto) -> Column {
                using V = typename std::decay_t<decltype(proto)>::value_type;
                std::vector<V> all = expand<V>(values, n);
                std::vector<V> res(n, V(0));
                std::map<int64_t, V> run;
                if (mMode == AccumulateMode::Total)
                {
                    for (size_t k = 0; k < n; ++k)
                        run.try_emplace(groups[k], V(0)).first->second += all[k];
                    for (size_t k = 0; k < n; ++k)
                        res[k] = run[groups[k]];
                }
                else
                    for (size_t k = 0; k < n; ++k)
                    {
                        V& r = run.try_emplace(groups[k], V(0)).first->second;
                        if (mMode == AccumulateMode::Trailing)
                            res[k] = r;
                        r += all[k];
                        if (mMode == AccumulateMode::Leading)
                            res[k] = r;
                    }
                return Column::of(std::move(res));
            },
            values.data);
        return toContext(ctx, out, src.domain);
    }

private:
    FieldPtr mValue, mGroup;
    Domain mDomain;
    AccumulateMode mMode;
};

/** Elements next to each element of the context's domain: across edges for points and faces, sharing a point for edges. */
std::vector<std::vector<Index>> neighbours(const FieldContext& ctx)
{
    std::vector<std::vector<Index>> nb(ctx.size);
    if (ctx.component == Component::Mesh)
    {
        const geo::Mesh& m = *ctx.mesh;
        for (Index e = 0; e < Index(ctx.size); ++e)
            switch (ctx.domain)
            {
            case Domain::Point:
                if (m.pointValid(e))
                    m.forEachPointEdge(e, [&](Index x) { nb[e].push_back(m.edgeOtherPoint(x, e)); });
                break;
            case Domain::Edge:
                if (m.edgeValid(e))
                    for (int s = 0; s < 2; ++s)
                        m.forEachPointEdge(m.edgePoint(e, s), [&](Index x) {
                            if (x != e)
                                nb[e].push_back(x);
                        });
                break;
            case Domain::Face:
                if (m.faceValid(e))
                    m.forEachFaceCorner(e, [&](Index c) {
                        for (Index r = m.cornerRadial(c); r != c; r = m.cornerRadial(r))
                            nb[e].push_back(m.cornerFace(r));
                    });
                break;
            case Domain::Corner:
                if (m.cornerValid(e))
                    nb[e] = {m.cornerPrev(e), m.cornerNext(e)};
                break;
            default: break;
            }
    }
    else if (ctx.component == Component::Curve && ctx.domain == Domain::Point)
    {
        const CurveSet& cs = *ctx.curves;
        for (size_t c = 0; c < cs.numCurves(); ++c)
        {
            Index a = cs.offsets[c], b = cs.offsets[c + 1];
            bool cyclic = cs.cyclic[c] != 0 && b - a > 2;
            for (Index p = a; p < b; ++p)
            {
                if (p > a)
                    nb[p].push_back(p - 1);
                else if (cyclic)
                    nb[p].push_back(b - 1);
                if (p + 1 < b)
                    nb[p].push_back(p + 1);
                else if (cyclic)
                    nb[p].push_back(a);
            }
        }
    }
    return nb;
}

template <class V> auto toAcc(const V& v)
{
    if constexpr (std::is_arithmetic_v<V>)
        return double(v);
    else
        return v;
}

template <class V, class A> V fromAcc(const A& a)
{
    if constexpr (std::is_same_v<V, int64_t>)
        return int64_t(std::llround(a));
    else if constexpr (std::is_same_v<V, uint8_t>)
        return uint8_t(a >= 0.5 ? 1 : 0);
    else
        return V(a);
}

class BlurField final : public Field
{
public:
    BlurField(SocketType t, FieldPtr value, FieldPtr weight, int iterations) : Field(t), mValue(std::move(value)), mWeight(std::move(weight)), mIterations(iterations) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        Column values = gn::evaluate(mValue, ctx, type());
        if (values.single || mIterations <= 0)
            return values;
        std::vector<std::vector<Index>> nb = neighbours(ctx);
        std::vector<double> weights = evaluateAll<double>(mWeight, ctx);
        return std::visit(
            [&](const auto& v) -> Column {
                using V = typename std::decay_t<decltype(v)>::value_type;
                std::vector<V> cur = v, next(v.size());
                for (int it = 0; it < mIterations; ++it)
                {
                    tbb::parallel_for(size_t(0), cur.size(), [&](size_t k) {
                        auto sum = toAcc(cur[k]);
                        double total = 1.0;
                        for (Index j : nb[k])
                        {
                            sum += toAcc(cur[j]) * weights[k];
                            total += weights[k];
                        }
                        next[k] = total > 0.0 ? fromAcc<V>(sum / total) : cur[k];
                    });
                    std::swap(cur, next);
                }
                return Column::of(std::move(cur));
            },
            values.data);
    }

private:
    FieldPtr mValue, mWeight;
    int mIterations;
};

const std::vector<std::string>& numericTypes()
{
    static const std::vector<std::string> v = {"float", "int", "vector"};
    return v;
}

const std::vector<std::string>& blurTypes()
{
    static const std::vector<std::string> v = {"float", "int", "vector", "color"};
    return v;
}

void accumulate()
{
    registerDecl({
        .type = "AccumulateField",
        .label = "Accumulate Field",
        .category = "Utilities",
        .doc = "Running totals of a field over a domain in index order: leading includes each element, trailing stops just before it, total "
               "is the sum of its group. Elements with the same group id accumulate together.",
        .props = {prop("dataType", numericTypes(), "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("AccumulateField")->propValue(n, "dataType"));
                json one = t == T::Vector ? vec(1, 1, 1) : json(1);
                return Sockets{{fin("value", t, one), fin("groupId", T::Int, 0, Implicit::None, "Elements with equal ids are summed separately.")},
                               {fout("leading", t), fout("trailing", t), fout("total", t)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                Domain d = domainOf(c.prop("domain"));
                c.out("leading", std::make_shared<AccumulateField>(t, c.field("value"), c.field("groupId"), d, AccumulateMode::Leading));
                c.out("trailing", std::make_shared<AccumulateField>(t, c.field("value"), c.field("groupId"), d, AccumulateMode::Trailing));
                c.out("total", std::make_shared<AccumulateField>(t, c.field("value"), c.field("groupId"), d, AccumulateMode::Total));
            },
    });
}

void evaluateAtIndex()
{
    registerDecl({
        .type = "EvaluateAtIndex",
        .label = "Evaluate at Index",
        .category = "Utilities",
        .doc = "Reads a field at another element of the same geometry: the value at the index, evaluated on the domain. Out of range indices "
               "read zero.",
        .props = {prop("dataType", dataTypes(), "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("EvaluateAtIndex")->propValue(n, "dataType"));
                return Sockets{{fin("value", t, zeroJson(t)), fin("index", T::Int, 0, Implicit::Index)}, {fout("value", t)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                c.out("value", std::make_shared<AtIndexField>(t, c.field("value"), c.field("index"), domainOf(c.prop("domain"))));
            },
    });
}

void evaluateOnDomain()
{
    registerDecl({
        .type = "EvaluateOnDomain",
        .label = "Evaluate on Domain",
        .category = "Utilities",
        .doc = "Evaluates a field on another domain and brings the result back to the one it is read on, averaging where several elements meet.",
        .props = {prop("dataType", dataTypes(), "float"), prop("domain", meshDomains(), "point")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("EvaluateOnDomain")->propValue(n, "dataType"));
                return Sockets{{fin("value", t, zeroJson(t))}, {fout("value", t)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                c.out("value", std::make_shared<OnDomainField>(t, c.field("value"), domainOf(c.prop("domain"))));
            },
    });
}

void blur()
{
    registerDecl({
        .type = "BlurAttribute",
        .label = "Blur Attribute",
        .category = "Attribute",
        .doc = "Averages a field with its neighbours, repeated iterations times: points across edges, edges sharing a point, faces across "
               "edges, corners within their face, curve points along their curve. Weight scales how much the neighbours count.",
        .props = {prop("dataType", blurTypes(), "float")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                SocketType t = dataTypeOf(findDecl("BlurAttribute")->propValue(n, "dataType"));
                return Sockets{{fin("value", t, zeroJson(t)), bounded(in("iterations", T::Int, 1), 0, 10000), bounded(fin("weight", T::Float, 1.0), 0.0, 1.0)},
                               {fout("value", t)}};
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                int iterations = int(std::clamp<int64_t>(c.i("iterations"), 0, 10000));
                c.out("value", std::make_shared<BlurField>(t, c.field("value"), c.field("weight"), iterations));
            },
    });
}

/** '*' matches any run of characters. */
bool wildcardMatch(std::string_view pattern, std::string_view s)
{
    size_t p = 0, k = 0, star = std::string_view::npos, mark = 0;
    while (k < s.size())
    {
        if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            mark = k;
        }
        else if (p < pattern.size() && pattern[p] == s[k])
        {
            ++p;
            ++k;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            k = ++mark;
        }
        else
            return false;
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

void removeMatching(geo::AttributeSet& set, const std::string& name, bool wildcard)
{
    for (const std::string& n : set.names())
        if (wildcard ? wildcardMatch(name, n) : (n == name || n == "color:" + name))
            set.remove(n);
}

void removeNamedAttribute()
{
    registerDecl({
        .type = "RemoveNamedAttribute",
        .label = "Remove Named Attribute",
        .category = "Attribute",
        .doc = "Deletes attributes by name from every component and domain. With pattern set to wildcard, * matches any characters (uv*).",
        .props = {prop("pattern", {"exact", "wildcard"}, "exact")},
        .sockets = {.inputs = {geoIn(), in("name", T::String, "")}, .outputs = {geoOut()}},
        .exec =
            [](ExecContext& c) {
                std::string name = c.s("name");
                if (name.empty())
                {
                    c.out("geometry", c.geometry("geometry"));
                    return;
                }
                bool wildcard = c.prop("pattern") == "wildcard";
                auto mesh = [&](geo::Mesh& m) {
                    removeMatching(m.pointAttrs(), name, wildcard);
                    removeMatching(m.edgeAttrs(), name, wildcard);
                    removeMatching(m.faceAttrs(), name, wildcard);
                    removeMatching(m.cornerAttrs(), name, wildcard);
                };
                c.out("geometry", editComponents(c.geometry("geometry"), {.mesh = mesh,
                                                                          .points = mesh,
                                                                          .curves =
                                                                              [&](CurveSet& cs) {
                                                                                  removeMatching(cs.pointAttrs, name, wildcard);
                                                                                  removeMatching(cs.curveAttrs, name, wildcard);
                                                                              },
                                                                          .instances = [&](Instances& i) { removeMatching(i.attrs, name, wildcard); }}));
            },
    });
}

uint32_t floatBits(double v) { return std::bit_cast<uint32_t>(float(v)); }

uint32_t stringHash(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char ch : s)
        h = (h ^ ch) * 16777619u;
    return h;
}

void hashValue()
{
    static const std::vector<std::string> types = {"float", "int", "vector", "rotation", "color", "string"};
    registerDecl({
        .type = "HashValue",
        .label = "Hash Value",
        .category = "Utilities",
        .doc = "A stable pseudo-random integer from a value and a seed: equal inputs always give the same hash.",
        .props = {prop("dataType", types, "int")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                std::string dt = findDecl("HashValue")->propValue(n, "dataType");
                SocketDecl value = dt == "string" ? in("value", T::String, "") : fin("value", dataTypeOf(dt), zeroJson(dataTypeOf(dt)));
                return Sockets{{value, fin("seed", T::Int, 0)}, {fout("hash", T::Int)}};
            },
        .exec =
            [](ExecContext& c) {
                std::string dt = c.prop("dataType");
                FieldPtr seed = c.field("seed");
                FieldPtr out;
                if (dt == "string")
                {
                    uint32_t h = stringHash(c.s("value"));
                    out = map<int64_t, int64_t>(T::Int, [h](int64_t s) { return int64_t(tex::hashU(h, uint32_t(s))); }, {seed});
                }
                else if (dt == "float")
                    out = map<int64_t, double, int64_t>(T::Int, [](double v, int64_t s) { return int64_t(tex::hashU(floatBits(v), uint32_t(s))); }, {c.field("value"), seed});
                else if (dt == "vector" || dt == "rotation")
                    out = map<int64_t, D3, int64_t>(
                        T::Int, [](D3 v, int64_t s) { return int64_t(tex::hashU(floatBits(v.x), floatBits(v.y), floatBits(v.z), uint32_t(s))); },
                        {c.field("value"), seed});
                else if (dt == "color")
                    out = map<int64_t, D4, int64_t>(
                        T::Int,
                        [](D4 v, int64_t s) {
                            return int64_t(tex::hashU(tex::hashU(floatBits(v.r), floatBits(v.g), floatBits(v.b), floatBits(v.a)), uint32_t(s)));
                        },
                        {c.field("value"), seed});
                else
                    out = map<int64_t, int64_t, int64_t>(T::Int, [](int64_t v, int64_t s) { return int64_t(tex::hashU(uint32_t(v), uint32_t(s))); },
                                                         {c.field("value"), seed});
                c.out("hash", out);
            },
    });
}

struct IntOp
{
    int arity;
    int64_t (*fn)(int64_t, int64_t, int64_t);
};

int64_t floorDiv(int64_t a, int64_t b)
{
    if (b == 0)
        return 0;
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int64_t ipow(int64_t a, int64_t b)
{
    if (b < 0)
        return a == 1 ? 1 : a == -1 ? (b % 2 == 0 ? 1 : -1) : 0;
    uint64_t r = 1, base = uint64_t(a);
    for (uint64_t e = uint64_t(b); e; e >>= 1)
    {
        if (e & 1)
            r *= base;
        base *= base;
    }
    return int64_t(r);
}

const std::map<std::string, IntOp>& intOps()
{
    static const std::map<std::string, IntOp> ops = {
        {"add", {2, [](int64_t a, int64_t b, int64_t) { return a + b; }}},
        {"subtract", {2, [](int64_t a, int64_t b, int64_t) { return a - b; }}},
        {"multiply", {2, [](int64_t a, int64_t b, int64_t) { return a * b; }}},
        {"divide", {2, [](int64_t a, int64_t b, int64_t) { return b == 0 ? int64_t(0) : a / b; }}},
        {"divideFloor", {2, [](int64_t a, int64_t b, int64_t) { return floorDiv(a, b); }}},
        {"divideCeil", {2, [](int64_t a, int64_t b, int64_t) { return b == 0 ? int64_t(0) : -floorDiv(-a, b); }}},
        {"divideRound", {2, [](int64_t a, int64_t b, int64_t) { return b == 0 ? int64_t(0) : int64_t(std::llround(double(a) / double(b))); }}},
        {"multiplyAdd", {3, [](int64_t a, int64_t b, int64_t c) { return a * b + c; }}},
        {"absolute", {1, [](int64_t a, int64_t, int64_t) { return a < 0 ? -a : a; }}},
        {"negate", {1, [](int64_t a, int64_t, int64_t) { return -a; }}},
        {"power", {2, [](int64_t a, int64_t b, int64_t) { return ipow(a, b); }}},
        {"minimum", {2, [](int64_t a, int64_t b, int64_t) { return std::min(a, b); }}},
        {"maximum", {2, [](int64_t a, int64_t b, int64_t) { return std::max(a, b); }}},
        {"sign", {1, [](int64_t a, int64_t, int64_t) { return int64_t(a > 0 ? 1 : a < 0 ? -1 : 0); }}},
        {"modulo", {2, [](int64_t a, int64_t b, int64_t) { return b == 0 ? int64_t(0) : a % b; }}},
        {"flooredModulo", {2, [](int64_t a, int64_t b, int64_t) { return b == 0 ? int64_t(0) : a - floorDiv(a, b) * b; }}},
        {"gcd", {2, [](int64_t a, int64_t b, int64_t) { return std::gcd(a, b); }}},
        {"lcm", {2, [](int64_t a, int64_t b, int64_t) { return std::lcm(a, b); }}},
    };
    return ops;
}

const std::map<std::string, IntOp>& bitOps()
{
    static const std::map<std::string, IntOp> ops = {
        {"and", {2, [](int64_t a, int64_t b, int64_t) { return int64_t(int32_t(uint32_t(a) & uint32_t(b))); }}},
        {"or", {2, [](int64_t a, int64_t b, int64_t) { return int64_t(int32_t(uint32_t(a) | uint32_t(b))); }}},
        {"xor", {2, [](int64_t a, int64_t b, int64_t) { return int64_t(int32_t(uint32_t(a) ^ uint32_t(b))); }}},
        {"not", {1, [](int64_t a, int64_t, int64_t) { return int64_t(int32_t(~uint32_t(a))); }}},
        {"shift", {2, [](int64_t a, int64_t b, int64_t) {
             uint32_t v = uint32_t(a);
             if (b >= 32 || b <= -32)
                 return int64_t(0);
             return int64_t(int32_t(b >= 0 ? v << b : v >> -b));
         }}},
        {"rotate", {2, [](int64_t a, int64_t b, int64_t) {
             int s = int(((b % 32) + 32) % 32);
             return int64_t(int32_t(std::rotl(uint32_t(a), s)));
         }}},
    };
    return ops;
}

std::vector<std::string> opNames(const std::map<std::string, IntOp>& m)
{
    std::vector<std::string> out;
    for (auto& [k, v] : m)
        out.push_back(k);
    return out;
}

void intOpNode(const char* type, const char* label, const char* doc, const std::map<std::string, IntOp>& (*ops)(), const char* def)
{
    std::string typeName = type;
    registerDecl({
        .type = type,
        .label = label,
        .category = "Utilities",
        .doc = doc,
        .props = {prop("operation", opNames(ops()), def)},
        .dynamicSockets =
            [ops, typeName](const Group&, const GNode& n) {
                auto it = ops().find(findDecl(typeName)->propValue(n, "operation"));
                int arity = it == ops().end() ? 2 : it->second.arity;
                Sockets s;
                s.inputs = {fin("a", T::Int, 0)};
                if (arity > 1)
                    s.inputs.push_back(fin("b", T::Int, 0));
                if (arity > 2)
                    s.inputs.push_back(fin("c", T::Int, 0));
                s.outputs = {fout("value", T::Int)};
                return s;
            },
        .exec =
            [ops](ExecContext& c) {
                auto it = ops().find(c.prop("operation"));
                if (it == ops().end())
                    throw ExecError("unknown operation " + c.prop("operation"));
                auto fn = it->second.fn;
                FieldPtr b = c.sockets.inputIndex("b") >= 0 ? c.field("b") : constantInt(0);
                FieldPtr cc = c.sockets.inputIndex("c") >= 0 ? c.field("c") : constantInt(0);
                c.out("value", map<int64_t, int64_t, int64_t, int64_t>(T::Int, fn, {c.field("a"), b, cc}));
            },
    });
}

const std::vector<std::string>& switchTypes()
{
    static const std::vector<std::string> v = {"float", "int", "vector", "bool", "rotation", "color", "string", "geometry"};
    return v;
}

SocketType switchTypeOf(const std::string& s) { return s == "geometry" ? T::Geometry : s == "string" ? T::String : dataTypeOf(s); }

SocketDecl switchItem(const std::string& id, SocketType t)
{
    if (t == T::Geometry)
        return geoIn(id);
    if (t == T::String)
        return in(id, T::String, "");
    return fin(id, t, zeroJson(t));
}

SocketDecl switchOutput(SocketType t)
{
    if (t == T::Geometry)
        return geoOut("output");
    if (t == T::String)
        return out("output", T::String);
    return fout("output", t);
}

/** Per element, the item the index field picks; out of range picks zero. */
FieldPtr pickField(SocketType t, FieldPtr index, const std::vector<FieldPtr>& items)
{
    std::vector<FieldPtr> ins{std::move(index)};
    ins.insert(ins.end(), items.begin(), items.end());
    std::vector<SocketType> types(ins.size(), t);
    types[0] = T::Int;
    size_t count = items.size();
    return std::make_shared<FunctionField>(t, ins, types, [count, t](const std::vector<Column>& in, size_t n) {
        bool single = true;
        for (const Column& x : in)
            single = single && x.single;
        size_t cnt = single ? 1 : n;
        return std::visit(
            [&](const auto& proto) -> Column {
                using V = typename std::decay_t<decltype(proto)>::value_type;
                std::vector<V> out(cnt, V(0));
                for (size_t k = 0; k < cnt; ++k)
                {
                    int64_t i = in[0].at<int64_t>(k);
                    if (i >= 0 && size_t(i) < count)
                        out[k] = in[1 + size_t(i)].template at<V>(k);
                }
                return Column{std::move(out), single};
            },
            storageOf(t));
    });
}

/** Sends item k (or nothing when out of range) to the switch's output. */
void outputItem(ExecContext& c, SocketType t, const std::vector<std::string>& ids, FieldPtr index, int64_t single)
{
    bool ok = single >= 0 && size_t(single) < ids.size();
    if (t == T::Geometry)
        c.out("output", ok ? c.geometry(ids[size_t(single)]) : makeGeometry({}));
    else if (t == T::String)
        c.outString("output", ok ? c.s(ids[size_t(single)]) : std::string());
    else
    {
        std::vector<FieldPtr> items;
        for (const std::string& id : ids)
            items.push_back(c.field(id));
        c.out("output", pickField(t, std::move(index), items));
    }
}

int switchCount(const std::string& s)
{
    try
    {
        return std::clamp(std::stoi(s), 1, 64);
    }
    catch (const std::exception&)
    {
        return 2;
    }
}

void indexSwitch()
{
    registerDecl({
        .type = "IndexSwitch",
        .label = "Index Switch",
        .category = "Utilities",
        .doc = "Picks one of several inputs by index. For data types the index can be a field, choosing per element; out of range gives zero.",
        .props = {prop("dataType", switchTypes(), "geometry"), prop("count", {}, "2", "Number of items, 1 to 64.")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                const NodeDecl* d = findDecl("IndexSwitch");
                SocketType t = switchTypeOf(d->propValue(n, "dataType"));
                int count = switchCount(d->propValue(n, "count"));
                Sockets s;
                s.inputs.push_back(isDataType(t) ? fin("index", T::Int, 0) : in("index", T::Int, 0));
                for (int k = 0; k < count; ++k)
                    s.inputs.push_back(switchItem("item" + std::to_string(k), t));
                s.outputs = {switchOutput(t)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = switchTypeOf(c.prop("dataType"));
                std::vector<std::string> ids;
                for (int k = 0; k < switchCount(c.prop("count")); ++k)
                    ids.push_back("item" + std::to_string(k));
                outputItem(c, t, ids, c.field("index"), isDataType(t) ? 0 : c.i("index"));
            },
    });
}

/** Menu entries from a comma list, trimmed, empties dropped. */
std::vector<std::string> menuOptions(const std::string& list)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find(',', start);
        if (end == std::string::npos)
            end = list.size();
        std::string item = list.substr(start, end - start);
        size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
        if (a != std::string::npos)
            out.push_back(item.substr(a, b - a + 1));
        start = end + 1;
    }
    return out;
}

/** A socket id for a menu entry: letters, digits and underscores. */
std::string menuId(const std::string& option)
{
    std::string id;
    for (char ch : option)
        id += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    if (id.empty() || std::isdigit(static_cast<unsigned char>(id[0])))
        id = "_" + id;
    return id;
}

void menuSwitch()
{
    registerDecl({
        .type = "MenuSwitch",
        .label = "Menu Switch",
        .category = "Utilities",
        .doc = "Picks the input named by menu from a list of named entries (options: a comma list). Expose menu as a string group input to "
               "offer the choice on the modifier.",
        .props = {prop("dataType", switchTypes(), "geometry"), prop("options", {}, "A, B", "Entry names, comma separated.")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                const NodeDecl* d = findDecl("MenuSwitch");
                SocketType t = switchTypeOf(d->propValue(n, "dataType"));
                std::vector<std::string> options = menuOptions(d->propValue(n, "options"));
                Sockets s;
                s.inputs.push_back(in("menu", T::String, options.empty() ? std::string() : options[0], "Name of the entry to pass on."));
                for (const std::string& o : options)
                {
                    SocketDecl item = switchItem(menuId(o), t);
                    item.doc = o;
                    s.inputs.push_back(std::move(item));
                }
                s.outputs = {switchOutput(t)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = switchTypeOf(c.prop("dataType"));
                std::vector<std::string> options = menuOptions(c.prop("options"));
                std::string chosen = c.s("menu");
                std::vector<std::string> ids;
                int64_t pick = -1;
                for (size_t k = 0; k < options.size(); ++k)
                {
                    ids.push_back(menuId(options[k]));
                    if (pick < 0 && (options[k] == chosen || ids.back() == chosen))
                        pick = int64_t(k);
                }
                outputItem(c, t, ids, constantInt(pick), pick);
            },
    });
}

D3 axisAngleToEuler(D3 axis, double angle) { return matrixToEuler(axisAngle(axis, angle)); }

glm::dquat eulerToQuat(const D3& e)
{
    glm::dquat q = glm::quat_cast(eulerToMatrix(e));
    return q.w < 0.0 ? -q : q;
}

void rotationNodes()
{
    registerDecl({
        .type = "AxisAngleToRotation",
        .label = "Axis Angle to Rotation",
        .category = "Rotation",
        .doc = "A rotation of angle radians about an axis.",
        .sockets = {.inputs = {fin("axis", T::Vector, vec(0, 0, 1)), fin("angle", T::Float, 0.0)}, .outputs = {fout("rotation", T::Rotation)}},
        .exec = [](ExecContext& c) { c.out("rotation", map<D3, D3, double>(T::Rotation, axisAngleToEuler, {c.field("axis"), c.field("angle")})); },
    });
    registerDecl({
        .type = "RotationToAxisAngle",
        .label = "Rotation to Axis Angle",
        .category = "Rotation",
        .doc = "The axis and angle (0 to pi radians) of a rotation. No rotation gives the Z axis.",
        .sockets = {.inputs = {fin("rotation", T::Rotation, vec(0, 0, 0))}, .outputs = {fout("axis", T::Vector), fout("angle", T::Float)}},
        .exec =
            [](ExecContext& c) {
                c.out("axis", map<D3, D3>(
                                  T::Vector,
                                  [](D3 e) {
                                      glm::dquat q = eulerToQuat(e);
                                      double s = std::sqrt(std::max(0.0, 1.0 - q.w * q.w));
                                      return s < 1e-9 ? D3(0, 0, 1) : D3(q.x, q.y, q.z) / s;
                                  },
                                  {c.field("rotation")}));
                c.out("angle", map<double, D3>(T::Float, [](D3 e) { return 2.0 * std::acos(std::clamp(eulerToQuat(e).w, -1.0, 1.0)); }, {c.field("rotation")}));
            },
    });
    registerDecl({
        .type = "RotateRotation",
        .label = "Rotate Rotation",
        .category = "Rotation",
        .doc = "Applies a second rotation: in global space it turns about the world axes, in local space about the rotation's own axes.",
        .props = {prop("space", {"global", "local"}, "global")},
        .sockets = {.inputs = {fin("rotation", T::Rotation, vec(0, 0, 0)), fin("rotateBy", T::Rotation, vec(0, 0, 0))}, .outputs = {fout("rotation", T::Rotation)}},
        .exec =
            [](ExecContext& c) {
                bool local = c.prop("space") == "local";
                c.out("rotation", map<D3, D3, D3>(
                                      T::Rotation,
                                      [local](D3 r, D3 by) {
                                          glm::dmat3 a = eulerToMatrix(r), b = eulerToMatrix(by);
                                          return matrixToEuler(local ? a * b : b * a);
                                      },
                                      {c.field("rotation"), c.field("rotateBy")}));
            },
    });
    registerDecl({
        .type = "InvertRotation",
        .label = "Invert Rotation",
        .category = "Rotation",
        .doc = "The rotation that undoes this one.",
        .sockets = {.inputs = {fin("rotation", T::Rotation, vec(0, 0, 0))}, .outputs = {fout("rotation", T::Rotation)}},
        .exec =
            [](ExecContext& c) {
                c.out("rotation", map<D3, D3>(T::Rotation, [](D3 r) { return matrixToEuler(glm::transpose(eulerToMatrix(r))); }, {c.field("rotation")}));
            },
    });
    registerDecl({
        .type = "EulerToQuaternion",
        .label = "Rotation to Quaternion",
        .category = "Rotation",
        .doc = "A rotation as a unit quaternion with w not negative.",
        .sockets = {.inputs = {fin("rotation", T::Rotation, vec(0, 0, 0))},
                    .outputs = {fout("w", T::Float), fout("x", T::Float), fout("y", T::Float), fout("z", T::Float)}},
        .exec =
            [](ExecContext& c) {
                FieldPtr r = c.field("rotation");
                c.out("w", map<double, D3>(T::Float, [](D3 e) { return eulerToQuat(e).w; }, {r}));
                c.out("x", map<double, D3>(T::Float, [](D3 e) { return eulerToQuat(e).x; }, {r}));
                c.out("y", map<double, D3>(T::Float, [](D3 e) { return eulerToQuat(e).y; }, {r}));
                c.out("z", map<double, D3>(T::Float, [](D3 e) { return eulerToQuat(e).z; }, {r}));
            },
    });
    registerDecl({
        .type = "QuaternionToEuler",
        .label = "Quaternion to Rotation",
        .category = "Rotation",
        .doc = "A rotation from quaternion components, normalized first. All zero gives no rotation.",
        .sockets = {.inputs = {fin("w", T::Float, 1.0), fin("x", T::Float, 0.0), fin("y", T::Float, 0.0), fin("z", T::Float, 0.0)},
                    .outputs = {fout("rotation", T::Rotation)}},
        .exec =
            [](ExecContext& c) {
                c.out("rotation", map<D3, double, double, double, double>(
                                      T::Rotation,
                                      [](double w, double x, double y, double z) {
                                          double len = std::sqrt(w * w + x * x + y * y + z * z);
                                          if (len < 1e-12)
                                              return D3(0.0);
                                          glm::dquat q;
                                          q.w = w / len;
                                          q.x = x / len;
                                          q.y = y / len;
                                          q.z = z / len;
                                          return matrixToEuler(glm::mat3_cast(q));
                                      },
                                      {c.field("w"), c.field("x"), c.field("y"), c.field("z")}));
            },
    });
    registerDecl({
        .type = "TransformPoint",
        .label = "Transform Point",
        .category = "Utilities",
        .doc = "Moves a point by a transform given as translation, rotation and scale (scale first, then rotation, then translation).",
        .sockets = {.inputs = {fin("vector", T::Vector, vec(0, 0, 0)), fin("translation", T::Vector, vec(0, 0, 0)), fin("rotation", T::Rotation, vec(0, 0, 0)),
                               fin("scale", T::Vector, vec(1, 1, 1))},
                    .outputs = {fout("vector", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                c.out("vector", map<D3, D3, D3, D3, D3>(
                                    T::Vector, [](D3 p, D3 t, D3 r, D3 s) { return D3(composeTransform(t, r, s) * D4(p, 1.0)); },
                                    {c.field("vector"), c.field("translation"), c.field("rotation"), c.field("scale")}));
            },
    });
    registerDecl({
        .type = "TransformDirection",
        .label = "Transform Direction",
        .category = "Utilities",
        .doc = "Turns and scales a direction by a rotation and scale; translation doesn't apply to directions.",
        .sockets = {.inputs = {fin("direction", T::Vector, vec(0, 0, 1)), fin("rotation", T::Rotation, vec(0, 0, 0)), fin("scale", T::Vector, vec(1, 1, 1))},
                    .outputs = {fout("direction", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                c.out("direction", map<D3, D3, D3, D3>(T::Vector, [](D3 d, D3 r, D3 s) { return eulerToMatrix(r) * (s * d); },
                                                       {c.field("direction"), c.field("rotation"), c.field("scale")}));
            },
    });
}

int64_t utf8Length(const std::string& s)
{
    int64_t n = 0;
    for (unsigned char ch : s)
        if ((ch & 0xC0) != 0x80)
            ++n;
    return n;
}

/** Code points [start, start + length) of UTF-8 text. */
std::string utf8Slice(const std::string& s, int64_t start, int64_t length)
{
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size(); ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            starts.push_back(i);
    int64_t n = int64_t(starts.size());
    start = std::clamp<int64_t>(start, 0, n);
    int64_t end = std::clamp<int64_t>(start + std::max<int64_t>(length, 0), start, n);
    size_t a = start < n ? starts[size_t(start)] : s.size();
    size_t b = end < n ? starts[size_t(end)] : s.size();
    return s.substr(a, b - a);
}

void stringNodes()
{
    registerDecl({
        .type = "JoinStrings",
        .label = "Join Strings",
        .category = "Text",
        .doc = "Joins every linked string in link order, with the delimiter between them.",
        .sockets = {.inputs = {in("delimiter", T::String, ""), {.id = "strings", .type = T::String, .multi = true}}, .outputs = {out("string", T::String)}},
        .exec =
            [](ExecContext& c) {
                std::string result, delimiter = c.s("delimiter");
                int k = c.sockets.inputIndex("strings");
                bool first = true;
                for (const Value& v : c.inputs[size_t(k)])
                {
                    if (!first)
                        result += delimiter;
                    result += v.string;
                    first = false;
                }
                c.outString("string", std::move(result));
            },
    });
    registerDecl({
        .type = "ReplaceString",
        .label = "Replace String",
        .category = "Text",
        .doc = "Replaces every occurrence of find with replace.",
        .sockets = {.inputs = {in("string", T::String, ""), in("find", T::String, ""), in("replace", T::String, "")}, .outputs = {out("string", T::String)}},
        .exec =
            [](ExecContext& c) {
                std::string s = c.s("string"), find = c.s("find"), rep = c.s("replace");
                if (!find.empty())
                    for (size_t at = s.find(find); at != std::string::npos; at = s.find(find, at + rep.size()))
                        s.replace(at, find.size(), rep);
                c.outString("string", std::move(s));
            },
    });
    registerDecl({
        .type = "SliceString",
        .label = "Slice String",
        .category = "Text",
        .doc = "length characters starting at position (0 is the first), counting characters rather than bytes.",
        .sockets = {.inputs = {in("string", T::String, ""), bounded(in("position", T::Int, 0), 0.0), bounded(in("length", T::Int, 10), 0.0)},
                    .outputs = {out("string", T::String)}},
        .exec = [](ExecContext& c) { c.outString("string", utf8Slice(c.s("string"), c.i("position"), c.i("length"))); },
    });
    registerDecl({
        .type = "StringLength",
        .label = "String Length",
        .category = "Text",
        .doc = "Number of characters in the string.",
        .sockets = {.inputs = {in("string", T::String, "")}, .outputs = {fout("length", T::Int)}},
        .exec = [](ExecContext& c) { c.out("length", constantInt(utf8Length(c.s("string")))); },
    });
    registerDecl({
        .type = "ValueToString",
        .label = "Value to String",
        .category = "Text",
        .doc = "A number as text with a fixed number of decimals.",
        .props = {prop("dataType", {"float", "int"}, "float")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                bool isInt = findDecl("ValueToString")->propValue(n, "dataType") == "int";
                Sockets s;
                s.inputs = {isInt ? in("value", T::Int, 0) : in("value", T::Float, 0.0)};
                if (!isInt)
                    s.inputs.push_back(bounded(in("decimals", T::Int, 0), 0, 20));
                s.outputs = {out("string", T::String)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                if (c.prop("dataType") == "int")
                    c.outString("string", std::to_string(c.i("value")));
                else
                    c.outString("string", std::format("{:.{}f}", c.f("value"), int(std::clamp<int64_t>(c.i("decimals"), 0, 20))));
            },
    });
}

void warningNode()
{
    registerDecl({
        .type = "Warning",
        .label = "Warning",
        .category = "Utilities",
        .doc = "Reports the message when show is true and the node is evaluated (its output used downstream). An error stops the group "
               "with the message; a warning or info is logged.",
        .props = {prop("warningType", {"error", "warning", "info"}, "error")},
        .sockets = {.inputs = {in("show", T::Bool, true), in("message", T::String, "")}, .outputs = {fout("show", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                bool show = c.b("show");
                c.out("show", constantBool(show));
                if (!show)
                    return;
                std::string msg = c.s("message").empty() ? c.node.name : c.s("message");
                std::string kind = c.prop("warningType");
                if (kind == "error")
                    throw ExecError(c.node.name + ": " + msg);
                if (kind == "warning")
                    log::warn("{}: {}", c.node.name, msg);
                else
                    log::info("{}: {}", c.node.name, msg);
            },
    });
}

void selfObject()
{
    registerDecl({
        .type = "SelfObject",
        .label = "Self Object",
        .category = "Input",
        .doc = "The world transform of the object the modifier is on, as location, rotation and scale. Inside a nested group it is the "
               "identity.",
        .sockets = {.outputs = {fout("location", T::Vector), fout("rotation", T::Rotation), fout("scale", T::Vector)}},
        .exec =
            [](ExecContext& c) {
                const glm::dmat4& m = c.env.ownerMatrix;
                D3 scale(glm::length(D3(m[0])), glm::length(D3(m[1])), glm::length(D3(m[2])));
                glm::dmat3 r(1.0);
                for (int k = 0; k < 3; ++k)
                    if (scale[k] > 1e-12)
                        r[k] = D3(m[k]) / scale[k];
                c.out("location", constantVector(D3(m[3])));
                c.out("rotation", constantVector(matrixToEuler(r), T::Rotation));
                c.out("scale", constantVector(scale));
            },
    });
}
} // namespace lila::gn::nodes::fieldnodes

namespace lila::gn::nodes
{
void registerFieldNodes()
{
    fieldnodes::accumulate();
    fieldnodes::evaluateAtIndex();
    fieldnodes::evaluateOnDomain();
    fieldnodes::blur();
    fieldnodes::removeNamedAttribute();
    fieldnodes::hashValue();
    fieldnodes::intOpNode("IntegerMath", "Integer Math", "Integer math on values or fields. Division by zero gives zero.", fieldnodes::intOps, "add");
    fieldnodes::intOpNode("BitMath", "Bit Math", "Bitwise operations on 32-bit integers. Shift moves left for positive b and right for negative; rotate wraps bits around.",
              fieldnodes::bitOps, "and");
    fieldnodes::indexSwitch();
    fieldnodes::menuSwitch();
    fieldnodes::rotationNodes();
    fieldnodes::stringNodes();
    fieldnodes::warningNode();
    fieldnodes::selfObject();
}
} // namespace lila::gn::nodes
