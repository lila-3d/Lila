#include "gn/Field.h"

#include "core/Profile.h"
#include "geo/Normals.h"

#include <cmath>

namespace lila::gn
{
namespace
{
using geo::Index;

template <class T> T fromDouble(double d)
{
    if constexpr (std::is_same_v<T, double>)
        return d;
    else if constexpr (std::is_same_v<T, int64_t>)
        return int64_t(std::llround(d));
    else if constexpr (std::is_same_v<T, uint8_t>)
        return d != 0.0 ? 1 : 0;
    else
        return glm::dvec3(d);
}

template <class S> double toDouble(const S& s)
{
    if constexpr (std::is_same_v<S, glm::dvec3>)
        return (s.x + s.y + s.z) / 3.0;
    else
        return double(s);
}

template <class T, class S> T convertOne(const S& s)
{
    if constexpr (std::is_same_v<T, S>)
        return s;
    else if constexpr (std::is_same_v<T, glm::dvec3>)
        return glm::dvec3(toDouble(s));
    else if constexpr (std::is_same_v<T, int64_t> && !std::is_same_v<S, glm::dvec3>)
        return int64_t(std::llround(double(s)));
    else
        return fromDouble<T>(toDouble(s));
}

template <class T> Column convertTo(const Column& c)
{
    return std::visit(
        [&](const auto& v) -> Column {
            using S = typename std::decay_t<decltype(v)>::value_type;
            if constexpr (std::is_same_v<S, T>)
                return c;
            else
            {
                std::vector<T> out(v.size());
                for (size_t k = 0; k < v.size(); ++k)
                    out[k] = convertOne<T>(v[k]);
                return {std::move(out), c.single};
            }
        },
        c.data);
}

class ConstantField final : public Field
{
public:
    ConstantField(SocketType t, Column v) : Field(t), mValue(std::move(v)) {}
    Column evaluate(const FieldContext&) const override { return mValue; }
    bool constant() const override { return true; }

private:
    Column mValue;
};

class ConvertField final : public Field
{
public:
    ConvertField(FieldPtr f, SocketType t) : Field(t), mInner(std::move(f)) {}
    Column evaluate(const FieldContext& ctx) const override { return convert(mInner->evaluate(ctx), type()); }
    bool constant() const override { return mInner->constant(); }

private:
    FieldPtr mInner;
};

Column indexColumn(size_t n)
{
    std::vector<int64_t> out(n);
    for (size_t k = 0; k < n; ++k)
        out[k] = int64_t(k);
    return Column::of(std::move(out));
}

/** Normals of a mesh on one domain. */
Column meshNormals(const geo::Mesh& m, Domain d)
{
    LILA_ZONE();

    switch (d)
    {
    case Domain::Point:
    {
        std::vector<glm::vec3> pn = geo::pointNormals(m);
        std::vector<glm::dvec3> out(pn.begin(), pn.end());
        return Column::of(std::move(out));
    }
    case Domain::Corner:
    {
        std::vector<glm::vec3> cn = geo::cornerNormals(m);
        std::vector<glm::dvec3> out(cn.begin(), cn.end());
        return Column::of(std::move(out));
    }
    case Domain::Face:
    {
        std::vector<glm::dvec3> fn = geo::faceNormals(m);
        return Column::of(std::move(fn));
    }
    default:
    {
        Column faces = meshNormals(m, Domain::Face);
        Column e = adaptDomain(m, faces, Domain::Face, Domain::Edge);
        for (glm::dvec3& n : e.get<glm::dvec3>())
            if (glm::length(n) > 1e-12)
                n = glm::normalize(n);
        return e;
    }
    }
}

class PositionField final : public Field
{
public:
    PositionField() : Field(SocketType::Vector) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        switch (ctx.component)
        {
        case Component::Mesh:
        case Component::Points:
        {
            std::vector<glm::dvec3> p(ctx.mesh->positions().begin(), ctx.mesh->positions().end());
            Column c = Column::of(std::move(p));
            return ctx.component == Component::Mesh ? adaptDomain(*ctx.mesh, c, Domain::Point, ctx.domain) : c;
        }
        case Component::Curve: return adaptCurveDomain(*ctx.curves, Column::of(ctx.curves->positions), Domain::Point, ctx.domain);
        case Component::Instances:
        {
            std::vector<glm::dvec3> p(ctx.instances->size());
            for (size_t k = 0; k < p.size(); ++k)
                p[k] = glm::dvec3(ctx.instances->transforms[k][3]);
            return Column::of(std::move(p));
        }
        default: return Column::of(glm::dvec3(0.0));
        }
    }
};

class NormalField final : public Field
{
public:
    NormalField() : Field(SocketType::Vector) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        switch (ctx.component)
        {
        case Component::Mesh: return meshNormals(*ctx.mesh, ctx.domain);
        case Component::Curve:
        {
            std::vector<glm::dvec3> n(ctx.curves->numPoints());
            for (size_t c = 0; c < ctx.curves->numCurves(); ++c)
            {
                std::vector<glm::dvec3> t, nn;
                curveFrames(ctx.curves->curvePoints(c), ctx.curves->cyclic[c] != 0, t, nn);
                std::copy(nn.begin(), nn.end(), n.begin() + ctx.curves->offsets[c]);
            }
            return adaptCurveDomain(*ctx.curves, Column::of(std::move(n)), Domain::Point, ctx.domain);
        }
        default: return Column::of(glm::dvec3(0.0, 1.0, 0.0));
        }
    }
};

