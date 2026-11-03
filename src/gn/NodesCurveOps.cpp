#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "curve/Ops.h"
#include "curve/Text.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <numeric>

namespace lila::gn::nodes::curveops
{
/** Arc length at each point; closed curves get one more entry, the length with the closing segment. */
std::vector<double> cumulative(std::span<const D3> p, bool cyclic)
{
    std::vector<double> acc(p.size() + (cyclic && p.size() > 1 ? 1 : 0), 0.0);
    for (size_t k = 1; k < acc.size(); ++k)
        acc[k] = acc[k - 1] + glm::distance(p[k - 1], p[k % p.size()]);
    return acc;
}

/** A point between two points of a curve set, t of the way from a to b. */
struct Mix
{
    Index a = 0, b = 0;
    double t = 0.0;
};

/** Where arc length s falls on a curve of n points starting at p0 (acc from cumulative). */
Mix mixAt(const std::vector<double>& acc, size_t n, Index p0, double s)
{
    if (n == 0 || acc.size() < 2)
        return {p0, p0, 0.0};
    s = std::clamp(s, 0.0, acc.back());
    size_t seg = size_t(std::upper_bound(acc.begin(), acc.end(), s) - acc.begin());
    seg = std::clamp<size_t>(seg, 1, acc.size() - 1) - 1;
    double len = acc[seg + 1] - acc[seg];
    double t = len > 0.0 ? (s - acc[seg]) / len : 0.0;
    return {p0 + Index(seg), p0 + Index((seg + 1) % n), std::clamp(t, 0.0, 1.0)};
}

/** An empty set with src's attribute layers, for building curves out of src's points. */
CurveSet layoutOf(const CurveSet& src)
{
    CurveSet out;
    out.pointAttrs.mergeLayout(src.pointAttrs);
    out.curveAttrs.mergeLayout(src.curveAttrs);
    return out;
}

/** Adds a poly curve whose points blend src's points, point attributes blended alike, curve attributes from src curve cu. */
void addMixed(CurveSet& out, const CurveSet& src, size_t cu, std::span<const Mix> mixes, bool cyclic)
{
    std::vector<D3> pts(mixes.size());
    for (size_t k = 0; k < mixes.size(); ++k)
        pts[k] = glm::mix(src.positions[mixes[k].a], src.positions[mixes[k].b], mixes[k].t);
    size_t q0 = out.positions.size();
    out.addCurve(pts, cyclic);
    out.curveAttrs.copyFrom(src.curveAttrs, out.numCurves() - 1, cu);
    for (size_t k = 0; k < mixes.size(); ++k)
    {
        Index srcs[2] = {mixes[k].a, mixes[k].b};
        float w[2] = {float(1.0 - mixes[k].t), float(mixes[k].t)};
        out.pointAttrs.mixFrom(src.pointAttrs, q0 + k, srcs, w);
    }
}

std::vector<Mix> wholeCurve(const CurveSet& cs, size_t cu)
{
    std::vector<Mix> m(cs.curveSize(cu));
    for (size_t k = 0; k < m.size(); ++k)
        m[k] = {cs.offsets[cu] + Index(k), cs.offsets[cu] + Index(k), 0.0};
    return m;
}

D3 rotateAbout(const D3& v, const D3& axis, double angle) { return angle == 0.0 ? v : axisAngle(axis, angle) * v; }

/** Unit tangents and normals (tilt applied) at every point of a poly set. */
void polyFrames(const CurveSet& cs, std::vector<D3>& tangents, std::vector<D3>& normals)
{
    tangents.assign(cs.numPoints(), D3(0.0, 0.0, 1.0));
    normals.assign(cs.numPoints(), D3(0.0, 1.0, 0.0));
    std::span<const float> tilt = cs.pointAttrs.get<float>("tilt");
    for (size_t cu = 0; cu < cs.numCurves(); ++cu)
    {
        std::vector<D3> t, n;
        curveFrames(cs.curvePoints(cu), cs.cyclic[cu] != 0, t, n);
        for (size_t k = 0; k < t.size(); ++k)
        {
            Index p = cs.offsets[cu] + Index(k);
            tangents[p] = t[k];
            normals[p] = tilt.empty() ? n[k] : rotateAbout(n[k], t[k], tilt[p]);
        }
    }
}

D3 mixUnit(const std::vector<D3>& v, const Mix& m)
{
    D3 d = glm::mix(v[m.a], v[m.b], m.t);
    double l = glm::length(d);
    return l > 1e-12 ? d / l : v[m.a];
}

/** Span parameter of control point i: anchors of poly and Bezier sit on whole spans, NURBS points spread over the spans. */
double controlParam(const curve::Spline& sp, size_t i)
{
    if (sp.kind != curve::Kind::Nurbs)
        return double(i);
    size_t n = sp.size();
    double spans = curve::spanCount(sp);
    return double(i) * spans / double(sp.closed ? n : std::max<size_t>(1, n - 1));
}

/** Arc length at span parameter u; uEnd is the parameter of the curve's end (a closed polyline doesn't repeat its start). */
double lengthAtParam(const curve::Polyline& pl, double u, double uEnd)
{
    if (pl.size() == 0)
        return 0.0;
    auto it = std::upper_bound(pl.params.begin(), pl.params.end(), u);
    if (it == pl.params.begin())
        return pl.arcLength.front();
    size_t i = size_t(it - pl.params.begin());
    if (i == pl.size())
    {
        double last = pl.params.back();
        if (!pl.closed || uEnd <= last)
            return pl.arcLength.back();
        return glm::mix(pl.arcLength.back(), pl.length, std::clamp((u - last) / (uEnd - last), 0.0, 1.0));
    }
    double u0 = pl.params[i - 1], u1 = pl.params[i];
    double t = u1 > u0 ? (u - u0) / (u1 - u0) : 0.0;
    return glm::mix(pl.arcLength[i - 1], pl.arcLength[i], t);
}

/** Nearest polyline point to span parameter u. */
size_t nearestSample(const curve::Polyline& pl, double u)
{
    auto it = std::lower_bound(pl.params.begin(), pl.params.end(), u);
    size_t i = std::min(size_t(it - pl.params.begin()), pl.size() - 1);
    if (i > 0 && std::abs(pl.params[i - 1] - u) < std::abs(pl.params[i] - u))
        --i;
    return i;
}

bool polyCurve(const CurveSet& cs, size_t c)
{
    std::span<const int32_t> kind = cs.curveAttrs.get<int32_t>("curve_type");
    return kind.empty() || kind[c] == int32_t(curve::Kind::Poly);
}

/** Arc length from the curve's start to every control point, and each curve's length (closing segment included). */
void curveLengths(const CurveSet& cs, std::vector<double>& atPoint, std::vector<double>& total)
{
    atPoint.assign(cs.numPoints(), 0.0);
    total.assign(cs.numCurves(), 0.0);
    for (size_t c = 0; c < cs.numCurves(); ++c)
    {
        size_t p0 = cs.offsets[c], n = cs.curveSize(c);
        if (n == 0)
            continue;
        if (polyCurve(cs, c))
        {
            std::vector<double> acc = cumulative(cs.curvePoints(c), cs.cyclic[c] != 0);
            std::copy_n(acc.begin(), n, atPoint.begin() + ptrdiff_t(p0));
            total[c] = acc.back();
            continue;
        }
        curve::Spline sp = cs.spline(c);
        if (!curve::isValid(sp))
            continue;
        curve::Polyline pl = curve::tessellate(sp);
        double uEnd = curve::spanCount(sp);
        total[c] = pl.length;
        for (size_t i = 0; i < n; ++i)
            atPoint[p0 + i] = lengthAtParam(pl, controlParam(sp, i), uEnd);
    }
}

/** Tangents and normals (tilt applied) at the control points; normalMode 1 keeps normals square to the up axis. */
void controlFrames(const CurveSet& cs, int normalMode, std::vector<D3>& tangents, std::vector<D3>& normals)
{
    tangents.assign(cs.numPoints(), D3(0.0, 0.0, 1.0));
    normals.assign(cs.numPoints(), D3(0.0, 1.0, 0.0));
    if (cs.isPoly())
        polyFrames(cs, tangents, normals);
    else
        for (size_t c = 0; c < cs.numCurves(); ++c)
        {
            size_t p0 = cs.offsets[c], n = cs.curveSize(c);
            if (polyCurve(cs, c))
            {
                std::vector<D3> t, nn;
                curveFrames(cs.curvePoints(c), cs.cyclic[c] != 0, t, nn);
                std::span<const float> tilt = cs.pointAttrs.get<float>("tilt");
                for (size_t k = 0; k < n; ++k)
                {
                    tangents[p0 + k] = t[k];
                    normals[p0 + k] = tilt.empty() ? nn[k] : rotateAbout(nn[k], t[k], tilt[p0 + k]);
                }
                continue;
            }
            curve::Spline sp = cs.spline(c);
            if (!curve::isValid(sp))
                continue;
            curve::Polyline pl = curve::tessellate(sp);
            if (pl.size() == 0)
                continue;
            std::vector<curve::Frame> fr = curve::frames(pl, {}, curve::tiltAlong(sp, pl));
            for (size_t i = 0; i < n; ++i)
            {
                const curve::Frame& f = fr[nearestSample(pl, controlParam(sp, i))];
                tangents[p0 + i] = f.t;
                normals[p0 + i] = f.n;
            }
        }
    if (normalMode == 1)
    {
        std::span<const float> tilt = cs.pointAttrs.get<float>("tilt");
        for (size_t p = 0; p < normals.size(); ++p)
        {
            D3 n(-tangents[p].z, 0.0, tangents[p].x);
            n = glm::length(n) > 1e-9 ? glm::normalize(n) : D3(1.0, 0.0, 0.0);
            normals[p] = tilt.empty() ? n : rotateAbout(n, tangents[p], tilt[p]);
        }
    }
}

/** A field over the control points of the curve being evaluated, moved to the context's domain; fallback elsewhere. */
template <class V, class Fn> Column onCurvePoints(const FieldContext& ctx, V fallback, Fn&& perPoint)
{
    if (ctx.component != Component::Curve)
        return Column::of(fallback);
    std::vector<V> v = perPoint(*ctx.curves);
    return adaptCurveDomain(*ctx.curves, Column::of(std::move(v)), Domain::Point, ctx.domain);
}

class ParameterField final : public Field
{
public:
    enum Which
    {
        Factor,
        Length,
        IndexOnCurve,
    };
    explicit ParameterField(Which w) : Field(w == IndexOnCurve ? SocketType::Int : SocketType::Float), mWhich(w) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve)
            return mWhich == IndexOnCurve ? Column::of(int64_t(0)) : Column::of(0.0);
        const CurveSet& cs = *ctx.curves;
        if (mWhich == IndexOnCurve)
        {
            std::vector<int64_t> idx(cs.numPoints());
            for (size_t c = 0; c < cs.numCurves(); ++c)
                for (Index p = cs.offsets[c]; p < cs.offsets[c + 1]; ++p)
                    idx[p] = int64_t(p - cs.offsets[c]);
            if (ctx.domain == Domain::Curve)
                return Column::of(std::vector<int64_t>(cs.numCurves(), 0));
            return Column::of(std::move(idx));
        }
        std::vector<double> at, total;
        curveLengths(cs, at, total);
        if (ctx.domain == Domain::Curve)
        {
            double all = std::accumulate(total.begin(), total.end(), 0.0), before = 0.0;
            std::vector<double> v(cs.numCurves());
            for (size_t c = 0; c < v.size(); ++c)
            {
                v[c] = mWhich == Length ? before : (all > 0.0 ? before / all : 0.0);
                before += total[c];
            }
            return Column::of(std::move(v));
        }
        if (mWhich == Factor)
            for (size_t c = 0; c < cs.numCurves(); ++c)
                for (Index p = cs.offsets[c]; p < cs.offsets[c + 1]; ++p)
                    at[p] = total[c] > 0.0 ? at[p] / total[c] : 0.0;
        return Column::of(std::move(at));
    }

