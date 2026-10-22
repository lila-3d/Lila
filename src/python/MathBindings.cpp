#include "math/Math.h"

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/operators.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/vector.h>

#include <array>
#include <charconv>
#include <limits>
#include <string>

namespace nb = nanobind;
using namespace nb::literals;

namespace lila::python
{
/** A plain Python sequence (tuple, list, numpy row...) of MinN..MaxN numbers. Never a bound math type. */
template <typename T, size_t MinN, size_t MaxN> struct NumSeq
{
    std::array<T, MaxN> v{};
    size_t n = 0;
};
} // namespace lila::python

namespace nanobind::detail
{
template <typename T, size_t MinN, size_t MaxN> struct type_caster<lila::python::NumSeq<T, MinN, MaxN>>
{
    using Seq = lila::python::NumSeq<T, MinN, MaxN>;
    NB_TYPE_CASTER(Seq, const_name("collections.abc.Sequence[") + make_caster<T>::Name + const_name("]"))

    bool from_python(handle src, uint32_t, cleanup_list*) noexcept
    {
        PyObject* o = src.ptr();
        // Bound types are excluded so a Rotator never converts to a Vector by way of its __getitem__.
        if (PyUnicode_Check(o) || PyBytes_Check(o) || PyByteArray_Check(o) || inst_check(src) || !PySequence_Check(o))
            return false;
        const Py_ssize_t n = PySequence_Size(o);
        if (n < 0)
        {
            PyErr_Clear();
            return false;
        }
        if (size_t(n) < MinN || size_t(n) > MaxN)
            return false;
        for (Py_ssize_t i = 0; i < n; ++i)
        {
            PyObject* item = PySequence_GetItem(o, i);
            if (!item)
            {
                PyErr_Clear();
                return false;
            }
            bool ok = true;
            if constexpr (std::is_floating_point_v<T>)
            {
                const double d = PyFloat_AsDouble(item);
                ok = !(d == -1.0 && PyErr_Occurred());
                value.v[size_t(i)] = T(d);
            }
            else
            {
                const long long l = PyLong_AsLongLong(item);
                ok = !(l == -1 && PyErr_Occurred()) && l >= (long long)std::numeric_limits<T>::min() &&
                     l <= (long long)std::numeric_limits<T>::max();
                value.v[size_t(i)] = T(l);
            }
            Py_DECREF(item);
            if (!ok)
            {
                PyErr_Clear();
                return false;
            }
        }
        value.n = size_t(n);
        return true;
    }

    static handle from_cpp(const Seq& s, rv_policy, cleanup_list*) noexcept
    {
        PyObject* t = PyTuple_New(Py_ssize_t(s.n));
        if (!t)
            return {};
        for (size_t i = 0; i < s.n; ++i)
        {
            PyObject* item;
            if constexpr (std::is_floating_point_v<T>)
                item = PyFloat_FromDouble(double(s.v[i]));
            else
                item = PyLong_FromLongLong((long long)s.v[i]);
            PyTuple_SET_ITEM(t, Py_ssize_t(i), item);
        }
        return t;
    }
};
} // namespace nanobind::detail

namespace lila::python
{
namespace
{
using namespace lila::math;

template <size_t N> using FloatSeq = NumSeq<double, N, N>;

// Formatting ---------------------------------------------------------------------------------

/** Python-style shortest round-trip repr: 1.0, 0.1, 1e-05. */
template <typename F> std::string fmtFloat(F v)
{
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    std::string s(buf, end);
    if (s.find_first_of(".eni") == std::string::npos)
        s += ".0";
    return s;
}

std::string fmt(double v) { return fmtFloat(v); }
std::string fmt(float v) { return fmtFloat(v); }
std::string fmt(uint8_t v) { return std::to_string(int(v)); }
std::string fmt(int32_t v) { return std::to_string(v); }

// Field traits drive repr, indexing, iteration and pickling ----------------------------------

template <typename T> struct Fields;

#define LILA_FIELDS(Type, Elem, ...)                                                                              \
    template <> struct Fields<Type>                                                                               \
    {                                                                                                             \
        using E = Elem;                                                                                           \
        static constexpr const char* typeName = #Type;                                                            \
        static constexpr std::array names{__VA_ARGS__};                                                           \
        static constexpr int N = int(names.size());                                                               \
        static E& at(Type& v, int i);                                                                             \
    };

LILA_FIELDS(Vector2, double, "x", "y")
LILA_FIELDS(Vector, double, "x", "y", "z")
LILA_FIELDS(Vector4, double, "x", "y", "z", "w")
LILA_FIELDS(IntPoint, int32_t, "x", "y")
LILA_FIELDS(IntVector, int32_t, "x", "y", "z")
LILA_FIELDS(Rotator, double, "pitch", "yaw", "roll")
LILA_FIELDS(Quat, double, "x", "y", "z", "w")
LILA_FIELDS(Plane, double, "x", "y", "z", "w")
LILA_FIELDS(LinearColor, float, "r", "g", "b", "a")
LILA_FIELDS(Color, uint8_t, "r", "g", "b", "a")
#undef LILA_FIELDS

double& Fields<Vector2>::at(Vector2& v, int i) { return v[i]; }
double& Fields<Vector>::at(Vector& v, int i) { return v[i]; }
double& Fields<Vector4>::at(Vector4& v, int i) { return v[i]; }
int32_t& Fields<IntPoint>::at(IntPoint& v, int i) { return i == 0 ? v.x : v.y; }
int32_t& Fields<IntVector>::at(IntVector& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }
double& Fields<Rotator>::at(Rotator& v, int i) { return i == 0 ? v.pitch : (i == 1 ? v.yaw : v.roll); }
double& Fields<Quat>::at(Quat& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : (i == 2 ? v.z : v.w)); }
double& Fields<Plane>::at(Plane& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : (i == 2 ? v.z : v.w)); }
float& Fields<LinearColor>::at(LinearColor& v, int i) { return v[i]; }
uint8_t& Fields<Color>::at(Color& v, int i) { return i == 0 ? v.r : (i == 1 ? v.g : (i == 2 ? v.b : v.a)); }

template <typename T> typename Fields<T>::E get(const T& v, int i) { return Fields<T>::at(const_cast<T&>(v), i); }

int pyIndex(int i, int n)
{
    if (i < 0)
        i += n;
    if (i < 0 || i >= n)
        throw nb::index_error("index out of range");
    return i;
}

template <typename T> std::string reprOf(const T& v)
{
    using F = Fields<T>;
    std::string s = std::string(F::typeName) + "(";
    for (int i = 0; i < F::N; ++i)
    {
        if (i)
            s += ", ";
        s += std::string(F::names[size_t(i)]) + "=" + fmt(get(v, i));
    }
    return s + ")";
}

template <typename T> nb::tuple toTuple(const T& v)
{
    using F = Fields<T>;
    nb::list l;
    for (int i = 0; i < F::N; ++i)
        l.append(get(v, i));
    return nb::tuple(l);
}

/** repr, sequence protocol, equality, copy and pickling, all from Fields<T>. */
template <typename T> void defValueProtocol(nb::class_<T>& cls)
{
    using F = Fields<T>;
    using E = typename F::E;
    cls.def("__repr__", &reprOf<T>)
        .def("__len__", [](const T&) { return F::N; })
        .def("__getitem__", [](const T& v, int i) { return get(v, pyIndex(i, F::N)); })
        .def("__setitem__", [](T& v, int i, E value) { F::at(v, pyIndex(i, F::N)) = value; })
        .def("__iter__", [](const T& v) { return nb::iter(toTuple(v)); })
        .def("to_tuple", &toTuple<T>)
        .def("__getstate__", &toTuple<T>)
        .def("__setstate__",
             [](T& v, const nb::tuple& state)
             {
                 if (nb::len(state) != size_t(F::N))
                     throw nb::value_error("bad pickle state");
                 new (&v) T();
                 for (int i = 0; i < F::N; ++i)
                     F::at(v, i) = nb::cast<E>(state[size_t(i)]);
             })
        .def("__copy__", [](const T& v) { return v; })
        .def("__deepcopy__", [](const T& v, nb::handle) { return v; }, "memo"_a)
        .def("copy", [](const T& v) { return v; })
        .def("__eq__", [](const T& a, const T& b) { return a == b; }, nb::is_operator())
        .def("__ne__", [](const T& a, const T& b) { return !(a == b); }, nb::is_operator());
    cls.attr("__hash__") = nb::none();
    for (int i = 0; i < F::N; ++i)
        cls.def_prop_rw(
            F::names[size_t(i)], [i](const T& v) { return get(v, i); }, [i](T& v, E value) { F::at(v, i) = value; });
}

