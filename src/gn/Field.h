#pragma once

#include "core/Export.h"

#include "gn/Geometry.h"

#include <tbb/parallel_for.h>

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <variant>

namespace lila::gn
{
/** Storage per socket type: Float double, Int int64_t, Bool uint8_t, Vector and Rotation dvec3. */
using ColumnData = std::variant<std::vector<double>, std::vector<int64_t>, std::vector<uint8_t>, std::vector<glm::dvec3>>;

/** One value per element, or one value for all of them when single. */
struct LILA_SCENE_API Column
{
    ColumnData data = std::vector<double>{0.0};
    bool single = true;

    size_t size() const;
    template <class T> const std::vector<T>& get() const { return std::get<std::vector<T>>(data); }
    template <class T> std::vector<T>& get() { return std::get<std::vector<T>>(data); }
    template <class T> T at(size_t k) const
    {
        const auto& v = std::get<std::vector<T>>(data);
        return v[single ? 0 : k];
    }

    template <class T> static Column of(T v) { return {std::vector<T>{v}, true}; }
    template <class T> static Column of(std::vector<T> v) { return {std::move(v), false}; }
};

template <class T> constexpr SocketType socketOf()
{
    if constexpr (std::is_same_v<T, double>)
        return SocketType::Float;
    else if constexpr (std::is_same_v<T, int64_t>)
        return SocketType::Int;
    else if constexpr (std::is_same_v<T, uint8_t>)
        return SocketType::Bool;
    else
        return SocketType::Vector;
}

/** Converts storage: Blender's implicit conversions (vector to float is the average, float to vector splats). */
LILA_SCENE_API Column convert(const Column& c, SocketType to);
/** Every element, broadcasting a single value. */
template <class T> std::vector<T> expand(const Column& c, size_t n)
{
    const auto& v = c.get<T>();
    if (!c.single)
        return v;
    return std::vector<T>(n, v.empty() ? T{} : v[0]);
}

enum class Component : uint8_t
{
    Mesh,
    Points,
    Curve,
    Instances,
    None,
};

/** Where a field is evaluated: one component of a geometry, on one of its domains. */
struct LILA_SCENE_API FieldContext
{
    Component component = Component::None;
    Domain domain = Domain::Point;
    const geo::Mesh* mesh = nullptr; // Mesh and Points
    const CurveSet* curves = nullptr;
    const Instances* instances = nullptr;
    size_t size = 1;

    static FieldContext forMesh(const geo::Mesh& m, Domain d);
    static FieldContext forPoints(const geo::Mesh& m);
    static FieldContext forCurves(const CurveSet& c, Domain d);
    static FieldContext forInstances(const Instances& i);
};

LILA_SCENE_API size_t domainSize(const FieldContext& ctx, Domain d);

class Field
{
public:
    explicit Field(SocketType type) : mType(type) {}
    virtual ~Field() = default;

    SocketType type() const { return mType; }
    virtual Column evaluate(const FieldContext& ctx) const = 0;
    /** Doesn't read the context, so it gives the same single value everywhere. */
    virtual bool constant() const { return false; }

private:
    SocketType mType;
};

using FieldPtr = std::shared_ptr<const Field>;

LILA_SCENE_API FieldPtr constantField(SocketType type, Column value);
LILA_SCENE_API FieldPtr constantFloat(double v);
LILA_SCENE_API FieldPtr constantInt(int64_t v);
LILA_SCENE_API FieldPtr constantBool(bool v);
LILA_SCENE_API FieldPtr constantVector(const glm::dvec3& v, SocketType type = SocketType::Vector);
/** Wraps a field so it evaluates to another socket type. */
LILA_SCENE_API FieldPtr convertField(const FieldPtr& f, SocketType to);

/** Evaluates and converts to `as`, sized to the context unless single. */
LILA_SCENE_API Column evaluate(const FieldPtr& f, const FieldContext& ctx, SocketType as);
template <class T> std::vector<T> evaluateAll(const FieldPtr& f, const FieldContext& ctx)
{
    return expand<T>(evaluate(f, ctx, socketOf<T>()), ctx.size);
}
/** Value outside any geometry: inputs like Position read as zero. */
template <class T> T evaluateSingle(const FieldPtr& f)
{
    FieldContext none;
    return evaluate(f, none, socketOf<T>()).template at<T>(0);
}
/** Indices of elements where the bool field is true. */
LILA_SCENE_API std::vector<geo::Index> selected(const FieldPtr& selection, const FieldContext& ctx);

// Built-in inputs.
LILA_SCENE_API FieldPtr positionField();
LILA_SCENE_API FieldPtr normalField();
LILA_SCENE_API FieldPtr indexField();
/** The "id" attribute when present, otherwise the index. */
LILA_SCENE_API FieldPtr idField();
LILA_SCENE_API FieldPtr radiusField();
/** Reads an attribute by name on whatever domain it lives, adapted to the context's domain. */
LILA_SCENE_API FieldPtr attributeField(std::string name, SocketType type);
/** True where the attribute exists (single per context). */
LILA_SCENE_API FieldPtr attributeExistsField(std::string name);

/** Per-element function of input fields, run in parallel. */
class LILA_SCENE_API FunctionField : public Field
{
public:
    using Fn = std::function<Column(const std::vector<Column>& inputs, size_t n)>;
    FunctionField(SocketType type, std::vector<FieldPtr> inputs, std::vector<SocketType> inputTypes, Fn fn);