class IndexField final : public Field
{
public:
    IndexField() : Field(SocketType::Int) {}
    Column evaluate(const FieldContext& ctx) const override { return ctx.component == Component::None ? Column::of(int64_t(0)) : indexColumn(ctx.size); }
};

class AttributeField final : public Field
{
public:
    AttributeField(std::string name, SocketType t, Column fallback, bool indexFallback)
        : Field(t), mName(std::move(name)), mFallback(std::move(fallback)), mIndexFallback(indexFallback) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        Domain from = ctx.domain;
        std::optional<Column> c = readAttribute(ctx, mName, &from);
        if (!c)
            return mIndexFallback && ctx.component != Component::None ? indexColumn(ctx.size) : mFallback;
        Column adapted = *c;
        if (from != ctx.domain)
        {
            if (ctx.component == Component::Mesh)
                adapted = adaptDomain(*ctx.mesh, *c, from, ctx.domain);
            else if (ctx.component == Component::Curve)
                adapted = adaptCurveDomain(*ctx.curves, *c, from, ctx.domain);
        }
        return convert(adapted, type());
    }

private:
    std::string mName;
    Column mFallback;
    bool mIndexFallback;
};

class AttributeExistsField final : public Field
{
public:
    explicit AttributeExistsField(std::string name) : Field(SocketType::Bool), mName(std::move(name)) {}
    Column evaluate(const FieldContext& ctx) const override
    {
        Domain d;
        return Column::of(uint8_t(readAttribute(ctx, mName, &d) ? 1 : 0));
    }

private:
    std::string mName;
};

Column fromLayer(const geo::Attribute& a)
{
    size_t n = a.size();
    switch (a.type())
    {
    case geo::AttrType::Float:
    {
        auto v = a.values<float>();
        return Column::of(std::vector<double>(v.begin(), v.end()));
    }
    case geo::AttrType::Float2:
    {
        auto v = a.values<glm::vec2>();
        std::vector<glm::dvec3> out(n);
        for (size_t k = 0; k < n; ++k)
            out[k] = glm::dvec3(v[k].x, v[k].y, 0.0);
        return Column::of(std::move(out));
    }
    case geo::AttrType::Float3:
    {
        auto v = a.values<glm::vec3>();
        return Column::of(std::vector<glm::dvec3>(v.begin(), v.end()));
    }
    case geo::AttrType::Float4:
    {
        auto v = a.values<glm::vec4>();
        std::vector<glm::dvec3> out(n);
        for (size_t k = 0; k < n; ++k)
            out[k] = glm::dvec3(v[k]);
        return Column::of(std::move(out));
    }
    case geo::AttrType::Int:
    {
        auto v = a.values<int32_t>();
        return Column::of(std::vector<int64_t>(v.begin(), v.end()));
    }
    case geo::AttrType::Bool:
    {
        auto v = a.values<bool>();
        std::vector<uint8_t> out(n);
        for (size_t k = 0; k < n; ++k)
            out[k] = v[k] ? 1 : 0;
        return Column::of(std::move(out));
    }
    }
    return Column::of(0.0);
}