template <typename T, typename SeqT> T fromSeq(const SeqT& s)
{
    T v{};
    for (size_t i = 0; i < s.n; ++i)
        Fields<T>::at(v, int(i)) = typename Fields<T>::E(s.v[i]);
    return v;
}

/** Constructor from a plain sequence, also used for implicit conversion of tuples and lists. */
template <typename T, typename SeqT> void defSeqInit(nb::class_<T>& cls)
{
    cls.def("__init__", [](T* self, const SeqT& s) { new (self) T(fromSeq<T>(s)); }, "values"_a);
    nb::implicitly_convertible<SeqT, T>();
}

// Bulk / numpy -------------------------------------------------------------------------------

using InArray = nb::ndarray<const double, nb::ndim<2>, nb::device::cpu>;
using IoArray = nb::ndarray<double, nb::ndim<2>, nb::device::cpu>;
using OutArray = nb::ndarray<nb::numpy, double, nb::ndim<2>>;

OutArray newArray(size_t rows, size_t cols, double*& data)
{
    data = new double[rows * cols];
    nb::capsule owner(data, [](void* p) noexcept { delete[] static_cast<double*>(p); });
    return OutArray(data, {rows, cols}, owner);
}

void requireCols(size_t actual, size_t cols)
{
    if (actual != cols)
        throw nb::value_error(("expected an (N, " + std::to_string(cols) + ") array").c_str());
}

/** Maps each 3-component row through fn into a new (N, 3) float64 numpy array. */
template <typename Fn> OutArray mapRows(const InArray& in, Fn fn)
{
    requireCols(in.shape(1), 3);
    const size_t n = in.shape(0);
    double* out;
    OutArray result = newArray(n, 3, out);
    auto v = in.view();
    for (size_t i = 0; i < n; ++i)
    {
        const Vector r = fn(Vector(v(i, 0), v(i, 1), v(i, 2)));
        out[i * 3 + 0] = r.x;
        out[i * 3 + 1] = r.y;
        out[i * 3 + 2] = r.z;
    }
    return result;
}

template <typename Fn> void mapRowsInPlace(IoArray& io, Fn fn)
{
    requireCols(io.shape(1), 3);
    auto v = io.view();
    for (size_t i = 0; i < io.shape(0); ++i)
    {
        const Vector r = fn(Vector(v(i, 0), v(i, 1), v(i, 2)));
        v(i, 0) = r.x;
        v(i, 1) = r.y;
        v(i, 2) = r.z;
    }
}

template <typename T> OutArray rowsToArray(const std::vector<T>& items)
{
    constexpr int n = Fields<T>::N;
    double* out;
    OutArray result = newArray(items.size(), n, out);
    for (size_t i = 0; i < items.size(); ++i)
        for (int j = 0; j < n; ++j)
            out[i * n + size_t(j)] = get(items[i], j);
    return result;
}

nb::list arrayToVectors(const InArray& in)
{
    const size_t cols = in.shape(1);
    if (cols < 2 || cols > 4)
        throw nb::value_error("expected an (N, 2), (N, 3) or (N, 4) array");
    auto v = in.view();
    nb::list out;
    for (size_t i = 0; i < in.shape(0); ++i)
    {
        if (cols == 2)
            out.append(Vector2(v(i, 0), v(i, 1)));
        else if (cols == 3)
            out.append(Vector(v(i, 0), v(i, 1), v(i, 2)));
        else
            out.append(Vector4(v(i, 0), v(i, 1), v(i, 2), v(i, 3)));
    }
    return out;
}

/** Defines transform_positions / transform_vectors (new array) and their _in_place forms. */
template <typename T> void defBulkTransforms(nb::class_<T>& cls)
{
    cls.def(
           "transform_positions",
           [](const T& t, const InArray& a) { return mapRows(a, [&](const Vector& p) { return t.transformPosition(p); }); },
           "points"_a, "Transforms an (N, 3) array of positions into a new float64 numpy array.")
        .def(
            "transform_vectors",
            [](const T& t, const InArray& a) { return mapRows(a, [&](const Vector& p) { return t.transformVector(p); }); },
            "vectors"_a)
        .def(
            "transform_positions_in_place",
            [](const T& t, IoArray a) { mapRowsInPlace(a, [&](const Vector& p) { return t.transformPosition(p); }); },
            nb::arg("points").noconvert(), "Overwrites a writable (N, 3) float64 array, no copy.")
        .def(
            "transform_vectors_in_place",
            [](const T& t, IoArray a) { mapRowsInPlace(a, [&](const Vector& p) { return t.transformVector(p); }); },
            nb::arg("vectors").noconvert());
}

// Misc helpers -------------------------------------------------------------------------------

UpAxis parseUpAxis(const std::string& s)
{
    if (s == "y" || s == "Y")
        return UpAxis::Y;
    if (s == "z" || s == "Z")
        return UpAxis::Z;
    throw nb::value_error("up axis must be 'y' or 'z'");
}

Quat rotationFromObject(nb::handle h)
{
    if (h.is_none())
        return Quat::identity();
    Quat q;
    if (nb::try_cast<Quat>(h, q))
        return q;
    Rotator r;
    if (nb::try_cast<Rotator>(h, r))
        return r.quaternion();
    throw nb::type_error("rotation must be a Quat, a Rotator or a sequence of 3 (rotator) or 4 (quat) numbers");
}

// Defaults of a bound type would be held by the function object and leak at shutdown, so they arrive as None.
Vector safeNormal(const Vector& v, double tolerance, std::optional<Vector> resultIfZero)
{
    return v.getSafeNormal(tolerance, resultIfZero.value_or(Vector::zero()));
}

Vector safeNormal2D(const Vector& v, double tolerance, std::optional<Vector> resultIfZero)
{
    return v.getSafeNormal2D(tolerance, resultIfZero.value_or(Vector::zero()));
}

nb::tuple vecState(const Vector& v) { return nb::make_tuple(v.x, v.y, v.z); }
Vector vecFromState(nb::handle h) { return fromSeq<Vector>(nb::cast<FloatSeq<3>>(h)); }

std::string boxRepr(const Box& b)
{
    return "Box(min=" + reprOf(b.min) + ", max=" + reprOf(b.max) + ", is_valid=" + (b.isValid ? "True" : "False") +
           ")";
}

std::string transformRepr(const Transform& t)
{
    return "Transform(rotation=" + reprOf(t.rotation) + ", translation=" + reprOf(t.translation) +
           ", scale3d=" + reprOf(t.scale3d) + ")";
}

std::string matrixRepr(const Matrix& m)
{
    std::string s = "Matrix([";
    for (int r = 0; r < 4; ++r)
    {
        s += r ? ", [" : "[";
        for (int c = 0; c < 4; ++c)
            s += (c ? ", " : "") + fmt(m(r, c));
        s += "]";
    }
    return s + "])";
}

Matrix matrixFromRows(nb::handle rows)
{
    if (nb::len(rows) != 4)
        throw nb::value_error("Matrix needs 4 rows of 4 numbers");
    Matrix m;
    for (int r = 0; r < 4; ++r)
    {
        const FloatSeq<4> row = nb::cast<FloatSeq<4>>(rows[size_t(r)]);
        for (int c = 0; c < 4; ++c)
            m(r, c) = row.v[size_t(c)];
    }
    return m;
}

// Per-type bindings --------------------------------------------------------------------------