private:
    Which mWhich;
};

class FrameField final : public Field
{
public:
    FrameField(bool normal, int normalMode) : Field(SocketType::Vector), mNormal(normal), mMode(normalMode) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        return onCurvePoints(ctx, mNormal ? D3(0.0, 1.0, 0.0) : D3(0.0, 0.0, 1.0), [&](const CurveSet& cs) {
            std::vector<D3> t, n;
            controlFrames(cs, mMode, t, n);
            return mNormal ? n : t;
        });
    }

private:
    bool mNormal;
    int mMode;
};

class SplineLengthField final : public Field
{
public:
    explicit SplineLengthField(bool count) : Field(count ? SocketType::Int : SocketType::Float), mCount(count) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve)
            return mCount ? Column::of(int64_t(0)) : Column::of(0.0);
        const CurveSet& cs = *ctx.curves;
        Column perCurve;
        if (mCount)
        {
            std::vector<int64_t> n(cs.numCurves());
            for (size_t c = 0; c < n.size(); ++c)
                n[c] = int64_t(cs.curveSize(c));
            perCurve = Column::of(std::move(n));
        }
        else
        {
            std::vector<double> at, total;
            curveLengths(cs, at, total);
            perCurve = Column::of(std::move(total));
        }
        return adaptCurveDomain(cs, perCurve, Domain::Curve, ctx.domain);
    }

private:
    bool mCount;
};

class EndpointField final : public Field
{
public:
    EndpointField(FieldPtr start, FieldPtr end) : Field(SocketType::Bool), mStart(std::move(start)), mEnd(std::move(end)) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve)
            return Column::of(uint8_t(0));
        const CurveSet& cs = *ctx.curves;
        FieldContext perCurve = FieldContext::forCurves(cs, Domain::Curve);
        std::vector<int64_t> s = evaluateAll<int64_t>(mStart, perCurve), e = evaluateAll<int64_t>(mEnd, perCurve);
        std::vector<uint8_t> v(cs.numPoints(), 0);
        for (size_t c = 0; c < cs.numCurves(); ++c)
        {
            int64_t n = int64_t(cs.curveSize(c));
            for (int64_t i = 0; i < n; ++i)
                v[cs.offsets[c] + Index(i)] = (i < s[c] || i >= n - e[c]) ? 1 : 0;
        }
        return adaptCurveDomain(cs, Column::of(std::move(v)), Domain::Point, ctx.domain);
    }

private:
    FieldPtr mStart, mEnd;
};

class CurveOfPointField final : public Field
{
public:
    CurveOfPointField(FieldPtr point, bool indexInCurve) : Field(SocketType::Int), mPoint(std::move(point)), mInCurve(indexInCurve) {}

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve || ctx.curves->numPoints() == 0)
            return Column::of(int64_t(0));
        const CurveSet& cs = *ctx.curves;
        Column pc = gn::evaluate(mPoint, ctx, SocketType::Int);
        size_t n = pc.single ? 1 : ctx.size;
        std::vector<int64_t> out(n);
        for (size_t k = 0; k < n; ++k)
        {
            Index p = Index(std::clamp<int64_t>(pc.at<int64_t>(k), 0, int64_t(cs.numPoints()) - 1));
            size_t c = size_t(std::upper_bound(cs.offsets.begin(), cs.offsets.end(), p) - cs.offsets.begin()) - 1;
            c = std::min(c, cs.numCurves() - 1);
            out[k] = mInCurve ? int64_t(p - cs.offsets[c]) : int64_t(c);
        }
        return {std::move(out), pc.single};
    }