geo::AttrType layerTypeFor(SocketType t)
{
    switch (t)
    {
    case SocketType::Int: return geo::AttrType::Int;
    case SocketType::Bool: return geo::AttrType::Bool;
    case SocketType::Vector:
    case SocketType::Rotation: return geo::AttrType::Float3;
    default: return geo::AttrType::Float;
    }
}

/** Fills a layer from a column. Existing Float2/Float4 layers keep their layout (uv sets, colours). */
void fillLayer(geo::AttributeSet& set, std::string_view name, const Column& values, SocketType type, size_t n)
{
    geo::Attribute* existing = set.find(name);
    geo::AttrType want = layerTypeFor(type);
    if (existing && (existing->type() == geo::AttrType::Float2 || existing->type() == geo::AttrType::Float4) &&
        (type == SocketType::Vector || type == SocketType::Rotation))
        want = existing->type();
    geo::Attribute& a = set.add(name, want);
    switch (want)
    {
    case geo::AttrType::Float:
    {
        auto v = expand<double>(convert(values, SocketType::Float), n);
        auto dst = a.values<float>();
        for (size_t k = 0; k < n && k < dst.size(); ++k)
            dst[k] = float(v[k]);
        break;
    }
    case geo::AttrType::Int:
    {
        auto v = expand<int64_t>(convert(values, SocketType::Int), n);
        auto dst = a.values<int32_t>();
        for (size_t k = 0; k < n && k < dst.size(); ++k)
            dst[k] = int32_t(v[k]);
        break;
    }
    case geo::AttrType::Bool:
    {
        auto v = expand<uint8_t>(convert(values, SocketType::Bool), n);
        auto dst = a.values<bool>();
        for (size_t k = 0; k < n && k < dst.size(); ++k)
            dst[k] = v[k] != 0;
        break;
    }
    default:
    {
        auto v = expand<glm::dvec3>(convert(values, SocketType::Vector), n);
        if (want == geo::AttrType::Float2)
        {
            auto dst = a.values<glm::vec2>();
            for (size_t k = 0; k < n && k < dst.size(); ++k)
                dst[k] = glm::vec2(v[k]);
        }
        else if (want == geo::AttrType::Float4)
        {
            auto dst = a.values<glm::vec4>();
            for (size_t k = 0; k < n && k < dst.size(); ++k)
                dst[k] = glm::vec4(glm::vec3(v[k]), dst[k].w == 0.0f ? 1.0f : dst[k].w);
        }
        else
        {
            auto dst = a.values<glm::vec3>();
            for (size_t k = 0; k < n && k < dst.size(); ++k)
                dst[k] = glm::vec3(v[k]);
        }
        break;
    }
    }
}

template <class T> T zero()
{
    if constexpr (std::is_same_v<T, glm::dvec3>)
        return glm::dvec3(0.0);
    else
        return T(0);
}