void bindVector2(nb::module_& m)
{
    nb::class_<Vector2> c(m, "Vector2", "2D vector of doubles.");
    c.def(nb::init<double, double>(), "x"_a = 0.0, "y"_a = 0.0).def(nb::init<const Vector2&>());
    defSeqInit<Vector2, FloatSeq<2>>(c);
    defValueProtocol(c);
    c.def_prop_ro_static("ZERO", [](nb::handle) { return Vector2::zero(); })
        .def_prop_ro_static("ONE", [](nb::handle) { return Vector2::one(); })
        .def_prop_ro_static("UNIT45_DEG", [](nb::handle) { return Vector2::unit45Deg(); })
        .def(-nb::self)
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * nb::self)
        .def(nb::self / nb::self)
        .def(nb::self * double())
        .def(double() * nb::self)
        .def(nb::self / double())
        .def(nb::self += nb::self)
        .def(nb::self -= nb::self)
        .def(nb::self *= double())
        .def(nb::self /= double())
        .def(nb::self | nb::self)
        .def(nb::self ^ nb::self)
        .def("dot", &Vector2::dot, "b"_a)
        .def("cross", &Vector2::cross, "b"_a)
        .def("size", &Vector2::size)
        .def("length", &Vector2::size)
        .def("size_squared", &Vector2::sizeSquared)
        .def("length_squared", &Vector2::sizeSquared)
        .def("dist", &Vector2::dist, "b"_a)
        .def("distance", &Vector2::dist, "b"_a)
        .def("dist_squared", &Vector2::distSquared, "b"_a)
        .def("normalize", &Vector2::normalize, "tolerance"_a = kSmallNumber)
        .def("get_safe_normal", &Vector2::getSafeNormal, "tolerance"_a = kSmallNumber)
        .def("normal", &Vector2::getSafeNormal, "tolerance"_a = kSmallNumber)
        .def("is_nearly_zero", &Vector2::isNearlyZero, "tolerance"_a = kKindaSmallNumber)
        .def("is_zero", &Vector2::isZero)
        .def("equals", &Vector2::equals, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("get_abs", &Vector2::getAbs)
        .def("get_max", &Vector2::getMax)
        .def("get_min", &Vector2::getMin)
        .def("get_abs_max", &Vector2::getAbsMax)
        .def("get_abs_min", &Vector2::getAbsMin)
        .def("component_min", &Vector2::componentMin, "b"_a)
        .def("component_max", &Vector2::componentMax, "b"_a)
        .def("get_sign_vector", &Vector2::getSignVector)
        .def("get_rotated", &Vector2::getRotated, "angle_deg"_a)
        .def("clamp_axes", &Vector2::clampAxes, "min_axis"_a, "max_axis"_a)
        .def("lerp", &Vector2::lerp, "b"_a, "alpha"_a, "Vector2.lerp(a, b, alpha) or a.lerp(b, alpha).");
}

void bindVector(nb::module_& m)
{
    nb::class_<Vector> c(m, "Vector",
                         "3D vector of doubles, Lila space (right-handed, cm). UP, FORWARD, RIGHT and the 2D "
                         "(ground plane) helpers follow math.up_axis().");
    c.def(nb::init<double, double, double>(), "x"_a = 0.0, "y"_a = 0.0, "z"_a = 0.0).def(nb::init<const Vector&>());
    defSeqInit<Vector, FloatSeq<3>>(c);
    defValueProtocol(c);

    c.def_prop_ro_static("ZERO", [](nb::handle) { return Vector::zero(); })
        .def_prop_ro_static("ONE", [](nb::handle) { return Vector::one(); })
        .def_prop_ro_static("X_AXIS", [](nb::handle) { return Vector::xAxis(); })
        .def_prop_ro_static("Y_AXIS", [](nb::handle) { return Vector::yAxis(); })
        .def_prop_ro_static("Z_AXIS", [](nb::handle) { return Vector::zAxis(); })
        .def_prop_ro_static("UP", [](nb::handle) { return Vector::up(); })
        .def_prop_ro_static("DOWN", [](nb::handle) { return Vector::down(); })
        .def_prop_ro_static("FORWARD", [](nb::handle) { return Vector::forward(); })
        .def_prop_ro_static("BACKWARD", [](nb::handle) { return Vector::backward(); })
        .def_prop_ro_static("RIGHT", [](nb::handle) { return Vector::right(); })
        .def_prop_ro_static("LEFT", [](nb::handle) { return Vector::left(); });

    c.def(-nb::self)
        .def("__pos__", [](const Vector& v) { return v; })
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * nb::self)
        .def(nb::self / nb::self)
        .def(nb::self + double())
        .def(nb::self - double())
        .def(nb::self * double())
        .def(double() * nb::self)
        .def(nb::self / double())
        .def("__radd__", [](const Vector& v, double s) { return v + s; }, nb::is_operator())
        .def("__rsub__", [](const Vector& v, double s) { return Vector(s) - v; }, nb::is_operator())
        .def(nb::self += nb::self)
        .def(nb::self -= nb::self)
        .def(nb::self *= nb::self)
        .def(nb::self /= nb::self)
        .def(nb::self *= double())
        .def(nb::self /= double())
        .def(nb::self | nb::self)
        .def(nb::self ^ nb::self);

    c.def("dot", &Vector::dot, "b"_a)
        .def("cross", &Vector::cross, "b"_a)
        .def("size", &Vector::size)
        .def("length", &Vector::size)
        .def("size_squared", &Vector::sizeSquared)
        .def("length_squared", &Vector::sizeSquared)
        .def("size_2d", &Vector::size2D, "Size on the ground plane (up component dropped).")
        .def("size_squared_2d", &Vector::sizeSquared2D)
        .def("flatten", &Vector::flatten, "This with the up component removed.")
        .def("dist", &Vector::dist, "b"_a)
        .def("distance", &Vector::dist, "b"_a)
        .def("dist_squared", &Vector::distSquared, "b"_a)
        .def("dist_2d", &Vector::dist2D, "b"_a)
        .def("dist_squared_2d", &Vector::distSquared2D, "b"_a)
        .def("normalize", &Vector::normalize, "tolerance"_a = kSmallNumber,
             "Normalizes in place. False, and unchanged, if the squared size is within tolerance of zero.")
        .def("get_safe_normal", &safeNormal, "tolerance"_a = kSmallNumber, "result_if_zero"_a = nb::none())
        .def("normal", &safeNormal, "tolerance"_a = kSmallNumber, "result_if_zero"_a = nb::none())
        .def("get_safe_normal_2d", &safeNormal2D, "tolerance"_a = kSmallNumber, "result_if_zero"_a = nb::none())
        .def("get_unsafe_normal", &Vector::getUnsafeNormal)
        .def("is_nearly_zero", &Vector::isNearlyZero, "tolerance"_a = kKindaSmallNumber)
        .def("is_zero", &Vector::isZero)
        .def("is_normalized", &Vector::isNormalized)
        .def("is_unit", &Vector::isUnit, "length_squared_tolerance"_a = kKindaSmallNumber)
        .def("is_uniform", &Vector::isUniform, "tolerance"_a = kKindaSmallNumber)
        .def("equals", &Vector::equals, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("contains_nan", &Vector::containsNaN)
        .def("clamp_size", &Vector::clampSize, "min"_a, "max"_a)
        .def("clamp_size_2d", &Vector::clampSize2D, "min"_a, "max"_a)
        .def("clamp_max_size", &Vector::clampMaxSize, "max"_a)
        .def("projection", &Vector::projection, "Homogeneous divide by z: (x/z, y/z, 1).")
        .def("project_on_to", &Vector::projectOnTo, "target"_a)
        .def("project_on_to_normal", &Vector::projectOnToNormal, "normal"_a)
        .def("project_on_plane", &Vector::projectOnPlane, "plane_normal"_a)
        .def("mirror_by_vector", &Vector::mirrorByVector, "mirror_normal"_a)
        .def("mirror_by_plane", &Vector::mirrorByPlane, "plane"_a)
        .def("rotate_angle_axis", &Vector::rotateAngleAxis, "angle_deg"_a, "axis"_a,
             "Right-handed rotation in Lila space.")
        .def("heading_angle", &Vector::headingAngle, "Radians on the ground plane from FORWARD toward RIGHT.")
        .def("cosine_angle_2d", &Vector::cosineAngle2D, "b"_a)
        .def("get_abs", &Vector::getAbs)
        .def("get_max", &Vector::getMax)
        .def("get_min", &Vector::getMin)
        .def("get_abs_max", &Vector::getAbsMax)
        .def("get_abs_min", &Vector::getAbsMin)
        .def("component_min", &Vector::componentMin, "b"_a)
        .def("component_max", &Vector::componentMax, "b"_a)
        .def("get_sign_vector", &Vector::getSignVector)
        .def("grid_snap", &Vector::gridSnap, "grid"_a)
        .def("bound_to_cube", &Vector::boundToCube, "radius"_a)
        .def("bound_to_box", &Vector::boundToBox, "min"_a, "max"_a)
        .def("rotation", &Vector::rotation, "Rotator facing along this vector, zero roll.")
        .def("to_rotator", &Vector::toRotator)
        .def("to_orientation_quat", &Vector::toOrientationQuat)
        .def("lerp", &Vector::lerp, "b"_a, "alpha"_a, "Vector.lerp(a, b, alpha) or a.lerp(b, alpha).");
}