    Column evaluate(const FieldContext& ctx) const override;
    bool constant() const override { return mConstant; }

private:
    std::vector<FieldPtr> mInputs;
    std::vector<SocketType> mInputTypes;
    Fn mFn;
    bool mConstant = true;
};

namespace detail
{
template <class R, class... A, class F, size_t... I>
Column runMap(const F& fn, const std::vector<Column>& in, size_t n, std::index_sequence<I...>)
{
    bool single = (in[I].single && ...);
    size_t count = single ? 1 : n;
    std::vector<R> out(count);
    auto body = [&](size_t k) { out[k] = R(fn(in[I].template at<A>(k)...)); };
    if (count > 2048)
        tbb::parallel_for(size_t(0), count, body);
    else
        for (size_t k = 0; k < count; ++k)
            body(k);
    return {std::move(out), single};
}
} // namespace detail

/** A field computing fn(a, b, ...) per element with typed arguments. */
template <class R, class... A, class F> FieldPtr map(SocketType outType, F fn, std::array<FieldPtr, sizeof...(A)> inputs)
{
    std::vector<SocketType> types = {socketOf<A>()...};
    return std::make_shared<FunctionField>(
        outType, std::vector<FieldPtr>(inputs.begin(), inputs.end()), std::move(types),
        [fn](const std::vector<Column>& in, size_t n) { return detail::runMap<R, A...>(fn, in, n, std::index_sequence_for<A...>{}); });
}

/** Moves a value column between mesh domains: averages where several elements meet. */
LILA_SCENE_API Column adaptDomain(const geo::Mesh& m, const Column& c, Domain from, Domain to);
LILA_SCENE_API Column adaptCurveDomain(const CurveSet& cs, const Column& c, Domain from, Domain to);

/** Raw attribute lookup: values on the domain the attribute lives on. */
LILA_SCENE_API std::optional<Column> readAttribute(const FieldContext& ctx, std::string_view name, Domain* domainOut);

/** Writes a column as an attribute on a mesh domain (replacing any same-named layer on other domains). */
LILA_SCENE_API void writeAttribute(geo::Mesh& m, std::string_view name, Domain d, const Column& values, SocketType type);
LILA_SCENE_API void writeAttribute(CurveSet& c, std::string_view name, Domain d, const Column& values, SocketType type);
LILA_SCENE_API void writeAttribute(Instances& inst, std::string_view name, const Column& values, SocketType type);

/** Domains each component offers, first is its natural one. */
LILA_SCENE_API std::vector<Domain> componentDomains(Component c);

/** Tangents and rotation-minimizing normals along a poly curve. */
LILA_SCENE_API void curveFrames(std::span<const glm::dvec3> pts, bool cyclic, std::vector<glm::dvec3>& tangents, std::vector<glm::dvec3>& normals);

/** Integer hash for seeded random values: stable per (id, seed). */
LILA_SCENE_API uint32_t hash32(uint32_t a, uint32_t b = 0, uint32_t c = 0);
LILA_SCENE_API double hashToUnit(uint32_t h);
} // namespace lila::gn