/** Averages (or ANDs / ORs bools) source values into each target element. */
template <class T, class Gather> std::vector<T> gatherMix(size_t n, const std::vector<T>& src, bool singleSrc, bool boolAll, Gather gather)
{
    std::vector<T> out(n);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 1024), [&](const tbb::blocked_range<size_t>& r) {
        std::vector<Index> ids;
        for (size_t t = r.begin(); t < r.end(); ++t)
        {
            ids.clear();
            gather(t, ids);
            if (ids.empty())
            {
                out[t] = zero<T>();
                continue;
            }
            auto val = [&](Index i) { return src[singleSrc ? 0 : i]; };
            if constexpr (std::is_same_v<T, uint8_t>)
            {
                bool acc = boolAll;
                for (Index i : ids)
                    acc = boolAll ? (acc && val(i)) : (acc || val(i));
                out[t] = acc ? 1 : 0;
            }
            else if constexpr (std::is_same_v<T, int64_t>)
            {
                double s = 0.0;
                for (Index i : ids)
                    s += double(val(i));
                out[t] = int64_t(std::llround(s / double(ids.size())));
            }
            else
            {
                T s = zero<T>();
                for (Index i : ids)
                    s += val(i);
                out[t] = s / double(ids.size());
            }
        }
    });
    return out;
}
} // namespace

size_t Column::size() const
{
    return std::visit([](const auto& v) { return v.size(); }, data);
}

Column convert(const Column& c, SocketType to)
{
    switch (to)
    {
    case SocketType::Float: return convertTo<double>(c);
    case SocketType::Int: return convertTo<int64_t>(c);
    case SocketType::Bool: return convertTo<uint8_t>(c);
    case SocketType::Vector:
    case SocketType::Rotation: return convertTo<glm::dvec3>(c);
    default: return c;
    }
}

FieldContext FieldContext::forMesh(const geo::Mesh& m, Domain d)
{
    FieldContext c;
    c.component = Component::Mesh;
    c.mesh = &m;
    c.domain = d;
    c.size = domainSize(c, d);
    return c;
}

FieldContext FieldContext::forPoints(const geo::Mesh& m)
{
    FieldContext c;
    c.component = Component::Points;
    c.mesh = &m;
    c.domain = Domain::Point;
    c.size = m.pointCapacity();
    return c;
}

FieldContext FieldContext::forCurves(const CurveSet& cs, Domain d)
{
    FieldContext c;
    c.component = Component::Curve;
    c.curves = &cs;
    c.domain = d == Domain::Curve ? Domain::Curve : Domain::Point;
    c.size = domainSize(c, c.domain);
    return c;
}

FieldContext FieldContext::forInstances(const Instances& i)
{
    FieldContext c;
    c.component = Component::Instances;
    c.instances = &i;
    c.domain = Domain::Instance;
    c.size = i.size();
    return c;
}

size_t domainSize(const FieldContext& ctx, Domain d)
{
    switch (ctx.component)
    {
    case Component::Mesh:
        switch (d)
        {
        case Domain::Point: return ctx.mesh->pointCapacity();
        case Domain::Edge: return ctx.mesh->edgeCapacity();
        case Domain::Face: return ctx.mesh->faceCapacity();
        case Domain::Corner: return ctx.mesh->cornerCapacity();
        default: return 0;
        }
    case Component::Points: return ctx.mesh->pointCapacity();
    case Component::Curve: return d == Domain::Curve ? ctx.curves->numCurves() : ctx.curves->numPoints();
    case Component::Instances: return ctx.instances->size();
    default: return 1;
    }
}

FieldPtr constantField(SocketType type, Column value) { return std::make_shared<ConstantField>(type, convert(value, type)); }
FieldPtr constantFloat(double v) { return constantField(SocketType::Float, Column::of(v)); }
FieldPtr constantInt(int64_t v) { return constantField(SocketType::Int, Column::of(v)); }
FieldPtr constantBool(bool v) { return constantField(SocketType::Bool, Column::of(uint8_t(v ? 1 : 0))); }
FieldPtr constantVector(const glm::dvec3& v, SocketType type) { return constantField(type, Column::of(v)); }

FieldPtr convertField(const FieldPtr& f, SocketType to)
{
    if (!f || f->type() == to || !isDataType(to))
        return f;
    return std::make_shared<ConvertField>(f, to);
}