void bindVector4(nb::module_& m)
{
    nb::class_<Vector4> c(m, "Vector4");
    c.def(nb::init<double, double, double, double>(), "x"_a = 0.0, "y"_a = 0.0, "z"_a = 0.0, "w"_a = 0.0)
        .def(nb::init<const Vector&, double>(), "xyz"_a, "w"_a = 1.0)
        .def(nb::init<const Vector4&>());
    defSeqInit<Vector4, FloatSeq<4>>(c);
    defValueProtocol(c);
    c.def(-nb::self)
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * nb::self)
        .def(nb::self / nb::self)
        .def(nb::self * double())
        .def(double() * nb::self)
        .def(nb::self / double())
        .def("xyz", &Vector4::xyz)
        .def("dot3", &Vector4::dot3, "b"_a)
        .def("dot4", &Vector4::dot4, "b"_a)
        .def("size", &Vector4::size)
        .def("size_squared", &Vector4::sizeSquared)
        .def("size3", &Vector4::size3)
        .def("size_squared3", &Vector4::sizeSquared3)
        .def("get_safe_normal", &Vector4::getSafeNormal, "tolerance"_a = kSmallNumber)
        .def("is_nearly_zero3", &Vector4::isNearlyZero3, "tolerance"_a = kKindaSmallNumber)
        .def("equals", &Vector4::equals, "b"_a, "tolerance"_a = kKindaSmallNumber);
}

void bindIntTypes(nb::module_& m)
{
    nb::class_<IntPoint> p(m, "IntPoint");
    p.def(nb::init<int32_t, int32_t>(), "x"_a = 0, "y"_a = 0).def(nb::init<const IntPoint&>());
    defSeqInit<IntPoint, NumSeq<int32_t, 2, 2>>(p);
    defValueProtocol(p);
    p.def_prop_ro_static("ZERO", [](nb::handle) { return IntPoint(); })
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * int32_t())
        .def(nb::self / int32_t())
        .def("size", &IntPoint::size)
        .def("size_squared", &IntPoint::sizeSquared)
        .def("get_max", &IntPoint::getMax)
        .def("get_min", &IntPoint::getMin);

    nb::class_<IntVector> v(m, "IntVector");
    v.def(nb::init<int32_t, int32_t, int32_t>(), "x"_a = 0, "y"_a = 0, "z"_a = 0)
        .def(nb::init<const IntVector&>())
        .def("__init__", [](IntVector* self, const Vector& f) { new (self) IntVector(f); }, "vector"_a,
             "Truncates toward zero.");
    defSeqInit<IntVector, NumSeq<int32_t, 3, 3>>(v);
    defValueProtocol(v);
    v.def_prop_ro_static("ZERO", [](nb::handle) { return IntVector(); })
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * int32_t())
        .def(nb::self / int32_t())
        .def("size", &IntVector::size)
        .def("get_max", &IntVector::getMax)
        .def("get_min", &IntVector::getMin)
        .def("to_vector", &IntVector::toVector);
}

void bindRotator(nb::module_& m)
{
    nb::class_<Rotator> c(m, "Rotator",
                          "Pitch, yaw, roll in degrees with Unreal's meaning on Lila's FORWARD/RIGHT/UP axes: yaw "
                          "turns forward toward right, pitch turns forward toward up, roll turns up toward right. "
                          "Positional order is (pitch, yaw, roll) like FRotator; note unreal.Rotator in UE's Python is "
                          "(roll, pitch, yaw), so prefer keywords when porting.");
    c.def(nb::init<double, double, double>(), "pitch"_a = 0.0, "yaw"_a = 0.0, "roll"_a = 0.0)
        .def(nb::init<const Rotator&>())
        .def(nb::init<const Quat&>(), "quat"_a);
    defSeqInit<Rotator, FloatSeq<3>>(c);
    defValueProtocol(c);
    c.def_prop_ro_static("ZERO", [](nb::handle) { return Rotator::zero(); })
        .def(-nb::self)
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * double())
        .def(double() * nb::self)
        .def(nb::self += nb::self)
        .def(nb::self -= nb::self)
        .def(nb::self *= double())
        .def("is_nearly_zero", &Rotator::isNearlyZero, "tolerance"_a = kKindaSmallNumber)
        .def("is_zero", &Rotator::isZero)
        .def("equals", &Rotator::equals, "b"_a, "tolerance"_a = kKindaSmallNumber,
             "Per-axis comparison modulo 360.")
        .def("contains_nan", &Rotator::containsNaN)
        .def("normalize", &Rotator::normalize, "Wraps every axis into (-180, 180] in place.")
        .def("get_normalized", &Rotator::getNormalized)
        .def("get_denormalized", &Rotator::getDenormalized, "Every axis wrapped into [0, 360).")
        .def("clamp", &Rotator::clamp, "Every axis wrapped into [0, 360).")
        .def("add", &Rotator::add, "delta_pitch"_a, "delta_yaw"_a, "delta_roll"_a)
        .def("grid_snap", &Rotator::gridSnap, "grid"_a)
        .def("get_manhattan_distance", &Rotator::getManhattanDistance, "b"_a)
        .def("get_inverse", &Rotator::getInverse)
        .def("quaternion", &Rotator::quaternion)
        .def("to_quat", &Rotator::quaternion)
        .def("to_matrix", [](const Rotator& r) { return Matrix::makeFromRotator(r); })
        .def("vector", &Rotator::vector, "Forward direction.")
        .def("get_forward_vector", &Rotator::getForwardVector)
        .def("get_right_vector", &Rotator::getRightVector)
        .def("get_up_vector", &Rotator::getUpVector)
        .def("rotate_vector", &Rotator::rotateVector, "v"_a)
        .def("unrotate_vector", &Rotator::unrotateVector, "v"_a)
        .def("combine", [](const Rotator& a, const Rotator& b) { return composeRotators(a, b); }, "b"_a,
             "This rotation, then b.")
        .def("delta", [](const Rotator& a, const Rotator& b) { return normalizedDeltaRotator(a, b); }, "b"_a,
             "(self - b), normalized.")
        .def("lerp", &rlerp, "b"_a, "alpha"_a, "shortest_path"_a = false);
}

void bindQuat(nb::module_& m)
{
    nb::class_<Quat> c(m, "Quat",
                       "Unit quaternion (x, y, z, w), right-handed in Lila space. a * b applies b first, then a; "
                       "quat * vector rotates the vector.");
    c.def(nb::init<double, double, double, double>(), "x"_a = 0.0, "y"_a = 0.0, "z"_a = 0.0, "w"_a = 1.0)
        .def(nb::init<const Quat&>())
        .def(nb::init<const Vector&, double>(), "axis"_a, "angle_rad"_a)
        .def(nb::init<const Rotator&>(), "rotator"_a);
    defSeqInit<Quat, FloatSeq<4>>(c);
    defValueProtocol(c);
    c.def_prop_ro_static("IDENTITY", [](nb::handle) { return Quat::identity(); })
        .def(nb::self * nb::self)
        .def(nb::self * Vector())
        .def(nb::self * double())
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(-nb::self)
        .def(nb::self *= nb::self)
        .def(nb::self | nb::self)
        .def_static("make_from_rotator", &Quat::makeFromRotator, "rotator"_a)
        .def_static("make_from_axis_angle", [](const Vector& axis, double angle) { return Quat(axis, angle); },
                    "axis"_a, "angle_rad"_a)
        .def_static("find_between_vectors", &Quat::findBetweenVectors, "a"_a, "b"_a)
        .def_static("find_between_normals", &Quat::findBetweenNormals, "a"_a, "b"_a)
        .def_static("slerp", &Quat::slerp, "a"_a, "b"_a, "alpha"_a, "Shortest-arc spherical interpolation.")
        .def_static("slerp_full_path", &Quat::slerpFullPath, "a"_a, "b"_a, "alpha"_a)
        .def_static("fast_lerp", &Quat::fastLerp, "a"_a, "b"_a, "alpha"_a)
        .def("dot", &Quat::dot, "b"_a)
        .def("size", &Quat::size)
        .def("size_squared", &Quat::sizeSquared)
        .def("normalize", &Quat::normalize, "tolerance"_a = kSmallNumber)
        .def("get_normalized", &Quat::getNormalized, "tolerance"_a = kSmallNumber)
        .def("is_normalized", &Quat::isNormalized)
        .def("is_identity", &Quat::isIdentity, "tolerance"_a = kSmallNumber)
        .def("equals", &Quat::equals, "b"_a, "tolerance"_a = kKindaSmallNumber, "q and -q compare equal.")
        .def("contains_nan", &Quat::containsNaN)
        .def("inverse", &Quat::inverse)
        .def("rotate_vector", &Quat::rotateVector, "v"_a)
        .def("unrotate_vector", &Quat::unrotateVector, "v"_a)
        .def("rotator", &Quat::rotator)
        .def("to_rotator", &Quat::rotator)
        .def("to_matrix", [](const Quat& q) { return Matrix::makeFromQuat(q); })
        .def("get_angle", &Quat::getAngle, "Radians.")
        .def("get_rotation_axis", &Quat::getRotationAxis)
        .def(
            "to_axis_and_angle",
            [](const Quat& q)
            {
                Vector axis;
                double angle;
                q.toAxisAndAngle(axis, angle);
                return std::make_pair(axis, angle);
            },
            "(axis, angle_rad).")
        .def("angular_distance", &Quat::angularDistance, "b"_a, "Radians.")
        .def("enforce_shortest_arc_with", &Quat::enforceShortestArcWith, "other"_a)
        .def("get_axis_x", &Quat::getAxisX, "Rotated Lila +X.")
        .def("get_axis_y", &Quat::getAxisY, "Rotated Lila +Y.")
        .def("get_axis_z", &Quat::getAxisZ, "Rotated Lila +Z.")
        .def("get_forward_vector", &Quat::getForwardVector, "Rotated FORWARD.")
        .def("get_right_vector", &Quat::getRightVector, "Rotated RIGHT.")
        .def("get_up_vector", &Quat::getUpVector, "Rotated UP.");
}