private:
    FieldPtr mPoint;
    bool mInCurve;
};

class PointsOfCurveField final : public Field
{
public:
    PointsOfCurveField(FieldPtr curve, FieldPtr weights, FieldPtr sortIndex, bool total)
        : Field(SocketType::Int), mCurve(std::move(curve)), mWeights(std::move(weights)), mSort(std::move(sortIndex)), mTotal(total)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        if (ctx.component != Component::Curve || ctx.curves->numCurves() == 0)
            return Column::of(int64_t(0));
        const CurveSet& cs = *ctx.curves;
        Column cc = gn::evaluate(mCurve, ctx, SocketType::Int), sc = gn::evaluate(mSort, ctx, SocketType::Int);
        std::vector<double> w = mTotal ? std::vector<double>() : evaluateAll<double>(mWeights, FieldContext::forCurves(cs, Domain::Point));
        bool single = cc.single && sc.single;
        size_t n = single ? 1 : ctx.size;
        std::vector<int64_t> out(n, 0);
        for (size_t k = 0; k < n; ++k)
        {
            int64_t c = cc.at<int64_t>(k);
            if (c < 0 || c >= int64_t(cs.numCurves()))
                continue;
            int64_t size = int64_t(cs.curveSize(size_t(c)));
            if (mTotal || size == 0)
            {
                out[k] = size;
                continue;
            }
            std::vector<Index> order(static_cast<size_t>(size));
            std::iota(order.begin(), order.end(), cs.offsets[size_t(c)]);
            std::stable_sort(order.begin(), order.end(), [&](Index a, Index b) { return w[a] < w[b]; });
            int64_t s = sc.at<int64_t>(k);
            out[k] = int64_t(order[size_t(((s % size) + size) % size)]);
        }
        return {std::move(out), single};
    }

private:
    FieldPtr mCurve, mWeights, mSort;
    bool mTotal;
};

/** An evaluated curve set with what sampling it needs. */
struct SampleTable
{
    CurveSet ev;
    std::vector<std::vector<double>> acc;
    std::vector<double> start; // length of the curves before each
    double total = 0.0;
    std::vector<D3> tangents, normals;
};

std::shared_ptr<const SampleTable> sampleTable(const CurveSet& cs)
{
    auto t = std::make_shared<SampleTable>();
    t->ev = cs.evaluated();
    for (size_t c = 0; c < t->ev.numCurves(); ++c)
    {
        t->acc.push_back(cumulative(t->ev.curvePoints(c), t->ev.cyclic[c] != 0));
        t->start.push_back(t->total);
        t->total += t->acc.back().empty() ? 0.0 : t->acc.back().back();
    }
    polyFrames(t->ev, t->tangents, t->normals);
    return t;
}

template <class V> V lerpValue(const Column& c, const Mix& m)
{
    V a = c.at<V>(m.a), b = c.at<V>(m.b);
    if constexpr (std::is_same_v<V, double> || std::is_same_v<V, D3> || std::is_same_v<V, glm::dvec4>)
        return a + (b - a) * m.t;
    else
        return m.t < 0.5 ? a : b;
}

class SampleCurveField final : public Field
{
public:
    enum Which
    {
        Value,
        Position,
        Tangent,
        Normal,
    };

    SampleCurveField(SocketType type, std::shared_ptr<const SampleTable> table, Column values, FieldPtr param, FieldPtr curve, bool byLength,
                     bool allCurves, Which which)
        : Field(type), mTable(std::move(table)), mValues(std::move(values)), mParam(std::move(param)), mCurve(std::move(curve)),
          mByLength(byLength), mAll(allCurves), mWhich(which)
    {
    }

    Column evaluate(const FieldContext& ctx) const override
    {
        Column pc = gn::evaluate(mParam, ctx, SocketType::Float), cc = gn::evaluate(mCurve, ctx, SocketType::Int);
        bool single = pc.single && cc.single;
        size_t n = single ? 1 : ctx.size;
        std::vector<Mix> mixes(n);
        std::vector<uint8_t> ok(n, 0);
        for (size_t k = 0; k < n; ++k)
            ok[k] = locate(pc.at<double>(k), cc.at<int64_t>(k), mixes[k]) ? 1 : 0;
        const SampleTable& tb = *mTable;
        if (mWhich != Value)
        {
            std::vector<D3> out(n, D3(0.0));
            for (size_t k = 0; k < n; ++k)
            {
                if (!ok[k])
                    continue;
                const Mix& m = mixes[k];
                if (mWhich == Position)
                    out[k] = glm::mix(tb.ev.positions[m.a], tb.ev.positions[m.b], m.t);
                else
                    out[k] = mixUnit(mWhich == Tangent ? tb.tangents : tb.normals, m);
            }
            return {std::move(out), single};
        }
        return std::visit(
            [&](const auto& v) -> Column {
                using V = typename std::decay_t<decltype(v)>::value_type;
                std::vector<V> out(n, V(0));
                for (size_t k = 0; k < n; ++k)
                    if (ok[k])
                        out[k] = lerpValue<V>(mValues, mixes[k]);
                return {std::move(out), single};
            },
            mValues.data);
    }

private:
    bool locate(double param, int64_t curve, Mix& m) const
    {
        const SampleTable& tb = *mTable;
        size_t nc = tb.ev.numCurves();
        if (nc == 0)
            return false;
        size_t c = 0;
        double s = 0.0;
        if (mAll)
        {
            s = std::clamp(mByLength ? param : param * tb.total, 0.0, tb.total);
            c = size_t(std::upper_bound(tb.start.begin(), tb.start.end(), s) - tb.start.begin());
            c = std::clamp<size_t>(c, 1, nc) - 1;
            s -= tb.start[c];
        }
        else
        {
            c = size_t(std::clamp<int64_t>(curve, 0, int64_t(nc) - 1));
            double len = tb.acc[c].empty() ? 0.0 : tb.acc[c].back();
            s = mByLength ? param : param * len;
        }
        size_t np = tb.ev.curveSize(c);
        if (np == 0)
            return false;
        m = mixAt(tb.acc[c], np, tb.ev.offsets[c], s);
        return true;
    }

    std::shared_ptr<const SampleTable> mTable;
    Column mValues;
    FieldPtr mParam, mCurve;
    bool mByLength, mAll;
    Which mWhich;
};

std::vector<uint8_t> curveSelection(const FieldPtr& sel, const CurveSet& cs)
{
    return evaluateAll<uint8_t>(sel, FieldContext::forCurves(cs, Domain::Curve));
}