Column evaluate(const FieldPtr& f, const FieldContext& ctx, SocketType as)
{
    if (!f)
        return convert(Column::of(0.0), as);
    return convert(f->evaluate(ctx), as);
}

std::vector<Index> selected(const FieldPtr& selection, const FieldContext& ctx)
{
    Column c = evaluate(selection, ctx, SocketType::Bool);
    std::vector<Index> out;
    const auto& v = c.get<uint8_t>();
    if (c.single)
    {
        if (!v.empty() && v[0])
            for (size_t k = 0; k < ctx.size; ++k)
                out.push_back(Index(k));
        return out;
    }
    for (size_t k = 0; k < v.size(); ++k)
        if (v[k])
            out.push_back(Index(k));
    return out;
}

FieldPtr positionField()
{
    static FieldPtr f = std::make_shared<PositionField>();
    return f;
}

FieldPtr normalField()
{
    static FieldPtr f = std::make_shared<NormalField>();
    return f;
}

FieldPtr indexField()
{
    static FieldPtr f = std::make_shared<IndexField>();
    return f;
}

FieldPtr idField()
{
    static FieldPtr f = std::make_shared<AttributeField>("id", SocketType::Int, Column::of(int64_t(0)), true);
    return f;
}

FieldPtr radiusField()
{
    static FieldPtr f = std::make_shared<AttributeField>("radius", SocketType::Float, Column::of(1.0), false);
    return f;
}

FieldPtr attributeField(std::string name, SocketType type)
{
    if (name == "position")
        return convertField(positionField(), type);
    return std::make_shared<AttributeField>(std::move(name), type, convert(Column::of(0.0), type), false);
}

FieldPtr attributeExistsField(std::string name) { return std::make_shared<AttributeExistsField>(std::move(name)); }

FunctionField::FunctionField(SocketType type, std::vector<FieldPtr> inputs, std::vector<SocketType> inputTypes, Fn fn)
    : Field(type), mInputs(std::move(inputs)), mInputTypes(std::move(inputTypes)), mFn(std::move(fn))
{
    for (const FieldPtr& f : mInputs)
        if (f && !f->constant())
            mConstant = false;
}

Column FunctionField::evaluate(const FieldContext& ctx) const
{
    std::vector<Column> in(mInputs.size());
    for (size_t i = 0; i < mInputs.size(); ++i)
        in[i] = gn::evaluate(mInputs[i], ctx, mInputTypes[i]);
    return convert(mFn(in, ctx.size), type());
}

Column adaptDomain(const geo::Mesh& m, const Column& c, Domain from, Domain to)
{
    LILA_ZONE();

    if (from == to)
        return c;
    if (c.single)
        return c;
    FieldContext ctx = FieldContext::forMesh(m, to);
    size_t n = ctx.size;
    bool all = to == Domain::Face || (to == Domain::Edge && from != Domain::Face);

    auto gather = [&](size_t t, std::vector<Index>& ids) {
        Index e = Index(t);
        switch (to)
        {
        case Domain::Point:
            if (!m.pointValid(e))
                return;
            if (from == Domain::Edge)
                m.forEachPointEdge(e, [&](Index x) { ids.push_back(x); });
            else if (from == Domain::Face)
                m.forEachPointCorner(e, [&](Index x) { ids.push_back(m.cornerFace(x)); });
            else
                m.forEachPointCorner(e, [&](Index x) { ids.push_back(x); });
            return;
        case Domain::Edge:
            if (!m.edgeValid(e))
                return;
            if (from == Domain::Point)
            {
                ids.push_back(m.edgePoint(e, 0));
                ids.push_back(m.edgePoint(e, 1));
            }
            else if (from == Domain::Face)
                m.forEachEdgeCorner(e, [&](Index x) { ids.push_back(m.cornerFace(x)); });
            else
                m.forEachEdgeCorner(e, [&](Index x) {
                    ids.push_back(x);
                    ids.push_back(m.cornerNext(x));
                });
            return;
        case Domain::Face:
            if (!m.faceValid(e))
                return;
            m.forEachFaceCorner(e, [&](Index x) {
                ids.push_back(from == Domain::Point ? m.cornerPoint(x) : from == Domain::Edge ? m.cornerEdge(x) : x);
            });
            return;
        case Domain::Corner:
            if (!m.cornerValid(e))
                return;
            if (from == Domain::Point)
                ids.push_back(m.cornerPoint(e));
            else if (from == Domain::Face)
                ids.push_back(m.cornerFace(e));
            else
            {
                ids.push_back(m.cornerEdge(e));
                ids.push_back(m.cornerEdge(m.cornerPrev(e)));
            }
            return;
        default: return;
        }
    };

    return std::visit(
        [&](const auto& v) -> Column {
            using T = typename std::decay_t<decltype(v)>::value_type;
            return Column::of(gatherMix<T>(n, v, false, all, gather));
        },
        c.data);
}