void bindMatrix(nb::module_& m)
{
    nb::enum_<Axis>(m, "Axis").value("X", Axis::X).value("Y", Axis::Y).value("Z", Axis::Z);

    nb::class_<Matrix> c(m, "Matrix",
                         "4x4 matrix in Unreal's layout: m[row, col], rows 0-2 are the X/Y/Z axes, row 3 the origin. "
                         "a * b applies a first, then b.");
    c.def(nb::init<>())
        .def(nb::init<const Matrix&>())
        .def(nb::init<const Vector&, const Vector&, const Vector&, const Vector&>(), "x_axis"_a, "y_axis"_a,
             "z_axis"_a, "origin"_a)
        .def(nb::init<const Transform&>(), "transform"_a)
        .def("__init__", [](Matrix* self, nb::sequence rows) { new (self) Matrix(matrixFromRows(rows)); }, "rows"_a,
             "From 4 rows of 4 numbers.");
    c.attr("__hash__") = nb::none();
    c.def_prop_ro_static("IDENTITY", [](nb::handle) { return Matrix::identity(); })
        .def_static("make_translation", &Matrix::makeTranslation, "t"_a)
        .def_static("make_scale", &Matrix::makeScale, "s"_a)
        .def_static("make_from_quat", &Matrix::makeFromQuat, "q"_a)
        .def_static("make_from_rotator", &Matrix::makeFromRotator, "r"_a)
        .def_static("make_from_rotation_translation", &Matrix::makeFromRotationTranslation, "r"_a, "origin"_a)
        .def("__repr__", &matrixRepr)
        .def("__getitem__", [](const Matrix& mx, std::pair<int, int> rc)
             { return mx(pyIndex(rc.first, 4), pyIndex(rc.second, 4)); })
        .def("__getitem__", [](const Matrix& mx, int r)
             { const int i = pyIndex(r, 4); return Vector4(mx(i, 0), mx(i, 1), mx(i, 2), mx(i, 3)); })
        .def("__setitem__", [](Matrix& mx, std::pair<int, int> rc, double v)
             { mx(pyIndex(rc.first, 4), pyIndex(rc.second, 4)) = v; })
        .def("__setitem__",
             [](Matrix& mx, int r, const Vector4& v)
             {
                 const int i = pyIndex(r, 4);
                 for (int j = 0; j < 4; ++j)
                     mx(i, j) = v[j];
             })
        .def("__len__", [](const Matrix&) { return 4; })
        .def("__iter__",
             [](const Matrix& mx)
             {
                 nb::list rows;
                 for (int i = 0; i < 4; ++i)
                     rows.append(Vector4(mx(i, 0), mx(i, 1), mx(i, 2), mx(i, 3)));
                 return nb::iter(rows);
             })
        .def("to_rows",
             [](const Matrix& mx)
             {
                 nb::list rows;
                 for (int i = 0; i < 4; ++i)
                     rows.append(nb::make_tuple(mx(i, 0), mx(i, 1), mx(i, 2), mx(i, 3)));
                 return rows;
             })
        .def("__getstate__",
             [](const Matrix& mx)
             {
                 nb::list l;
                 for (int i = 0; i < 16; ++i)
                     l.append(mx(i / 4, i % 4));
                 return nb::tuple(l);
             })
        .def("__setstate__",
             [](Matrix& mx, const nb::tuple& s)
             {
                 if (nb::len(s) != 16)
                     throw nb::value_error("bad pickle state");
                 new (&mx) Matrix();
                 for (int i = 0; i < 16; ++i)
                     mx(i / 4, i % 4) = nb::cast<double>(s[size_t(i)]);
             })
        .def("__copy__", [](const Matrix& mx) { return mx; })
        .def("__deepcopy__", [](const Matrix& mx, nb::handle) { return mx; }, "memo"_a)
        .def("copy", [](const Matrix& mx) { return mx; })
        .def(nb::self * nb::self)
        .def(nb::self * double())
        .def(nb::self + nb::self)
        .def(nb::self *= nb::self)
        .def(nb::self == nb::self)
        .def("__ne__", [](const Matrix& a, const Matrix& b) { return !(a == b); }, nb::is_operator())
        .def("equals", &Matrix::equals, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("transform_vector4", &Matrix::transformVector4, "v"_a)
        .def("transform_position", &Matrix::transformPosition, "p"_a)
        .def("transform_vector", &Matrix::transformVector, "v"_a)
        .def("inverse_transform_position", &Matrix::inverseTransformPosition, "p"_a)
        .def("inverse_transform_vector", &Matrix::inverseTransformVector, "v"_a)
        .def("inverse", &Matrix::inverse, "Identity when singular.")
        .def("get_inverse", &Matrix::inverse)
        .def("transpose", &Matrix::transpose)
        .def("get_transposed", &Matrix::transpose)
        .def("determinant", &Matrix::determinant)
        .def("get_determinant", &Matrix::determinant)
        .def("rot_determinant", &Matrix::rotDeterminant)
        .def("get_origin", &Matrix::getOrigin)
        .def("set_origin", &Matrix::setOrigin, "origin"_a)
        .def("concat_translation", &Matrix::concatTranslation, "t"_a)
        .def("get_scaled_axis", &Matrix::getScaledAxis, "axis"_a)
        .def("get_unit_axis", &Matrix::getUnitAxis, "axis"_a)
        .def("set_axis", &Matrix::setAxis, "axis"_a, "v"_a)
        .def("get_scale_vector", &Matrix::getScaleVector, "tolerance"_a = kSmallNumber)
        .def("remove_scaling", &Matrix::removeScaling, "tolerance"_a = kSmallNumber)
        .def("get_matrix_without_scale", &Matrix::getMatrixWithoutScale, "tolerance"_a = kSmallNumber)
        .def("apply_scale", &Matrix::applyScale, "scale"_a)
        .def("rotator", &Matrix::rotator)
        .def("to_quat", &Matrix::toQuat)
        .def("to_transform", [](const Matrix& mx) { return Transform(mx); });
    defBulkTransforms(c);
}

void bindTransform(nb::module_& m)
{
    nb::class_<Transform> c(m, "Transform",
                            "Rotation (Quat), translation and scale3d; applies scale, rotation, translation. "
                            "a * b applies a first, then b (child * parent). Fields return copies, as in UE's Python.");
    // Exact-type overloads first: the general form takes any object for rotation.
    c.def(nb::init<const Transform&>()).def(nb::init<const Matrix&>(), "matrix"_a);
    c.def(
         "__init__",
         [](Transform* self, nb::handle rotation, std::optional<Vector> translation, std::optional<Vector> scale3d,
            std::optional<Vector> location, std::optional<Vector> scale)
         {
             if (translation && location)
                 throw nb::type_error("pass translation or location, not both");
             if (scale3d && scale)
                 throw nb::type_error("pass scale3d or scale, not both");
             new (self) Transform(rotationFromObject(rotation), translation.value_or(location.value_or(Vector::zero())),
                                  scale3d.value_or(scale.value_or(Vector::one())));
         },
         "rotation"_a = nb::none(), "translation"_a = nb::none(), "scale3d"_a = nb::none(), nb::kw_only(),
         "location"_a = nb::none(), "scale"_a = nb::none(),
         "rotation accepts a Quat or a Rotator; location/scale are keyword aliases as in UE's Python.");
    c.attr("__hash__") = nb::none();
    c.def_prop_ro_static("IDENTITY", [](nb::handle) { return Transform::identity(); })
        .def_prop_rw(
            "rotation", [](const Transform& t) { return t.rotation; },
            [](Transform& t, nb::handle r) { t.rotation = rotationFromObject(r); })
        .def_prop_rw(
            "translation", [](const Transform& t) { return t.translation; },
            [](Transform& t, const Vector& v) { t.translation = v; })
        .def_prop_rw(
            "location", [](const Transform& t) { return t.translation; },
            [](Transform& t, const Vector& v) { t.translation = v; })
        .def_prop_rw(
            "scale3d", [](const Transform& t) { return t.scale3d; }, [](Transform& t, const Vector& v) { t.scale3d = v; })
        .def("__repr__", &transformRepr)
        .def("__getstate__",
             [](const Transform& t)
             {
                 return nb::make_tuple(nb::make_tuple(t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w),
                                       vecState(t.translation), vecState(t.scale3d));
             })
        .def("__setstate__",
             [](Transform& t, const nb::tuple& s)
             {
                 if (nb::len(s) != 3)
                     throw nb::value_error("bad pickle state");
                 const Quat q = fromSeq<Quat>(nb::cast<FloatSeq<4>>(s[0]));
                 new (&t) Transform(q, vecFromState(s[1]), vecFromState(s[2]));
             })
        .def("__copy__", [](const Transform& t) { return t; })
        .def("__deepcopy__", [](const Transform& t, nb::handle) { return t; }, "memo"_a)
        .def("copy", [](const Transform& t) { return t; })
        .def(nb::self * nb::self)
        .def(nb::self == nb::self)
        .def("__ne__", [](const Transform& a, const Transform& b) { return !(a == b); }, nb::is_operator())
        .def("multiply", &Transform::multiply, "b"_a, "self then b.")
        .def_static("blend", &Transform::blend, "a"_a, "b"_a, "alpha"_a,
                    "Lerps translation and scale, slerps rotation.")
        .def("blend_with", &Transform::blendWith, "other"_a, "alpha"_a)
        .def("equals", &Transform::equals, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("equals_no_scale", &Transform::equalsNoScale, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("transform_position", &Transform::transformPosition, "p"_a)
        .def("transform_position_no_scale", &Transform::transformPositionNoScale, "p"_a)
        .def("transform_vector", &Transform::transformVector, "v"_a)
        .def("transform_vector_no_scale", &Transform::transformVectorNoScale, "v"_a)
        .def("transform_rotation", &Transform::transformRotation, "q"_a)
        .def("inverse_transform_position", &Transform::inverseTransformPosition, "p"_a)
        .def("inverse_transform_position_no_scale", &Transform::inverseTransformPositionNoScale, "p"_a)
        .def("inverse_transform_vector", &Transform::inverseTransformVector, "v"_a)
        .def("inverse_transform_vector_no_scale", &Transform::inverseTransformVectorNoScale, "v"_a)
        .def("inverse_transform_rotation", &Transform::inverseTransformRotation, "q"_a)
        .def("inverse", &Transform::inverse, "Exact for uniform scale.")
        .def("get_relative_transform", &Transform::getRelativeTransform, "other"_a,
             "self relative to other: result * other == self.")
        .def("get_relative_transform_reverse", &Transform::getRelativeTransformReverse, "other"_a,
             "other relative to self.")
        .def("to_matrix", &Transform::toMatrix)
        .def("to_matrix_with_scale", &Transform::toMatrixWithScale)
        .def("to_matrix_no_scale", &Transform::toMatrixNoScale)
        .def("rotator", &Transform::rotator)
        .def("get_determinant", &Transform::getDeterminant)
        .def("get_maximum_axis_scale", &Transform::getMaximumAxisScale)
        .def("get_minimum_axis_scale", &Transform::getMinimumAxisScale)
        .def("is_rotation_normalized", &Transform::isRotationNormalized)
        .def("normalize_rotation", &Transform::normalizeRotation)
        .def("get_scaled_axis", &Transform::getScaledAxis, "axis"_a)
        .def("get_unit_axis", &Transform::getUnitAxis, "axis"_a)
        .def("contains_nan", &Transform::containsNaN);
    defBulkTransforms(c);
}

void bindColors(nb::module_& m)
{
    nb::class_<LinearColor> lc(m, "LinearColor", "Linear-space float RGBA.");
    lc.def(nb::init<float, float, float, float>(), "r"_a = 0.0f, "g"_a = 0.0f, "b"_a = 0.0f, "a"_a = 1.0f)
        .def(nb::init<const LinearColor&>())
        .def(nb::init<const Color&>(), "color"_a, "Decodes an sRGB Color.");
    defSeqInit<LinearColor, NumSeq<double, 3, 4>>(lc);
    defValueProtocol(lc);
    lc.def_prop_ro_static("WHITE", [](nb::handle) { return LinearColor::white(); })
        .def_prop_ro_static("GRAY", [](nb::handle) { return LinearColor::gray(); })
        .def_prop_ro_static("BLACK", [](nb::handle) { return LinearColor::black(); })
        .def_prop_ro_static("TRANSPARENT", [](nb::handle) { return LinearColor::transparent(); })
        .def_prop_ro_static("RED", [](nb::handle) { return LinearColor::red(); })
        .def_prop_ro_static("GREEN", [](nb::handle) { return LinearColor::green(); })
        .def_prop_ro_static("BLUE", [](nb::handle) { return LinearColor::blue(); })
        .def_prop_ro_static("YELLOW", [](nb::handle) { return LinearColor::yellow(); })
        .def(nb::self + nb::self)
        .def(nb::self - nb::self)
        .def(nb::self * nb::self)
        .def(nb::self / nb::self)
        .def(nb::self * float())
        .def(float() * nb::self)
        .def(nb::self / float())
        .def_static("from_srgb_color", &LinearColor::fromSRGBColor, "color"_a)
        .def_static("from_hsv", &LinearColor::fromHSV, "hue"_a, "saturation"_a, "value"_a, "alpha"_a = 1.0f,
                    "Hue in degrees, saturation and value 0..1.")
        .def("lerp", &LinearColor::lerp, "b"_a, "alpha"_a)
        .def("equals", &LinearColor::equals, "b"_a, "tolerance"_a = 1.0e-4f)
        .def("to_color", &LinearColor::toColor, "srgb"_a = true, "To 8-bit; srgb encodes the rgb channels.")
        .def("to_fcolor", &LinearColor::toColor, "srgb"_a = true)
        .def("quantize", &LinearColor::quantize)
        .def("quantize_round", &LinearColor::quantizeRound)
        .def("linear_rgb_to_hsv", &LinearColor::linearRGBToHSV, "(hue degrees, saturation, value, alpha) in r, g, b, a.")
        .def("hsv_to_linear_rgb", &LinearColor::hsvToLinearRGB)
        .def("get_luminance", &LinearColor::getLuminance, "Rec. 709 weights.")
        .def("get_max", &LinearColor::getMax)
        .def("get_min", &LinearColor::getMin)
        .def("desaturate", &LinearColor::desaturate, "desaturation"_a)
        .def("get_clamped", &LinearColor::getClamped, "min"_a = 0.0f, "max"_a = 1.0f)
        .def("copy_with_new_opacity", &LinearColor::copyWithNewOpacity, "alpha"_a)
        .def("is_almost_black", &LinearColor::isAlmostBlack)
        .def("to_vector", &LinearColor::toVector);

    nb::class_<Color> c(m, "Color", "8-bit RGBA, sRGB-encoded rgb by convention.");
    c.def(nb::init<uint8_t, uint8_t, uint8_t, uint8_t>(), "r"_a = 0, "g"_a = 0, "b"_a = 0, "a"_a = 255)
        .def(nb::init<const Color&>());
    // Lists of 3 get alpha 255.
    c.def("__init__",
          [](Color* self, const NumSeq<uint8_t, 3, 4>& s)
          { new (self) Color(s.v[0], s.v[1], s.v[2], s.n == 4 ? s.v[3] : uint8_t(255)); },
          "values"_a);
    nb::implicitly_convertible<NumSeq<uint8_t, 3, 4>, Color>();
    defValueProtocol(c);
    c.def_prop_ro_static("WHITE", [](nb::handle) { return Color::white(); })
        .def_prop_ro_static("BLACK", [](nb::handle) { return Color::black(); })
        .def_prop_ro_static("TRANSPARENT", [](nb::handle) { return Color::transparent(); })
        .def_prop_ro_static("RED", [](nb::handle) { return Color::red(); })
        .def_prop_ro_static("GREEN", [](nb::handle) { return Color::green(); })
        .def_prop_ro_static("BLUE", [](nb::handle) { return Color::blue(); })
        .def_prop_ro_static("YELLOW", [](nb::handle) { return Color::yellow(); })
        .def_prop_ro_static("CYAN", [](nb::handle) { return Color::cyan(); })
        .def_prop_ro_static("MAGENTA", [](nb::handle) { return Color::magenta(); })
        .def_prop_ro_static("ORANGE", [](nb::handle) { return Color::orange(); })
        .def_prop_ro_static("PURPLE", [](nb::handle) { return Color::purple(); })
        .def_static(
            "from_hex",
            [](const std::string& hex)
            {
                const std::optional<Color> c = Color::fromHex(hex);
                if (!c)
                    throw nb::value_error(("not a hex color: " + hex).c_str());
                return *c;
            },
            "hex"_a, "RGB, RGBA, RRGGBB or RRGGBBAA, optional '#'.")
        .def("to_hex", &Color::toHex, "'RRGGBBAA'.")
        .def("to_packed_argb", &Color::toPackedARGB)
        .def("to_packed_rgba", &Color::toPackedRGBA)
        .def("to_linear", &Color::toLinear, "sRGB decode.")
        .def("reinterpret_as_linear", &Color::reinterpretAsLinear, "Divides by 255 with no curve.")
        .def("with_alpha", &Color::withAlpha, "alpha"_a);
    // A Color converts to LinearColor wherever one is expected.
    nb::implicitly_convertible<Color, LinearColor>();
}

void bindShapes(nb::module_& m)
{
    nb::class_<Box> b(m, "Box", "Axis-aligned box. Box() is invalid (empty) until a point is added.");
    b.def(nb::init<>())
        .def(nb::init<const Box&>())
        .def(nb::init<const Vector&, const Vector&>(), "min"_a, "max"_a)
        .def("__init__", [](Box* self, const std::vector<Vector>& pts) { new (self) Box(pts); }, "points"_a);
    b.attr("__hash__") = nb::none();
    b.def_prop_rw("min", [](const Box& x) { return x.min; }, [](Box& x, const Vector& v) { x.min = v; })
        .def_prop_rw("max", [](const Box& x) { return x.max; }, [](Box& x, const Vector& v) { x.max = v; })
        .def_rw("is_valid", &Box::isValid)
        .def_static("build_aabb", &Box::buildAABB, "origin"_a, "extent"_a)
        .def("__repr__", &boxRepr)
        .def("__getstate__", [](const Box& x) { return nb::make_tuple(vecState(x.min), vecState(x.max), x.isValid); })
        .def("__setstate__",
             [](Box& x, const nb::tuple& s)
             {
                 if (nb::len(s) != 3)
                     throw nb::value_error("bad pickle state");
                 new (&x) Box(vecFromState(s[0]), vecFromState(s[1]));
                 x.isValid = nb::cast<bool>(s[2]);
             })
        .def("__copy__", [](const Box& x) { return x; })
        .def("__deepcopy__", [](const Box& x, nb::handle) { return x; }, "memo"_a)
        .def("copy", [](const Box& x) { return x; })
        .def(nb::self + nb::self)
        .def(nb::self + Vector())
        .def(nb::self += nb::self)
        .def(nb::self += Vector())
        .def(nb::self == nb::self)
        .def("__ne__", [](const Box& x, const Box& y) { return !(x == y); }, nb::is_operator())
        .def("equals", &Box::equals, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("get_center", &Box::getCenter)
        .def("get_extent", &Box::getExtent, "Half size.")
        .def("get_size", &Box::getSize)
        .def("get_volume", &Box::getVolume)
        .def("get_center_and_extents", [](const Box& x) { return std::make_pair(x.getCenter(), x.getExtent()); })
        .def("is_inside", nb::overload_cast<const Vector&>(&Box::isInside, nb::const_), "point"_a,
             "Strictly inside.")
        .def("is_inside", nb::overload_cast<const Box&>(&Box::isInside, nb::const_), "box"_a)
        .def("is_inside_or_on", &Box::isInsideOrOn, "point"_a)
        .def("intersect", &Box::intersect, "other"_a, "Touching counts.")
        .def("overlap", &Box::overlap, "other"_a)
        .def("expand_by", nb::overload_cast<double>(&Box::expandBy, nb::const_), "w"_a)
        .def("expand_by", nb::overload_cast<const Vector&, const Vector&>(&Box::expandBy, nb::const_), "negative"_a,
             "positive"_a)
        .def("shift_by", &Box::shiftBy, "offset"_a)
        .def("move_to", &Box::moveTo, "center"_a)
        .def("get_closest_point_to", &Box::getClosestPointTo, "point"_a)
        .def("compute_squared_distance_to_point", &Box::computeSquaredDistanceToPoint, "point"_a)
        .def("transform_by", nb::overload_cast<const Transform&>(&Box::transformBy, nb::const_), "transform"_a)
        .def("transform_by", nb::overload_cast<const Matrix&>(&Box::transformBy, nb::const_), "matrix"_a)
        .def("inverse_transform_by", &Box::inverseTransformBy, "transform"_a)
        .def("get_vertices",
             [](const Box& x)
             {
                 const auto v = x.getVertices();
                 return std::vector<Vector>(v.begin(), v.end());
             });

    nb::class_<Plane> p(m, "Plane", "Points p with dot(normal, p) == w.");
    p.def(nb::init<double, double, double, double>(), "x"_a = 0.0, "y"_a = 0.0, "z"_a = 0.0, "w"_a = 0.0)
        .def(nb::init<const Plane&>())
        .def(nb::init<const Vector&, double>(), "normal"_a, "w"_a)
        .def(nb::init<const Vector&, const Vector&>(), "point"_a, "normal"_a)
        .def(nb::init<const Vector&, const Vector&, const Vector&>(), "a"_a, "b"_a, "c"_a,
             "Through three points, right-hand rule normal.");
    defValueProtocol(p);
    p.def("equals", &Plane::equals, "b"_a, "tolerance"_a = kKindaSmallNumber)
        .def("get_normal", &Plane::getNormal)
        .def("get_origin", &Plane::getOrigin)
        .def("is_valid", &Plane::isValid)
        .def("plane_dot", &Plane::planeDot, "p"_a, "Signed distance for a unit normal.")
        .def("flip", &Plane::flip)
        .def("project_point", &Plane::projectPoint, "p"_a)
        .def("transform_by", &Plane::transformBy, "matrix"_a);
}

void bindFunctions(nb::module_& m)
{
    m.attr("PI") = kPi;
    m.attr("SMALL_NUMBER") = kSmallNumber;
    m.attr("KINDA_SMALL_NUMBER") = kKindaSmallNumber;
    m.attr("BIG_NUMBER") = kBigNumber;

    m.def("set_up_axis", [](const std::string& a) { setUpAxis(parseUpAxis(a)); }, "axis"_a,
          "'y' (default) or 'z'. Process-wide; UP/FORWARD/RIGHT, Rotator and to_unreal follow it.");
    m.def("up_axis", [] { return std::string(upAxis() == UpAxis::Z ? "z" : "y"); });

    // Unreal conversion
    m.def("to_unreal", nb::overload_cast<const Vector&>(&toUnreal), "v"_a,
          "Lila space to Unreal space (left-handed Z-up, X forward, Y right). A Vector is a position or direction.")
        .def("to_unreal", nb::overload_cast<const Quat&>(&toUnreal), "q"_a)
        .def("to_unreal", nb::overload_cast<const Rotator&>(&toUnreal), "r"_a, "Identity: rotators share numbers.")
        .def("to_unreal", nb::overload_cast<const Transform&>(&toUnreal), "t"_a)
        .def("to_unreal", nb::overload_cast<const Matrix&>(&toUnreal), "m"_a)
        .def("to_unreal", nb::overload_cast<const Box&>(&toUnreal), "b"_a)
        .def("from_unreal", nb::overload_cast<const Vector&>(&fromUnreal), "v"_a)
        .def("from_unreal", nb::overload_cast<const Quat&>(&fromUnreal), "q"_a)
        .def("from_unreal", nb::overload_cast<const Rotator&>(&fromUnreal), "r"_a)
        .def("from_unreal", nb::overload_cast<const Transform&>(&fromUnreal), "t"_a)
        .def("from_unreal", nb::overload_cast<const Matrix&>(&fromUnreal), "m"_a)
        .def("from_unreal", nb::overload_cast<const Box&>(&fromUnreal), "b"_a)
        .def("scale_to_unreal", &scaleToUnreal, "scale3d"_a, "Per-axis scale; permutes without sign.")
        .def("scale_from_unreal", &scaleFromUnreal, "scale3d"_a)
        .def(
            "to_unreal_array",
            [](const InArray& a) { return mapRows(a, [](const Vector& v) { return toUnreal(v); }); }, "points"_a)
        .def(
            "from_unreal_array",
            [](const InArray& a) { return mapRows(a, [](const Vector& v) { return fromUnreal(v); }); }, "points"_a);

    // Rotator builders
    m.def("find_look_at_rotation", &findLookAtRotation, "start"_a, "target"_a)
        .def("make_rot_from_x", &makeRotFromX, "x"_a,
             "X/Y/Z are the rotator's own axes as in Unreal: X forward, Y right, Z up.")
        .def("make_rot_from_y", &makeRotFromY, "y"_a)
        .def("make_rot_from_z", &makeRotFromZ, "z"_a)
        .def("make_rot_from_xy", &makeRotFromXY, "x"_a, "y"_a)
        .def("make_rot_from_xz", &makeRotFromXZ, "x"_a, "z"_a)
        .def("make_rot_from_yx", &makeRotFromYX, "y"_a, "x"_a)
        .def("make_rot_from_yz", &makeRotFromYZ, "y"_a, "z"_a)
        .def("make_rot_from_zx", &makeRotFromZX, "z"_a, "x"_a)
        .def("make_rot_from_zy", &makeRotFromZY, "z"_a, "y"_a)
        .def("make_rotation_from_axes", &makeRotationFromAxes, "forward"_a, "right"_a, "up"_a)
        .def("compose_rotators", &composeRotators, "a"_a, "b"_a, "a, then b.")
        .def("normalized_delta_rotator", &normalizedDeltaRotator, "a"_a, "b"_a)
        .def("rlerp", &rlerp, "a"_a, "b"_a, "alpha"_a, "shortest_path"_a = false)
        .def("get_forward_vector", &Rotator::getForwardVector, "rotation"_a)
        .def("get_right_vector", &Rotator::getRightVector, "rotation"_a)
        .def("get_up_vector", &Rotator::getUpVector, "rotation"_a)
        .def("get_direction_unit_vector", &getDirectionUnitVector, "from_"_a, "to"_a)
        .def(
            "get_yaw_pitch_from_vector",
            [](const Vector& v)
            {
                double yaw, pitch;
                getYawPitchFromVector(v, yaw, pitch);
                return std::make_pair(yaw, pitch);
            },
            "v"_a, "(yaw, pitch) in degrees.");

    // Interpolation
    m.def("lerp", [](double a, double b, double alpha) { return math::lerp(a, b, alpha); }, "a"_a, "b"_a, "alpha"_a)
        .def("lerp", &Vector::lerp, "a"_a, "b"_a, "alpha"_a)
        .def("lerp", &Vector2::lerp, "a"_a, "b"_a, "alpha"_a)
        .def("lerp", [](const Rotator& a, const Rotator& b, double alpha) { return rlerp(a, b, alpha, false); },
             "a"_a, "b"_a, "alpha"_a)
        .def("lerp", &Quat::slerp, "a"_a, "b"_a, "alpha"_a)
        .def("lerp", &LinearColor::lerp, "a"_a, "b"_a, "alpha"_a)
        .def("lerp", &Transform::blend, "a"_a, "b"_a, "alpha"_a)
        .def("finterp_to", &finterpTo, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("finterp_to_constant", &finterpToConstant, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("vinterp_to", nb::overload_cast<const Vector&, const Vector&, double, double>(&vinterpTo), "current"_a,
             "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("vinterp_to", nb::overload_cast<const Vector2&, const Vector2&, double, double>(&vinterpTo),
             "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("vinterp_to_constant", &vinterpToConstant, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("rinterp_to", &rinterpTo, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("rinterp_to_constant", &rinterpToConstant, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a,
             "interp_speed in degrees per second.")
        .def("qinterp_to", &qinterpTo, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("qinterp_to_constant", &qinterpToConstant, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a,
             "interp_speed in radians per second.")
        .def("cinterp_to", &cinterpTo, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a)
        .def("tinterp_to", &tinterpTo, "current"_a, "target"_a, "delta_time"_a, "interp_speed"_a);

    // Scalars
    m.def("clamp", [](double v, double lo, double hi) { return math::clamp(v, lo, hi); }, "value"_a, "min"_a, "max"_a)
        .def("clamp_angle", &clampAngle, "angle_deg"_a, "min_deg"_a, "max_deg"_a)
        .def("normalize_axis", &normalizeAxis, "angle_deg"_a, "Wraps into (-180, 180].")
        .def("clamp_axis", &clampAxis, "angle_deg"_a, "Wraps into [0, 360).")
        .def("unwind_degrees", &unwindDegrees, "angle_deg"_a)
        .def("unwind_radians", &unwindRadians, "angle_rad"_a)
        .def("wrap", &wrap, "value"_a, "min"_a, "max"_a)
        .def("grid_snap", nb::overload_cast<double, double>(&gridSnap), "value"_a, "grid"_a)
        .def("get_range_pct", &getRangePct, "min"_a, "max"_a, "value"_a)
        .def("map_range_clamped", &mapRangeClamped, "value"_a, "in_range_a"_a, "in_range_b"_a, "out_range_a"_a,
             "out_range_b"_a)
        .def("map_range_unclamped", &mapRangeUnclamped, "value"_a, "in_range_a"_a, "in_range_b"_a, "out_range_a"_a,
             "out_range_b"_a)
        .def("nearly_equal", [](double a, double b, double tol) { return isNearlyEqual(a, b, tol); }, "a"_a, "b"_a,
             "error_tolerance"_a = 1.0e-6)
        .def("is_nearly_zero", [](double a, double tol) { return isNearlyZero(a, tol); }, "value"_a,
             "error_tolerance"_a = 1.0e-6)
        .def("degrees", &radiansToDegrees, "radians"_a)
        .def("radians", &degreesToRadians, "degrees"_a)
        .def("radians_to_degrees", &radiansToDegrees, "radians"_a)
        .def("degrees_to_radians", &degreesToRadians, "degrees"_a)
        .def("dsin", [](double d) { return std::sin(degreesToRadians(d)); }, "degrees"_a)
        .def("dcos", [](double d) { return std::cos(degreesToRadians(d)); }, "degrees"_a)
        .def("dtan", [](double d) { return std::tan(degreesToRadians(d)); }, "degrees"_a)
        .def("dasin", [](double v) { return radiansToDegrees(std::asin(v)); }, "value"_a)
        .def("dacos", [](double v) { return radiansToDegrees(std::acos(v)); }, "value"_a)
        .def("datan", [](double v) { return radiansToDegrees(std::atan(v)); }, "value"_a)
        .def("datan2", [](double y, double x) { return radiansToDegrees(std::atan2(y, x)); }, "y"_a, "x"_a);

    m.def("line_plane_intersection", &linePlaneIntersection, "line_start"_a, "line_end"_a, "plane"_a);

    // Bulk conversion
    m.def("vectors_to_array", &rowsToArray<Vector>, "vectors"_a, "list[Vector] to an (N, 3) float64 numpy array.")
        .def("vectors_to_array", &rowsToArray<Vector2>, "vectors"_a)
        .def("vectors_to_array", &rowsToArray<Vector4>, "vectors"_a)
        .def("array_to_vectors", &arrayToVectors, "array"_a,
             "(N, 2|3|4) array (numpy or any buffer) to a list of Vector2/Vector/Vector4.")
        .def(
            "array_to_vectors",
            [](const std::vector<NumSeq<double, 2, 4>>& rows)
            {
                nb::list out;
                for (const auto& r : rows)
                {
                    if (r.n == 2)
                        out.append(Vector2(r.v[0], r.v[1]));
                    else if (r.n == 3)
                        out.append(Vector(r.v[0], r.v[1], r.v[2]));
                    else
                        out.append(Vector4(r.v[0], r.v[1], r.v[2], r.v[3]));
                }
                return out;
            },
            "rows"_a);
}
} // namespace

void bindMath(nb::module_& m)
{
    m.doc() = "Unreal-flavoured vector math in Lila space (right-handed, Y-up by default, cm).";
    bindVector2(m);
    bindVector(m);
    bindVector4(m);
    bindIntTypes(m);
    bindRotator(m);
    bindQuat(m);
    bindMatrix(m);
    bindTransform(m);
    bindColors(m);
    bindShapes(m);
    bindFunctions(m);
}
} // namespace lila::python