void trimCurve()
{
    registerDecl({
        .type = "TrimCurve",
        .label = "Trim Curve",
        .category = "Curve",
        .doc = "Keeps the part of each curve between start and end, as factors of its length or as lengths from its start. Closed "
               "curves open; Bezier and NURBS curves come out as poly.",
        .props = {prop("mode", {"factor", "length"}, "factor")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                bool byLength = findDecl("TrimCurve")->propValue(n, "mode") == "length";
                Sockets s;
                s.inputs = {geoIn("curve"), fin("selection", T::Bool, true, Implicit::None, "Curves to trim.")};
                if (byLength)
                    s.inputs.insert(s.inputs.end(), {bounded(fin("start", T::Float, 0.0, Implicit::None, "cm from the start."), 0.0),
                                                     bounded(fin("end", T::Float, 100.0, Implicit::None, "cm from the start."), 0.0)});
                else
                    s.inputs.insert(s.inputs.end(), {bounded(fin("start", T::Float, 0.0), 0.0, 1.0), bounded(fin("end", T::Float, 1.0), 0.0, 1.0)});
                s.outputs = {geoOut("curve")};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                bool byLength = c.prop("mode") == "length";
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        CurveSet ev = cs.evaluated();
                                                                        FieldContext perCurve = FieldContext::forCurves(ev, Domain::Curve);
                                                                        std::vector<uint8_t> sel = curveSelection(c.field("selection"), ev);
                                                                        std::vector<double> s0 = evaluateAll<double>(c.field("start"), perCurve);
                                                                        std::vector<double> s1 = evaluateAll<double>(c.field("end"), perCurve);
                                                                        CurveSet out = layoutOf(ev);
                                                                        for (size_t cu = 0; cu < ev.numCurves(); ++cu)
                                                                        {
                                                                            size_t n = ev.curveSize(cu);
                                                                            bool cyc = ev.cyclic[cu] != 0;
                                                                            if (!sel[cu] || n < 2)
                                                                            {
                                                                                addMixed(out, ev, cu, wholeCurve(ev, cu), cyc);
                                                                                continue;
                                                                            }
                                                                            std::vector<double> acc = cumulative(ev.curvePoints(cu), cyc);
                                                                            double len = acc.back();
                                                                            double a = std::clamp(byLength ? s0[cu] : s0[cu] * len, 0.0, len);
                                                                            double b = std::clamp(byLength ? s1[cu] : s1[cu] * len, a, len);
                                                                            Index p0 = ev.offsets[cu];
                                                                            std::vector<Mix> mixes{mixAt(acc, n, p0, a)};
                                                                            if (b - a > 1e-9)
                                                                            {
                                                                                for (size_t k = 0; k < acc.size(); ++k)
                                                                                    if (acc[k] > a + 1e-9 && acc[k] < b - 1e-9)
                                                                                        mixes.push_back({p0 + Index(k % n), p0 + Index(k % n), 0.0});
                                                                                mixes.push_back(mixAt(acc, n, p0, b));
                                                                            }
                                                                            addMixed(out, ev, cu, mixes, false);
                                                                        }
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });
}

void reverseCurve()
{
    registerDecl({
        .type = "ReverseCurve",
        .label = "Reverse Curve",
        .category = "Curve",
        .doc = "Runs the selected curves the other way, keeping their shape and point attributes.",
        .sockets = {.inputs = {geoIn("curve"), fin("selection", T::Bool, true, Implicit::None, "Curves to reverse.")}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        std::vector<uint8_t> sel = curveSelection(c.field("selection"), cs);
                                                                        std::vector<std::pair<size_t, curve::Spline>> nurbs;
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                            if (sel[cu] && cu < cs.knots.size() && !cs.knots[cu].empty())
                                                                            {
                                                                                curve::Spline sp = cs.spline(cu);
                                                                                curve::reverse(sp);
                                                                                nurbs.emplace_back(cu, std::move(sp));
                                                                            }
                                                                        std::vector<Index> perm(cs.numPoints());
                                                                        std::iota(perm.begin(), perm.end(), Index(0));
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                            if (sel[cu])
                                                                                for (Index p = cs.offsets[cu]; p < cs.offsets[cu + 1]; ++p)
                                                                                    perm[p] = cs.offsets[cu] + cs.offsets[cu + 1] - 1 - p;
                                                                        std::vector<D3> pos(cs.numPoints());
                                                                        for (size_t p = 0; p < pos.size(); ++p)
                                                                            pos[p] = cs.positions[perm[p]];
                                                                        cs.positions = std::move(pos);
                                                                        cs.pointAttrs.reorder(perm);
                                                                        std::span<glm::vec3> hl = cs.pointAttrs.get<glm::vec3>("handle_left");
                                                                        std::span<glm::vec3> hr = cs.pointAttrs.get<glm::vec3>("handle_right");
                                                                        std::span<int32_t> tl = cs.pointAttrs.get<int32_t>("handle_type_left");
                                                                        std::span<int32_t> tr = cs.pointAttrs.get<int32_t>("handle_type_right");
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                            if (sel[cu])
                                                                                for (Index p = cs.offsets[cu]; p < cs.offsets[cu + 1]; ++p)
                                                                                {
                                                                                    if (!hl.empty() && !hr.empty())
                                                                                        std::swap(hl[p], hr[p]);
                                                                                    if (!tl.empty() && !tr.empty())
                                                                                        std::swap(tl[p], tr[p]);
                                                                                }
                                                                        for (auto& [cu, sp] : nurbs)
                                                                        {
                                                                            cs.knots[cu] = sp.knots;
                                                                            std::copy(sp.points.begin(), sp.points.end(), cs.positions.begin() + cs.offsets[cu]);
                                                                        }
                                                                    }}));
            },
    });
}

/** Each span cut into cuts[span] + 1 pieces, keeping the exact shape; tilt and radius follow along. */
curve::Spline subdivideSpline(const curve::Spline& sp, std::span<const int64_t> cuts)
{
    std::vector<curve::Piece> ps = curve::pieces(sp), out;
    std::vector<double> params;
    for (size_t i = 0; i < ps.size(); ++i)
    {
        int n = int(std::clamp<int64_t>(cuts.empty() ? 0 : cuts[std::min(i, cuts.size() - 1)], 0, 1000));
        for (int k = 0; k <= n; ++k)
        {
            out.push_back(curve::subPiece(ps[i], double(k) / (n + 1), double(k + 1) / (n + 1)));
            params.push_back(double(ps[i].span) + double(k) / (n + 1));
        }
    }
    if (!sp.closed)
        params.push_back(double(curve::spanCount(sp)));
    curve::Spline r = curve::fromPieces(out, sp.closed, sp.kind);
    if (r.size() == params.size())
    {
        if (!sp.tilt.empty())
            for (double u : params)
                r.tilt.push_back(curve::pointValueAt(sp, sp.tilt, 0.0, u));
        if (!sp.radius.empty())
            for (double u : params)
                r.radius.push_back(curve::pointValueAt(sp, sp.radius, 1.0, u));
    }
    r.normalize();
    return r;
}