Column adaptCurveDomain(const CurveSet& cs, const Column& c, Domain from, Domain to)
{
    if (from == to || c.single)
        return c;
    if (to == Domain::Curve)
        return std::visit(
            [&](const auto& v) -> Column {
                using T = typename std::decay_t<decltype(v)>::value_type;
                return Column::of(gatherMix<T>(cs.numCurves(), v, false, true, [&](size_t t, std::vector<Index>& ids) {
                    for (Index p = cs.offsets[t]; p < cs.offsets[t + 1]; ++p)
                        ids.push_back(p);
                }));
            },
            c.data);
    return std::visit(
        [&](const auto& v) -> Column {
            using T = typename std::decay_t<decltype(v)>::value_type;
            std::vector<T> out(cs.numPoints());
            for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                for (Index p = cs.offsets[cu]; p < cs.offsets[cu + 1]; ++p)
                    out[p] = v[cu];
            return Column::of(std::move(out));
        },
        c.data);
}

std::optional<Column> readAttribute(const FieldContext& ctx, std::string_view name, Domain* domainOut)
{
    switch (ctx.component)
    {
    case Component::Mesh:
    case Component::Points:
    {
        if (name == "position")
        {
            *domainOut = Domain::Point;
            return Column::of(std::vector<glm::dvec3>(ctx.mesh->positions().begin(), ctx.mesh->positions().end()));
        }
        // The context's own domain wins when a name exists on several.
        static constexpr Domain kOrder[] = {Domain::Point, Domain::Edge, Domain::Face, Domain::Corner};
        if (ctx.domain <= Domain::Corner)
            if (const geo::Attribute* a = ctx.mesh->attributes(geo::Domain(ctx.domain)).find(name))
            {
                *domainOut = ctx.domain;
                return fromLayer(*a);
            }
        for (Domain d : kOrder)
            if (const geo::Attribute* a = ctx.mesh->attributes(geo::Domain(d)).find(name))
            {
                *domainOut = d;
                return fromLayer(*a);
            }
        return std::nullopt;
    }
    case Component::Curve:
        if (name == "position")
        {
            *domainOut = Domain::Point;
            return Column::of(ctx.curves->positions);
        }
        if (const geo::Attribute* a = ctx.curves->pointAttrs.find(name))
        {
            *domainOut = Domain::Point;
            return fromLayer(*a);
        }
        if (const geo::Attribute* a = ctx.curves->curveAttrs.find(name))
        {
            *domainOut = Domain::Curve;
            return fromLayer(*a);
        }
        return std::nullopt;
    case Component::Instances:
        if (const geo::Attribute* a = ctx.instances->attrs.find(name))
        {
            *domainOut = Domain::Instance;
            return fromLayer(*a);
        }
        return std::nullopt;
    default: return std::nullopt;
    }
}