void subdivideCurve()
{
    registerDecl({
        .type = "SubdivideCurve",
        .label = "Subdivide Curve",
        .category = "Curve",
        .doc = "Adds points to every segment without changing the shape. Cuts is read per point, for the segment that starts there.",
        .sockets = {.inputs = {geoIn("curve"), bounded(fin("cuts", T::Int, 1), 0, 1000)}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        std::vector<int64_t> cuts =
                                                                            evaluateAll<int64_t>(c.field("cuts"), FieldContext::forCurves(cs, Domain::Point));
                                                                        CurveSet out = layoutOf(cs);
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                        {
                                                                            size_t n = cs.curveSize(cu);
                                                                            Index p0 = cs.offsets[cu];
                                                                            bool cyc = cs.cyclic[cu] != 0;
                                                                            std::span<const int64_t> mine(cuts.data() + p0, n);
                                                                            if (!polyCurve(cs, cu))
                                                                            {
                                                                                curve::Spline sp = cs.spline(cu);
                                                                                if (curve::isValid(sp))
                                                                                {
                                                                                    curve::Spline r = subdivideSpline(sp, mine);
                                                                                    out.addSpline(r);
                                                                                    size_t oc = out.numCurves() - 1;
                                                                                    out.curveAttrs.copyFrom(cs.curveAttrs, oc, cu);
                                                                                    if (auto t = out.curveAttrs.get<int32_t>("curve_type"); !t.empty())
                                                                                        t[oc] = int32_t(r.kind);
                                                                                    if (auto d = out.curveAttrs.get<int32_t>("degree"); !d.empty())
                                                                                        d[oc] = r.degree;
                                                                                    continue;
                                                                                }
                                                                            }
                                                                            std::vector<Mix> mixes;
                                                                            size_t segs = n < 2 ? 0 : (cyc ? n : n - 1);
                                                                            for (size_t i = 0; i < n; ++i)
                                                                            {
                                                                                Index a = p0 + Index(i);
                                                                                mixes.push_back({a, a, 0.0});
                                                                                if (i >= segs)
                                                                                    continue;
                                                                                Index b = p0 + Index((i + 1) % n);
                                                                                int k = int(std::clamp<int64_t>(mine[i], 0, 1000));
                                                                                for (int j = 1; j <= k; ++j)
                                                                                    mixes.push_back({a, b, double(j) / (k + 1)});
                                                                            }
                                                                            addMixed(out, cs, cu, mixes, cyc);
                                                                        }
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });
}

void sampleCurve()
{
    registerDecl({
        .type = "SampleCurve",
        .label = "Sample Curve",
        .category = "Curve",
        .doc = "Reads position, tangent, normal and a value field at a point along a curve, by factor of its length or by length. "
               "With All Curves the curves count as one, end to end.",
        .props = {prop("dataType", dataTypes(), "float"), prop("mode", {"factor", "length"}, "factor")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                const NodeDecl* d = findDecl("SampleCurve");
                SocketType t = dataTypeOf(d->propValue(n, "dataType"));
                bool byLength = d->propValue(n, "mode") == "length";
                Sockets s;
                s.inputs = {geoIn("curves"), fin("value", t, zeroJson(t), Implicit::None, "Evaluated on the curve points."),
                            byLength ? bounded(fin("length", T::Float, 0.0, Implicit::None, "cm."), 0.0) : bounded(fin("factor", T::Float, 0.0), 0.0, 1.0),
                            fin("curveIndex", T::Int, 0), in("allCurves", T::Bool, false)};
                s.outputs = {fout("value", t), fout("position", T::Vector), fout("tangent", T::Vector), fout("normal", T::Vector)};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                SocketType t = dataTypeOf(c.prop("dataType"));
                bool byLength = c.prop("mode") == "length";
                GeometryPtr g = c.geometry("curves");
                auto table = sampleTable(g->curves ? *g->curves : CurveSet());
                Column values = gn::evaluate(c.field("value"), FieldContext::forCurves(table->ev, Domain::Point), t);
                FieldPtr param = c.field(byLength ? "length" : "factor"), curveIndex = c.field("curveIndex");
                bool all = c.b("allCurves");
                auto make = [&](SocketType type, SampleCurveField::Which w) {
                    return std::make_shared<SampleCurveField>(type, table, values, param, curveIndex, byLength, all, w);
                };
                c.out("value", make(t, SampleCurveField::Value));
                c.out("position", make(T::Vector, SampleCurveField::Position));
                c.out("tangent", make(T::Vector, SampleCurveField::Tangent));
                c.out("normal", make(T::Vector, SampleCurveField::Normal));
            },
    });
}

void readNodes()
{
    registerDecl({
        .type = "CurveParameter",
        .label = "Curve Parameter",
        .category = "Curve",
        .doc = "Per curve point: how far along its curve it is as a factor and as a length, and its index on the curve. On the curve "
               "domain, the length of the curves before it.",
        .sockets = {.outputs = {fout("factor", T::Float), fout("length", T::Float), fout("index", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("factor", std::make_shared<ParameterField>(ParameterField::Factor));
                c.out("length", std::make_shared<ParameterField>(ParameterField::Length));
                c.out("index", std::make_shared<ParameterField>(ParameterField::IndexOnCurve));
            },
    });
    registerDecl({
        .type = "CurveTangent",
        .label = "Curve Tangent",
        .category = "Curve",
        .doc = "Direction of the curve at each point.",
        .sockets = {.outputs = {fout("tangent", T::Vector)}},
        .exec = [](ExecContext& c) { c.out("tangent", std::make_shared<FrameField>(false, 0)); },
    });
    registerDecl({
        .type = "CurveNormal",
        .label = "Curve Normal",
        .category = "Curve",
        .doc = "Normal of the curve at each point, tilt included. minimumTwist turns as little as it can along the curve; yUp stays "
               "square to the up axis.",
        .props = {prop("mode", {"minimumTwist", "yUp"}, "minimumTwist")},
        .sockets = {.outputs = {fout("normal", T::Vector)}},
        .exec = [](ExecContext& c) { c.out("normal", std::make_shared<FrameField>(true, c.prop("mode") == "yUp" ? 1 : 0)); },
    });
    registerDecl({
        .type = "SplineLength",
        .label = "Spline Length",
        .category = "Curve",
        .doc = "Length and point count of the curve each element belongs to.",
        .sockets = {.outputs = {fout("length", T::Float), fout("pointCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("length", std::make_shared<SplineLengthField>(false));
                c.out("pointCount", std::make_shared<SplineLengthField>(true));
            },
    });
    registerDecl({
        .type = "EndpointSelection",
        .label = "Endpoint Selection",
        .category = "Curve",
        .doc = "True for the first start and last end points of each curve.",
        .sockets = {.inputs = {bounded(fin("startSize", T::Int, 1), 0), bounded(fin("endSize", T::Int, 1), 0)}, .outputs = {fout("selection", T::Bool)}},
        .exec = [](ExecContext& c) { c.out("selection", std::make_shared<EndpointField>(c.field("startSize"), c.field("endSize"))); },
    });
    registerDecl({
        .type = "CurveOfPoint",
        .label = "Curve of Point",
        .category = "Curve",
        .doc = "The curve a point belongs to, and its index on that curve.",
        .sockets = {.inputs = {fin("pointIndex", T::Int, nullptr, Implicit::Index)}, .outputs = {fout("curveIndex", T::Int), fout("indexInCurve", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("curveIndex", std::make_shared<CurveOfPointField>(c.field("pointIndex"), false));
                c.out("indexInCurve", std::make_shared<CurveOfPointField>(c.field("pointIndex"), true));
            },
    });
    registerDecl({
        .type = "PointsOfCurve",
        .label = "Points of Curve",
        .category = "Curve",
        .doc = "A point of a curve, picked by its place when the curve's points are sorted by weight (sort index wraps), and how "
               "many points the curve has.",
        .sockets = {.inputs = {fin("curveIndex", T::Int, nullptr, Implicit::Index), fin("weights", T::Float, 0.0, Implicit::None, "Evaluated on the points."),
                               fin("sortIndex", T::Int, 0)},
                    .outputs = {fout("pointIndex", T::Int), fout("total", T::Int)}},
        .exec =
            [](ExecContext& c) {
                c.out("pointIndex", std::make_shared<PointsOfCurveField>(c.field("curveIndex"), c.field("weights"), c.field("sortIndex"), false));
                c.out("total", std::make_shared<PointsOfCurveField>(c.field("curveIndex"), c.field("weights"), c.field("sortIndex"), true));
            },
    });
}

void setTilt()
{
    registerDecl({
        .type = "SetCurveTilt",
        .label = "Set Curve Tilt",
        .category = "Curve",
        .doc = "Sets the tilt of curve points (radians around the tangent), which turns the normal and swept profiles.",
        .sockets = {.inputs = {geoIn("curve"), selectionIn(), fin("tilt", T::Float, 0.0)}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        FieldContext ctx = FieldContext::forCurves(cs, Domain::Point);
                                                                        std::vector<uint8_t> sel = evaluateAll<uint8_t>(c.field("selection"), ctx);
                                                                        std::vector<double> tilt = evaluateAll<double>(c.field("tilt"), ctx);
                                                                        std::vector<double> v = evaluateAll<double>(attributeField("tilt", T::Float), ctx);
                                                                        for (size_t k = 0; k < v.size(); ++k)
                                                                            if (sel[k])
                                                                                v[k] = tilt[k];
                                                                        writeAttribute(cs, "tilt", Domain::Point, Column::of(std::move(v)), T::Float);
                                                                    }}));
            },
    });
}

/** Positions of the points a guide-following node grows from: point clouds and mesh vertices. */
std::vector<D3> rootPoints(const Geometry& g)
{
    std::vector<D3> out;
    if (g.points)
        out.insert(out.end(), g.points->positions().begin(), g.points->positions().end());
    if (g.mesh)
        out.insert(out.end(), g.mesh->positions().begin(), g.mesh->positions().end());
    return out;
}

void interpolateCurves()
{
    registerDecl({
        .type = "InterpolateCurves",
        .label = "Interpolate Curves",
        .category = "Curve",
        .doc = "Grows a curve from every point by blending the shapes of the nearest guide curves (by their first points), closer "
               "guides weighing more. For hair and grass from a few guides.",
        .sockets = {.inputs = {geoIn("guideCurves"), geoIn("points"), bounded(in("maxNeighbors", T::Int, 4), 1, 64)},
                    .outputs = {geoOut("curves"), fout("closestIndex", T::Int, "The nearest guide's index, per curve.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr guides = c.geometry("guideCurves");
                std::vector<D3> roots = rootPoints(*c.geometry("points"));
                CurveSet ev = guides->curves ? guides->curves->evaluated() : CurveSet();
                std::vector<size_t> usable;
                std::vector<std::vector<double>> acc(ev.numCurves());
                for (size_t g = 0; g < ev.numCurves(); ++g)
                    if (ev.curveSize(g) > 0)
                    {
                        usable.push_back(g);
                        acc[g] = cumulative(ev.curvePoints(g), false);
                    }
                std::string closestName = c.anonymous("closestIndex");
                if (usable.empty() || roots.empty())
                {
                    c.out("curves", makeGeometry({}));
                    c.out("closestIndex", attributeField(closestName, T::Int));
                    return;
                }
                size_t k = std::min<size_t>(usable.size(), size_t(std::clamp<int64_t>(c.i("maxNeighbors"), 1, 64)));
                auto sample = [&](size_t g, double f) {
                    size_t n = ev.curveSize(g);
                    Mix m = mixAt(acc[g], n, ev.offsets[g], f * (acc[g].empty() ? 0.0 : acc[g].back()));
                    return glm::mix(ev.positions[m.a], ev.positions[m.b], m.t);
                };
                std::vector<std::vector<D3>> grown(roots.size());
                std::vector<int64_t> closest(roots.size(), 0);
                tbb::parallel_for(size_t(0), roots.size(), [&](size_t r) {
                    std::vector<std::pair<double, size_t>> near(usable.size());
                    for (size_t i = 0; i < usable.size(); ++i)
                        near[i] = {glm::distance(roots[r], ev.positions[ev.offsets[usable[i]]]), usable[i]};
                    std::partial_sort(near.begin(), near.begin() + ptrdiff_t(k), near.end());
                    std::vector<double> w(k);
                    if (near[0].first < 1e-9)
                        w[0] = 1.0;
                    else
                    {
                        double sum = 0.0;
                        for (size_t i = 0; i < k; ++i)
                            sum += w[i] = 1.0 / near[i].first;
                        for (double& x : w)
                            x /= sum;
                    }
                    size_t count = 2;
                    for (size_t i = 0; i < k; ++i)
                        count = std::max(count, ev.curveSize(near[i].second));
                    std::vector<D3>& pts = grown[r];
                    pts.assign(count, roots[r]);
                    for (size_t j = 0; j < count; ++j)
                    {
                        double f = double(j) / double(count - 1);
                        for (size_t i = 0; i < k; ++i)
                            if (w[i] > 0.0)
                                pts[j] += w[i] * (sample(near[i].second, f) - ev.positions[ev.offsets[near[i].second]]);
                    }
                    closest[r] = int64_t(near[0].second);
                });
                CurveSet out;
                for (const std::vector<D3>& pts : grown)
                    out.addCurve(pts, false);
                writeAttribute(out, closestName, Domain::Curve, Column::of(std::move(closest)), T::Int);
                c.out("curves", curveGeometry(std::move(out)));
                c.out("closestIndex", attributeField(closestName, T::Int));
            },
    });
}

void curveToPoints()
{
    registerDecl({
        .type = "CurveToPoints",
        .label = "Curve to Points",
        .category = "Curve",
        .doc = "Points along curves: evenly spaced by count or by length, or at every evaluated point. Point and curve attributes carry "
               "over. Rotation turns X along the tangent and Y onto the normal, for instancing.",
        .props = {prop("mode", {"count", "length", "evaluated"}, "count")},
        .sockets = {.inputs = {geoIn("curve"), bounded(in("count", T::Int, 10), 1, 1000000),
                               bounded(in("length", T::Float, 10.0, "Spacing, cm (length mode)."), 1e-6)},
                    .outputs = {geoOut("points"), fout("tangent", T::Vector), fout("normal", T::Vector), fout("rotation", T::Rotation)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                GeometryPtr g = c.geometry("curve");
                std::string mode = c.prop("mode");
                int count = int(std::clamp<int64_t>(c.i("count"), 1, 1000000));
                double spacing = std::max(1e-6, c.f("length"));
                std::string tanName = c.anonymous("tangent"), nrmName = c.anonymous("normal"), rotName = c.anonymous("rotation");
                c.out("tangent", attributeField(tanName, T::Vector));
                c.out("normal", attributeField(nrmName, T::Vector));
                c.out("rotation", attributeField(rotName, T::Rotation));
                if (!g->curves)
                {
                    c.out("points", makeGeometry({}));
                    return;
                }
                CurveSet ev = g->curves->evaluated();
                std::vector<D3> tangents, normals;
                polyFrames(ev, tangents, normals);
                geo::Mesh m;
                std::vector<std::pair<Mix, size_t>> placed; // mix and its curve
                for (size_t cu = 0; cu < ev.numCurves(); ++cu)
                {
                    size_t n = ev.curveSize(cu);
                    if (n == 0)
                        continue;
                    bool cyc = ev.cyclic[cu] != 0;
                    if (mode == "evaluated")
                    {
                        for (const Mix& mx : wholeCurve(ev, cu))
                            placed.push_back({mx, cu});
                        continue;
                    }
                    std::vector<double> acc = cumulative(ev.curvePoints(cu), cyc);
                    double len = acc.back();
                    int k = count;
                    if (mode == "length")
                        k = std::max(cyc ? 3 : 2, int(std::round(len / spacing)) + (cyc ? 0 : 1));
                    for (int i = 0; i < k; ++i)
                    {
                        double s = cyc ? len * i / k : (k == 1 ? 0.0 : len * i / (k - 1));
                        placed.push_back({mixAt(acc, n, ev.offsets[cu], s), cu});
                    }
                }
                for (const auto& [mx, cu] : placed)
                    m.addPoint(glm::mix(ev.positions[mx.a], ev.positions[mx.b], mx.t));
                m.pointAttrs().mergeLayout(ev.curveAttrs);
                m.pointAttrs().mergeLayout(ev.pointAttrs);
                std::vector<D3> tan(placed.size()), nrm(placed.size()), rot(placed.size());
                for (size_t k = 0; k < placed.size(); ++k)
                {
                    const auto& [mx, cu] = placed[k];
                    m.pointAttrs().copyFrom(ev.curveAttrs, k, cu);
                    Index srcs[2] = {mx.a, mx.b};
                    float w[2] = {float(1.0 - mx.t), float(mx.t)};
                    m.pointAttrs().mixFrom(ev.pointAttrs, k, srcs, w);
                    tan[k] = mixUnit(tangents, mx);
                    D3 n = mixUnit(normals, mx);
                    n = glm::length(n - tan[k] * glm::dot(n, tan[k])) > 1e-12 ? glm::normalize(n - tan[k] * glm::dot(n, tan[k])) : n;
                    nrm[k] = n;
                    rot[k] = matrixToEuler(glm::dmat3(tan[k], n, glm::cross(tan[k], n)));
                }
                writeAttribute(m, tanName, Domain::Point, Column::of(std::move(tan)), T::Vector);
                writeAttribute(m, nrmName, Domain::Point, Column::of(std::move(nrm)), T::Vector);
                writeAttribute(m, rotName, Domain::Point, Column::of(std::move(rot)), T::Rotation);
                c.out("points", pointGeometry(std::move(m)));
            },
    });
}

/** Blender's XY primitive plane laid on Lila's ground plane, matching Curve Circle. */
D3 ground(double x, double y) { return {x, 0.0, -y}; }

void quadrilateral()
{
    registerDecl({
        .type = "CurveQuadrilateral",
        .label = "Quadrilateral",
        .category = "Curve Primitives",
        .doc = "A closed four-point curve in the ground plane: rectangle, parallelogram, trapezoid, kite, or four given points.",
        .props = {prop("mode", {"rectangle", "parallelogram", "trapezoid", "kite", "points"}, "rectangle")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                std::string mode = findDecl("CurveQuadrilateral")->propValue(n, "mode");
                Sockets s;
                s.outputs = {geoOut("curve")};
                if (mode == "rectangle")
                    s.inputs = {bounded(in("width", T::Float, 100.0), 0.0), bounded(in("height", T::Float, 100.0), 0.0)};
                else if (mode == "parallelogram")
                    s.inputs = {bounded(in("width", T::Float, 100.0), 0.0), bounded(in("height", T::Float, 100.0), 0.0), in("offset", T::Float, 50.0)};
                else if (mode == "trapezoid")
                    s.inputs = {bounded(in("bottomWidth", T::Float, 200.0), 0.0), bounded(in("topWidth", T::Float, 100.0), 0.0), in("offset", T::Float, 0.0),
                                bounded(in("height", T::Float, 100.0), 0.0)};
                else if (mode == "kite")
                    s.inputs = {bounded(in("width", T::Float, 100.0), 0.0), in("bottomHeight", T::Float, 150.0), in("topHeight", T::Float, 50.0)};
                else
                    s.inputs = {in("point1", T::Vector, vec(-50, 0, 50)), in("point2", T::Vector, vec(50, 0, 50)), in("point3", T::Vector, vec(50, 0, -50)),
                                in("point4", T::Vector, vec(-50, 0, -50))};
                return s;
            },
        .exec =
            [](ExecContext& c) {
                std::string mode = c.prop("mode");
                std::array<D3, 4> p;
                if (mode == "rectangle")
                {
                    double w = c.f("width") / 2, h = c.f("height") / 2;
                    p = {ground(w, h), ground(-w, h), ground(-w, -h), ground(w, -h)};
                }
                else if (mode == "parallelogram")
                {
                    double w = c.f("width") / 2, h = c.f("height") / 2, o = c.f("offset") / 2;
                    p = {ground(w + o, h), ground(-w + o, h), ground(-w - o, -h), ground(w - o, -h)};
                }
                else if (mode == "trapezoid")
                {
                    double b = c.f("bottomWidth") / 2, t = c.f("topWidth") / 2, o = c.f("offset"), h = c.f("height") / 2;
                    p = {ground(t + o, h), ground(-t + o, h), ground(-b, -h), ground(b, -h)};
                }
                else if (mode == "kite")
                {
                    double w = c.f("width") / 2;
                    p = {ground(0, c.f("topHeight")), ground(-w, 0), ground(0, -c.f("bottomHeight")), ground(w, 0)};
                }
                else
                    p = {c.v("point1"), c.v("point2"), c.v("point3"), c.v("point4")};
                CurveSet cs;
                cs.addCurve(p, true);
                c.out("curve", curveGeometry(std::move(cs)));
            },
    });
}

void star()
{
    registerDecl({
        .type = "CurveStar",
        .label = "Star",
        .category = "Curve Primitives",
        .doc = "A closed star in the ground plane, alternating outer and inner points. Outer Points selects the tips.",
        .sockets = {.inputs = {bounded(in("points", T::Int, 8), 3, 100000), bounded(in("innerRadius", T::Float, 25.0), 0.0),
                               bounded(in("outerRadius", T::Float, 50.0), 0.0), in("twist", T::Float, 0.0, "Turn of the inner points, radians.")},
                    .outputs = {geoOut("curve"), fout("outerPoints", T::Bool)}},
        .exec =
            [](ExecContext& c) {
                int n = int(std::clamp<int64_t>(c.i("points"), 3, 100000));
                double ri = c.f("innerRadius"), ro = c.f("outerRadius"), tw = c.f("twist");
                std::vector<D3> pts;
                std::vector<uint8_t> outer;
                for (int k = 0; k < n; ++k)
                {
                    double a = 2.0 * kPi * k / n, b = 2.0 * kPi * (k + 0.5) / n + tw;
                    pts.push_back(ground(std::cos(a) * ro, std::sin(a) * ro));
                    pts.push_back(ground(std::cos(b) * ri, std::sin(b) * ri));
                    outer.push_back(1);
                    outer.push_back(0);
                }
                CurveSet cs;
                cs.addCurve(pts, true);
                std::string name = c.anonymous("outerPoints");
                writeAttribute(cs, name, Domain::Point, Column::of(std::move(outer)), T::Bool);
                c.out("curve", curveGeometry(std::move(cs)));
                c.out("outerPoints", attributeField(name, T::Bool));
            },
    });
}

void arc()
{
    registerDecl({
        .type = "CurveArc",
        .label = "Arc",
        .category = "Curve Primitives",
        .doc = "An arc: by radius and angles in the ground plane, or through three points. Resolution is the point count. Connect "
               "Center closes it through the centre, Invert Arc takes the other way round.",
        .props = {prop("mode", {"radius", "points"}, "radius")},
        .dynamicSockets =
            [](const Group&, const GNode& n) {
                bool points = findDecl("CurveArc")->propValue(n, "mode") == "points";
                Sockets s;
                s.inputs = {bounded(in("resolution", T::Int, 16), 2, 100000)};
                if (points)
                    s.inputs.insert(s.inputs.end(),
                                    {in("start", T::Vector, vec(-50, 0, 0)), in("middle", T::Vector, vec(0, 0, -100)), in("end", T::Vector, vec(50, 0, 0))});
                else
                    s.inputs.insert(s.inputs.end(), {bounded(in("radius", T::Float, 50.0), 0.0), in("startAngle", T::Float, 0.0, "Radians."),
                                                     in("sweepAngle", T::Float, 1.75 * kPi, "Radians.")});
                s.inputs.insert(s.inputs.end(), {in("connectCenter", T::Bool, false), in("invertArc", T::Bool, false)});
                s.outputs = {geoOut("curve")};
                if (points)
                    s.outputs.insert(s.outputs.end(), {out("center", T::Vector), out("normal", T::Vector), out("radius", T::Float)});
                return s;
            },
        .exec =
            [](ExecContext& c) {
                int res = int(std::clamp<int64_t>(c.i("resolution"), 2, 100000));
                bool invert = c.b("invertArc");
                D3 center(0.0), u(1.0, 0.0, 0.0), v(0.0, 0.0, -1.0), normal(0.0, 1.0, 0.0);
                double r = 0.0, a0 = 0.0, sweep = 0.0;
                std::vector<D3> pts;
                if (c.prop("mode") == "points")
                {
                    D3 a = c.v("start"), b = c.v("middle"), e = c.v("end");
                    D3 ab = b - a, ae = e - a, n = glm::cross(ab, ae);
                    double nn = glm::dot(n, n);
                    if (nn < 1e-18)
                    {
                        for (int k = 0; k < res; ++k)
                            pts.push_back(glm::mix(a, e, double(k) / (res - 1)));
                        center = (a + e) * 0.5;
                    }
                    else
                    {
                        center = a + (glm::cross(n, ab) * glm::dot(ae, ae) + glm::cross(ae, n) * glm::dot(ab, ab)) / (2.0 * nn);
                        normal = glm::normalize(n);
                        r = glm::distance(center, a);
                        u = glm::normalize(a - center);
                        v = glm::cross(normal, u);
                        D3 de = e - center;
                        sweep = std::atan2(glm::dot(de, v), glm::dot(de, u));
                        if (sweep <= 0.0)
                            sweep += 2.0 * kPi;
                        if (invert)
                            sweep -= 2.0 * kPi;
                    }
                    c.out("center", constantVector(center));
                    c.out("normal", constantVector(normal));
                    c.out("radius", constantFloat(r));
                }
                else
                {
                    r = c.f("radius");
                    a0 = c.f("startAngle");
                    sweep = c.f("sweepAngle");
                    if (invert)
                    {
                        a0 += sweep;
                        sweep = 2.0 * kPi - sweep;
                    }
                }
                if (pts.empty())
                    for (int k = 0; k < res; ++k)
                    {
                        double t = a0 + sweep * k / (res - 1);
                        pts.push_back(center + r * (u * std::cos(t) + v * std::sin(t)));
                    }
                bool closed = c.b("connectCenter");
                if (closed)
                    pts.push_back(center);
                CurveSet cs;
                cs.addCurve(pts, closed);
                c.out("curve", curveGeometry(std::move(cs)));
            },
    });
}

void stringToCurves()
{
    registerDecl({
        .type = "StringToCurves",
        .label = "String to Curves",
        .category = "Curve",
        .doc = "Text as closed Bezier outlines in the XY plane, the first baseline on y = 0. Fill Curve makes faces of it. Character is "
               "each curve's character index.",
        .props = {textProp("font", "font", "", "A font file, or a family and style such as \"Inter Bold\"; empty for the default."),
                  prop("align", {"left", "center", "right", "justify"}, "left"), prop("anchor", {"baseline", "top", "middle", "bottom"}, "baseline")},
        .sockets = {.inputs = {edited(in("string", T::String, "Text"), "multiline"), bounded(in("size", T::Float, 10.0, "Em size, cm."), 0.0), in("letterSpacing", T::Float, 0.0, "cm."),
                               in("wordSpacing", T::Float, 0.0, "cm."), bounded(in("lineSpacing", T::Float, 1.0), 0.0),
                               bounded(in("wrapWidth", T::Float, 0.0, "cm; 0 breaks only at new lines."), 0.0)},
                    .outputs = {geoOut("curves"), fout("character", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                curve::TextOptions o;
                o.text = c.s("string");
                o.font = c.prop("font");
                o.size = std::max(0.0, c.f("size"));
                o.letterSpacing = c.f("letterSpacing");
                o.wordSpacing = c.f("wordSpacing");
                o.lineSpacing = c.f("lineSpacing");
                o.wrapWidth = std::max(0.0, c.f("wrapWidth"));
                static const std::vector<std::string> aligns = {"left", "center", "right", "justify"}, anchors = {"baseline", "top", "middle", "bottom"};
                o.align = curve::TextAlign(std::max<ptrdiff_t>(0, std::ranges::find(aligns, c.prop("align")) - aligns.begin()) % 4);
                o.anchor = curve::TextAnchor(std::max<ptrdiff_t>(0, std::ranges::find(anchors, c.prop("anchor")) - anchors.begin()) % 4);
                std::string name = c.anonymous("character");
                c.out("character", attributeField(name, T::Int));
                if (o.text.empty() || o.size <= 0.0)
                {
                    c.out("curves", makeGeometry({}));
                    return;
                }
                curve::TextResult r = curve::textCurves(o);
                CurveSet cs;
                std::vector<int64_t> chars;
                for (size_t k = 0; k < r.curves.size(); ++k)
                {
                    if (!curve::isValid(r.curves[k]))
                        continue;
                    cs.addSpline(r.curves[k]);
                    chars.push_back(k < r.character.size() ? r.character[k] : 0);
                }
                writeAttribute(cs, name, Domain::Curve, Column::of(std::move(chars)), T::Int);
                c.out("curves", curveGeometry(std::move(cs)));
            },
    });
}

void offsetCurve()
{
    registerDecl({
        .type = "OffsetCurve",
        .label = "Offset Curve",
        .category = "Curve",
        .doc = "Offsets each curve within its plane by a distance: outward for closed curves, to the left for open ones. Loops the "
               "offset makes are trimmed, so a curve can give several.",
        .props = {prop("join", {"round", "miter", "square"}, "round")},
        .sockets = {.inputs = {geoIn("curve"), in("distance", T::Float, 10.0, "cm; negative goes the other way.")}, .outputs = {geoOut("curve")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                curve::OffsetOptions o;
                o.distance = c.f("distance");
                curve::parseJoinType(c.prop("join"), &o.join);
                c.out("curve", editComponents(c.geometry("curve"), {.curves = [&](CurveSet& cs) {
                                                                        CurveSet out;
                                                                        for (size_t cu = 0; cu < cs.numCurves(); ++cu)
                                                                        {
                                                                            curve::Spline sp = cs.spline(cu);
                                                                            if (!curve::isValid(sp))
                                                                                continue;
                                                                            try
                                                                            {
                                                                                for (const curve::Spline& r : curve::offset(sp, o))
                                                                                    if (curve::isValid(r))
                                                                                        out.addSpline(r);
                                                                            }
                                                                            catch (const std::invalid_argument&)
                                                                            {
                                                                                // Degenerate curve: nothing to offset.
                                                                            }
                                                                        }
                                                                        cs = std::move(out);
                                                                    }}));
            },
    });
}
} // namespace lila::gn::nodes::curveops

namespace lila::gn::nodes
{
void registerCurveOpsNodes()
{
    curveops::trimCurve();
    curveops::reverseCurve();
    curveops::subdivideCurve();
    curveops::sampleCurve();
    curveops::readNodes();
    curveops::setTilt();
    curveops::interpolateCurves();
    curveops::curveToPoints();
    curveops::quadrilateral();
    curveops::star();
    curveops::arc();
    curveops::stringToCurves();
    curveops::offsetCurve();
}
} // namespace lila::gn::nodes