void writeAttribute(geo::Mesh& m, std::string_view name, Domain d, const Column& values, SocketType type)
{
    if (d > Domain::Corner)
        d = Domain::Point;
    FieldContext ctx = FieldContext::forMesh(m, d);
    if (name == "position" && d == Domain::Point)
    {
        auto v = expand<glm::dvec3>(convert(values, SocketType::Vector), ctx.size);
        for (Index p = 0; p < m.pointCapacity(); ++p)
            m.setPosition(p, v[p]);
        return;
    }
    for (int k = 0; k < 4; ++k)
        if (Domain(k) != d)
            m.attributes(geo::Domain(k)).remove(name);
    fillLayer(m.attributes(geo::Domain(d)), name, values, type, ctx.size);
}

void writeAttribute(CurveSet& c, std::string_view name, Domain d, const Column& values, SocketType type)
{
    if (name == "position" && d == Domain::Point)
    {
        c.positions = expand<glm::dvec3>(convert(values, SocketType::Vector), c.numPoints());
        return;
    }
    (d == Domain::Curve ? c.pointAttrs : c.curveAttrs).remove(name);
    fillLayer(d == Domain::Curve ? c.curveAttrs : c.pointAttrs, name, values, type, d == Domain::Curve ? c.numCurves() : c.numPoints());
}

void writeAttribute(Instances& inst, std::string_view name, const Column& values, SocketType type)
{
    fillLayer(inst.attrs, name, values, type, inst.size());
}

std::vector<Domain> componentDomains(Component c)
{
    switch (c)
    {
    case Component::Mesh: return {Domain::Point, Domain::Edge, Domain::Face, Domain::Corner};
    case Component::Points: return {Domain::Point};
    case Component::Curve: return {Domain::Point, Domain::Curve};
    case Component::Instances: return {Domain::Instance};
    default: return {};
    }
}

uint32_t hash32(uint32_t a, uint32_t b, uint32_t c)
{
    // Jenkins' lookup3 final mix.
    auto rot = [](uint32_t x, int k) { return (x << k) | (x >> (32 - k)); };
    a += 0xdeadbeef;
    b += 0x9e3779b9;
    c += 0x85ebca6b;
    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);
    return c;
}

double hashToUnit(uint32_t h) { return double(h) / 4294967296.0; }

void curveFrames(std::span<const glm::dvec3> pts, bool cyclic, std::vector<glm::dvec3>& tangents, std::vector<glm::dvec3>& normals)
{
    size_t n = pts.size();
    tangents.assign(n, glm::dvec3(0.0, 0.0, 1.0));
    normals.assign(n, glm::dvec3(0.0, 1.0, 0.0));
    if (n < 2)
        return;
    for (size_t k = 0; k < n; ++k)
    {
        glm::dvec3 prev = k > 0 ? pts[k - 1] : (cyclic ? pts[n - 1] : pts[k]);
        glm::dvec3 next = k + 1 < n ? pts[k + 1] : (cyclic ? pts[0] : pts[k]);
        glm::dvec3 t = next - prev;
        tangents[k] = glm::length(t) > 1e-12 ? glm::normalize(t) : (k > 0 ? tangents[k - 1] : glm::dvec3(0.0, 0.0, 1.0));
    }
    // Start from the axis least aligned with the first tangent, then parallel-transport it along the curve.
    glm::dvec3 t0 = tangents[0];
    glm::dvec3 a = std::abs(t0.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
    glm::dvec3 nrm = glm::normalize(glm::cross(glm::cross(t0, a), t0));
    normals[0] = nrm;
    for (size_t k = 1; k < n; ++k)
    {
        glm::dvec3 axis = glm::cross(tangents[k - 1], tangents[k]);
        double s = glm::length(axis);
        if (s > 1e-12)
        {
            axis /= s;
            double ang = std::atan2(s, glm::dot(tangents[k - 1], tangents[k]));
            nrm = nrm * std::cos(ang) + glm::cross(axis, nrm) * std::sin(ang) + axis * glm::dot(axis, nrm) * (1.0 - std::cos(ang));
        }
        nrm = glm::normalize(nrm - tangents[k] * glm::dot(nrm, tangents[k]));
        normals[k] = nrm;
    }
}
} // namespace lila::gn
