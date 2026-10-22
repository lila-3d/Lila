"""lila.math tests. Run with: lilabatch.exe --batch X:/Lila/python/tests/test_math.py"""

import copy
import math as pymath
import pickle

import lila
from lila import math as lm
from lila.math import (Box, Color, IntPoint, IntVector, LinearColor, Matrix, Plane, Quat, Rotator, Transform, Vector,
                       Vector2, Vector4)

EPS = 1e-9


def close(a, b, tol=1e-6):
    if isinstance(a, (int, float)):
        return abs(a - b) <= tol
    return a.equals(b, tol)


def for_each_up_axis(fn):
    try:
        for axis in ("y", "z"):
            lm.set_up_axis(axis)
            fn()
    finally:
        lm.set_up_axis("y")


def test_package():
    assert lila.math is lm
    assert lm.up_axis() == "y"


def test_vector_construction_and_dunders():
    v = Vector(1, 2, 3)
    assert repr(v) == "Vector(x=1.0, y=2.0, z=3.0)"
    assert repr(Vector(0.1, -2.5, 1e-5)) == "Vector(x=0.1, y=-2.5, z=1e-05)"
    assert Vector() == Vector(0, 0, 0)
    assert Vector(x=5) == Vector(5, 0, 0)
    assert Vector((1, 2, 3)) == v
    assert Vector([1, 2, 3]) == v
    assert Vector(v) == v and Vector(v) is not v
    x, y, z = v
    assert (x, y, z) == (1.0, 2.0, 3.0)
    assert list(v) == [1.0, 2.0, 3.0]
    assert len(v) == 3
    assert v[0] == 1.0 and v[-1] == 3.0
    try:
        v[3]
        assert False, "expected IndexError"
    except IndexError:
        pass
    w = Vector(v)
    w[1] = 9
    w.z = 7
    assert w == Vector(1, 9, 7)
    assert v == (1, 2, 3)
    assert v != Vector(1, 2, 4)
    assert (v == "abc") is False
    assert v.to_tuple() == (1.0, 2.0, 3.0)

    c = copy.copy(v)
    d = copy.deepcopy(v)
    assert c == v and d == v and c is not v
    assert pickle.loads(pickle.dumps(v)) == v
    try:
        hash(v)
        assert False, "Vector should be unhashable"
    except TypeError:
        pass

    for bad in ("abc", (1, 2), (1, 2, 3, 4), Rotator(1, 2, 3), None):
        try:
            Vector(bad)
            assert False, "accepted %r" % (bad,)
        except TypeError:
            pass


def test_vector_ops():
    a = Vector(1, 2, 3)
    b = Vector(4, 5, 6)
    assert a + b == Vector(5, 7, 9)
    assert b - a == Vector(3, 3, 3)
    assert a * b == Vector(4, 10, 18)
    assert a * 2 == Vector(2, 4, 6) and 2 * a == Vector(2, 4, 6)
    assert a / 2 == Vector(0.5, 1, 1.5)
    assert -a == Vector(-1, -2, -3)
    assert a + 1 == Vector(2, 3, 4) and 1 + a == Vector(2, 3, 4)
    assert 10 - a == Vector(9, 8, 7)
    assert (a | b) == 32.0 and a.dot(b) == 32.0
    assert (a ^ b) == Vector(-3, 6, -3) and a.cross(b) == Vector(-3, 6, -3)
    assert a + (1, 1, 1) == Vector(2, 3, 4)
    assert a.dot((1, 0, 0)) == 1.0
    c = Vector(a)
    c += b
    assert c == Vector(5, 7, 9)
    c *= 2
    assert c == Vector(10, 14, 18)

    assert close(Vector(3, 4, 0).size(), 5) and close(Vector(3, 4, 0).length(), 5)
    assert Vector(3, 4, 0).size_squared() == 25
    assert close(a.dist(b), pymath.sqrt(27)) and close(a.distance(b), a.dist(b))
    n = Vector(0, 3, 4)
    assert n.normalize() is True and close(n, Vector(0, 0.6, 0.8))
    z = Vector()
    assert z.normalize() is False
    assert Vector.ZERO.get_safe_normal() == Vector.ZERO
    assert Vector().get_safe_normal(result_if_zero=Vector.UP) == Vector.UP
    assert Vector(1e-6, 0, 0).is_nearly_zero() and not Vector(1, 0, 0).is_nearly_zero()
    assert Vector().is_zero()
    assert Vector(1, 1, 1).equals(Vector(1, 1, 1.00001))
    assert not Vector(1, 1, 1).equals(Vector(1, 1, 1.1))
    assert close(Vector(10, 0, 0).clamp_size(1, 5), Vector(5, 0, 0))
    assert close(Vector(10, 0, 0).clamp_max_size(2), Vector(2, 0, 0))
    assert close(Vector(2, 4, 2).projection(), Vector(1, 2, 1))
    assert close(Vector(1, 1, 0).project_on_to(Vector(5, 0, 0)), Vector(1, 0, 0))
    assert close(Vector(1, -1, 0).mirror_by_vector(Vector(0, 1, 0)), Vector(1, 1, 0))
    assert close(Vector.X_AXIS.rotate_angle_axis(90, Vector.Z_AXIS), Vector.Y_AXIS)
    assert Vector(-3, 2, -1).get_abs() == Vector(3, 2, 1)
    assert Vector(-3, 2, -1).get_max() == 2 and Vector(-3, 2, -1).get_min() == -3
    assert Vector(-3, 2, -1).get_abs_max() == 3 and Vector(-3, 2, -1).get_abs_min() == 1
    assert a.component_min(Vector(0, 5, 1)) == Vector(0, 2, 1)
    assert a.component_max(Vector(0, 5, 1)) == Vector(1, 5, 3)
    assert Vector.lerp(a, b, 0.5) == Vector(2.5, 3.5, 4.5)
    assert a.lerp(b, 0.5) == Vector(2.5, 3.5, 4.5)
    assert lm.lerp(a, b, 0.5) == Vector(2.5, 3.5, 4.5)
    assert lm.lerp(0.0, 10.0, 0.25) == 2.5


def test_constants_follow_up_axis():
    lm.set_up_axis("y")
    assert Vector.UP == Vector(0, 1, 0)
    assert Vector.FORWARD == Vector(0, 0, 1)
    assert Vector.RIGHT == Vector(-1, 0, 0)
    lm.set_up_axis("z")
    assert lm.up_axis() == "z"
    assert Vector.UP == Vector(0, 0, 1)
    assert Vector.FORWARD == Vector(1, 0, 0)
    assert Vector.RIGHT == Vector(0, -1, 0)
    lm.set_up_axis("y")
    try:
        lm.set_up_axis("x")
        assert False
    except ValueError:
        pass

    def check():
        assert close(Vector.FORWARD ^ Vector.UP, Vector.RIGHT)
        assert Vector.DOWN == -Vector.UP and Vector.LEFT == -Vector.RIGHT and Vector.BACKWARD == -Vector.FORWARD
        u = Vector.UP
        u.x = 99  # constants are fresh copies
        assert Vector.UP != u
        v = Vector.FORWARD * 3 + Vector.RIGHT * 4 + Vector.UP * 50
        assert close(v.size_2d(), 5)
        assert close(Vector.RIGHT.heading_angle(), pymath.pi / 2)

    for_each_up_axis(check)


def test_rotator():
    r = Rotator(10, 20, 30)
    assert repr(r) == "Rotator(pitch=10.0, yaw=20.0, roll=30.0)"
    assert Rotator(yaw=90) == Rotator(0, 90, 0)
    assert Rotator(roll=5, pitch=1, yaw=2) == Rotator(1, 2, 5)
    assert Rotator((1, 2, 3)) == Rotator(1, 2, 3)
    p, y, rl = r
    assert (p, y, rl) == (10, 20, 30)
    assert pickle.loads(pickle.dumps(r)) == r

    def check():
        assert close(Rotator(0, 90, 0).vector(), Vector.RIGHT)
        assert close(Rotator(90, 0, 0).get_forward_vector(), Vector.UP)
        assert close(Rotator(0, 0, 90).get_up_vector(), Vector.RIGHT)
        rr = Rotator(12, -70, 33)
        q = rr.quaternion()
        assert Rotator(q).equals(rr, 1e-6)
        assert q.rotator().equals(rr, 1e-6)
        v = Vector(1, 2, 3)
        assert close(rr.unrotate_vector(rr.rotate_vector(v)), v)
        assert close(rr.get_inverse().rotate_vector(rr.rotate_vector(v)), v)
        assert lm.compose_rotators(Rotator(0, 30, 0), Rotator(0, 45, 0)).equals(Rotator(0, 75, 0), 1e-9)
        assert Rotator(0, 30, 0).combine(Rotator(0, 45, 0)).equals(Rotator(0, 75, 0), 1e-9)

    for_each_up_axis(check)

    n = Rotator(370, -190, 540)
    assert n.normalize() is None
    assert n.equals(Rotator(10, 170, 180), 1e-9)
    assert Rotator(-10, -90, 0).get_denormalized().equals(Rotator(350, 270, 0), 1e-9)
    assert Rotator(0, 360, 0).is_nearly_zero()
    assert Rotator(1, 2, 3) + Rotator(1, 1, 1) == Rotator(2, 3, 4)
    assert Rotator(1, 2, 3) * 2 == Rotator(2, 4, 6)


def test_quat():
    q = Quat(Vector.Z_AXIS, pymath.pi / 2)
    assert close(q.rotate_vector(Vector.X_AXIS), Vector.Y_AXIS)
    assert close(q * Vector.X_AXIS, Vector.Y_AXIS)
    assert close(q.unrotate_vector(Vector.Y_AXIS), Vector.X_AXIS)
    assert Quat.IDENTITY == Quat(0, 0, 0, 1)
    assert Quat() == Quat.IDENTITY
    assert repr(Quat()) == "Quat(x=0.0, y=0.0, z=0.0, w=1.0)"
    axis, angle = q.to_axis_and_angle()
    assert close(axis, Vector.Z_AXIS) and close(angle, pymath.pi / 2)
    assert close(q.get_angle(), pymath.pi / 2)
    assert (q * q.inverse()).is_identity(1e-12)
    assert q.equals(-q)
    assert close(Quat.slerp(Quat.IDENTITY, q, 0.5).get_angle(), pymath.pi / 4)
    assert close(Quat.IDENTITY.angular_distance(q), pymath.pi / 2)
    assert close(q.get_axis_x(), Vector.Y_AXIS)
    assert pickle.loads(pickle.dumps(q)) == q
    assert Quat((0, 0, 0, 1)) == Quat.IDENTITY
    fb = Quat.find_between_vectors(Vector(1, 0, 0), Vector(0, 0, 5))
    assert close(fb.rotate_vector(Vector(1, 0, 0)), Vector(0, 0, 1))

    def check():
        r = Rotator(20, 30, 40)
        rq = Quat(r)
        assert close(rq.get_forward_vector(), r.get_forward_vector())
        assert close(rq.get_right_vector(), r.get_right_vector())
        assert close(rq.get_up_vector(), r.get_up_vector())

    for_each_up_axis(check)


def test_transform():
    t = Transform(Rotator(0, 90, 0), Vector(10, 20, 30), Vector(2, 2, 2))
    assert t.rotation.equals(Rotator(0, 90, 0).quaternion())
    assert t.translation == Vector(10, 20, 30) and t.location == Vector(10, 20, 30)
    assert t.scale3d == Vector(2, 2, 2)
    assert Transform() == Transform.IDENTITY
    assert Transform(location=(1, 2, 3), rotation=(0, 90, 0), scale=(1, 1, 1)).equals(
        Transform(Rotator(0, 90, 0), Vector(1, 2, 3)))
    assert Transform((0, 0, 0, 1), (1, 2, 3)).translation == Vector(1, 2, 3)
    assert lm.make_transform((1, 2, 3), (0, 45, 0), (2, 2, 2)).equals(
        Transform(Rotator(0, 45, 0), Vector(1, 2, 3), Vector(2, 2, 2)))
    assert repr(Transform()).startswith("Transform(rotation=Quat(")

    # Fields are copies, as in UE's Python.
    t2 = Transform(t)
    t2.translation.x = 999
    assert t2.translation == Vector(10, 20, 30)
    t2.translation = Vector(1, 1, 1)
    assert t2.translation == Vector(1, 1, 1) and t.translation == Vector(10, 20, 30)
    t2.rotation = Rotator(0, 45, 0)
    assert t2.rotator().equals(Rotator(0, 45, 0), 1e-9)

    p = Vector(1, 2, 3)
    a = Transform(Rotator(10, 20, 30), Vector(5, 6, 7), Vector(1.5, 1.5, 1.5))
    b = Transform(Rotator(-40, 50, 60), Vector(-1, 2, -3), Vector(0.5, 0.5, 0.5))
    assert close((a * b).transform_position(p), b.transform_position(a.transform_position(p)))
    assert close(a.multiply(b).transform_position(p), (a * b).transform_position(p))
    assert close(a.inverse().transform_position(a.transform_position(p)), p)
    assert close(a.inverse_transform_position(a.transform_position(p)), p)
    assert close(a.inverse_transform_vector(a.transform_vector(p)), p)
    assert close(a.transform_position_no_scale(p), a.rotation.rotate_vector(p) + a.translation)
    assert (a.get_relative_transform(b) * b).equals(a, 1e-8)
    assert close(a.to_matrix().transform_position(p), a.transform_position(p))
    assert (a.to_matrix() * b.to_matrix()).equals((a * b).to_matrix(), 1e-8)
    assert Transform(a.to_matrix()).equals(a, 1e-8)
    mid = Transform.blend(Transform(), Transform(Rotator(0, 90, 0), Vector(10, 0, 0), Vector(3, 3, 3)), 0.5)
    assert close(mid.translation, Vector(5, 0, 0)) and close(mid.scale3d, Vector(2, 2, 2))
    assert mid.rotator().equals(Rotator(0, 45, 0), 1e-9)
    assert pickle.loads(pickle.dumps(a)) == a
    assert copy.deepcopy(a) == a


def test_matrix():
    m = Matrix(Vector(0, 1, 0), Vector(-1, 0, 0), Vector(0, 0, 1), Vector(10, 0, 0))
    assert close(m.transform_position(Vector(1, 0, 0)), Vector(10, 1, 0))
    assert close(m.transform_vector(Vector(1, 0, 0)), Vector(0, 1, 0))
    assert m.get_origin() == Vector(10, 0, 0)
    assert m[3, 0] == 10.0 and m[0, 1] == 1.0
    assert m[3] == Vector4(10, 0, 0, 1)
    assert (m * m.inverse()).equals(Matrix.IDENTITY, 1e-12)
    assert m.to_quat().equals(Quat(Vector.Z_AXIS, pymath.pi / 2), 1e-12)
    assert Matrix(m.to_rows()) == m
    assert pickle.loads(pickle.dumps(m)) == m
    s = Matrix.make_scale(Vector(2, 2, 2))
    t = Matrix.make_translation(Vector(1, 2, 3))
    assert close((s * t).transform_position(Vector(1, 1, 1)), Vector(3, 4, 5))
    assert close(m.get_scaled_axis(lm.Axis.X), Vector(0, 1, 0))
    m2 = Matrix(m)
    m2[3, 0] = 5
    assert m2.get_origin() == Vector(5, 0, 0) and m.get_origin() == Vector(10, 0, 0)
    assert repr(Matrix()).startswith("Matrix([[1.0, 0.0, 0.0, 0.0]")


def test_unreal_conversion():
    lm.set_up_axis("y")
    assert close(lm.to_unreal(Vector(1, 2, 3)), Vector(3, -1, 2))
    assert close(lm.to_unreal((1, 2, 3)), Vector(3, -1, 2))
    assert close(lm.scale_to_unreal(Vector(1, 2, 3)), Vector(3, 1, 2))

    def check():
        assert close(lm.to_unreal(Vector.UP), Vector(0, 0, 1), EPS)
        assert close(lm.to_unreal(Vector.FORWARD), Vector(1, 0, 0), EPS)
        assert close(lm.to_unreal(Vector.RIGHT), Vector(0, 1, 0), EPS)
        p = Vector(12, -7, 3)
        assert close(lm.from_unreal(lm.to_unreal(p)), p, EPS)
        r = Rotator(15, 60, -20)
        assert lm.to_unreal(r) == r
        q = r.quaternion()
        uq = lm.to_unreal(q)
        assert close(uq.rotate_vector(lm.to_unreal(p)), lm.to_unreal(q.rotate_vector(p)))
        assert lm.from_unreal(uq).equals(q, 1e-12)
        # Unreal forward for a rotator: (cos p cos y, cos p sin y, sin p).
        cp, sp = pymath.cos(pymath.radians(15)), pymath.sin(pymath.radians(15))
        cy, sy = pymath.cos(pymath.radians(60)), pymath.sin(pymath.radians(60))
        assert close(uq.rotate_vector(Vector(1, 0, 0)), Vector(cp * cy, cp * sy, sp))
        t = Transform(r, Vector(1, 2, 3), Vector(1, 2, 3))
        ut = lm.to_unreal(t)
        assert close(ut.transform_position(lm.to_unreal(p)), lm.to_unreal(t.transform_position(p)))
        assert lm.from_unreal(ut).equals(t, 1e-9)
        m = t.to_matrix()
        assert close(lm.to_unreal(m).transform_position(lm.to_unreal(p)), lm.to_unreal(m.transform_position(p)))
        b = Box(Vector(-1, -2, -3), Vector(4, 5, 6))
        assert close(lm.to_unreal(b).get_volume(), b.get_volume())

    for_each_up_axis(check)


def test_kismet():
    def check():
        look = lm.find_look_at_rotation(Vector(1, 1, 1), Vector(1, 1, 1) + (Vector.FORWARD + Vector.RIGHT) * 5)
        assert look.equals(Rotator(0, 45, 0), 1e-9)
        r = Rotator(20, -35, 50)
        f, rt, u = r.get_forward_vector(), r.get_right_vector(), r.get_up_vector()
        assert lm.make_rot_from_xz(f, u).equals(r, 1e-6)
        assert lm.make_rot_from_xy(f, rt).equals(r, 1e-6)
        assert lm.make_rot_from_zx(u, f).equals(r, 1e-6)
        assert lm.make_rotation_from_axes(f, rt, u).equals(r, 1e-6)
        assert close(lm.make_rot_from_x(f).vector(), f)
        assert close(lm.get_forward_vector(r), f)

    for_each_up_axis(check)

    assert close(lm.vinterp_to(Vector(), Vector(10, 0, 0), 0.1, 5), Vector(5, 0, 0))
    assert close(lm.vinterp_to_constant(Vector(), Vector(10, 0, 0), 0.5, 4), Vector(2, 0, 0))
    assert lm.rinterp_to(Rotator(0, 170, 0), Rotator(0, -170, 0), 0.1, 5).equals(Rotator(0, 180, 0), 1e-9)
    assert lm.rinterp_to_constant(Rotator(), Rotator(0, 90, 0), 0.5, 20).equals(Rotator(0, 10, 0), 1e-9)
    assert close(lm.finterp_to(0, 10, 0.1, 5), 5)
    assert close(lm.finterp_to_constant(0, 10, 0.5, 4), 2)
    assert lm.rlerp(Rotator(), Rotator(0, 270, 0), 0.5, True).equals(Rotator(0, -45, 0), 1e-9)
    assert lm.rlerp(Rotator(), Rotator(0, 270, 0), 0.5).equals(Rotator(0, 135, 0), 1e-9)
    assert lm.clamp(5, 0, 3) == 3
    assert close(lm.map_range_clamped(15, 0, 10, 0, 100), 100)
    assert close(lm.map_range_unclamped(15, 0, 10, 0, 100), 150)
    assert lm.nearly_equal(1.0, 1.0 + 1e-7) and not lm.nearly_equal(1.0, 1.1)
    assert close(lm.degrees(pymath.pi), 180) and close(lm.radians(180), pymath.pi)
    assert close(lm.normalize_axis(270), -90) and close(lm.clamp_axis(-90), 270)
    assert close(lm.clamp_angle(100, -45, 45), 45)
    assert close(lm.dsin(90), 1) and close(lm.datan2(1, 1), 45)
    yaw, pitch = lm.get_yaw_pitch_from_vector(Vector.RIGHT)
    assert close(yaw, 90) and close(pitch, 0)


def test_colors():
    c = LinearColor(1, 0.5, 0)
    assert c.a == 1.0
    assert repr(c) == "LinearColor(r=1.0, g=0.5, b=0.0, a=1.0)"
    assert LinearColor((1, 0, 0)) == LinearColor.RED
    assert LinearColor((1, 0, 0, 0.5)).a == 0.5
    col = Color(255, 128, 0)
    assert col.a == 255 and repr(col) == "Color(r=255, g=128, b=0, a=255)"
    assert col.to_hex() == "FF8000FF"
    assert Color.from_hex("#ff8000") == col
    try:
        Color.from_hex("nope")
        assert False
    except ValueError:
        pass
    assert LinearColor(col).to_color() == col
    assert col.to_linear().to_color(True) == col
    assert LinearColor(0.5, 0.5, 0.5).to_color(False) == Color(128, 128, 128)
    assert Color((1, 2, 3)) == Color(1, 2, 3, 255)
    assert lm.lerp(LinearColor.BLACK, LinearColor.WHITE, 0.5).equals(LinearColor(0.5, 0.5, 0.5))
    # A Color is accepted where a LinearColor is expected (sRGB decode).
    assert lm.lerp(Color.WHITE, LinearColor.WHITE, 0.0).equals(LinearColor.WHITE)
    hsv = LinearColor(0.2, 0.4, 0.8).linear_rgb_to_hsv()
    assert close(hsv.r, 220, 1e-3)
    assert hsv.hsv_to_linear_rgb().equals(LinearColor(0.2, 0.4, 0.8), 1e-5)
    assert pickle.loads(pickle.dumps(col)) == col
    try:
        Color(256, 0, 0)
        assert False
    except TypeError:
        pass


def test_box_and_plane():
    b = Box()
    assert not b.is_valid
    b += Vector(1, 2, 3)
    b += (-1, 0, 5)
    assert b.is_valid and b.min == Vector(-1, 0, 3) and b.max == Vector(1, 2, 5)
    assert b.get_center() == Vector(0, 1, 4) and b.get_extent() == Vector(1, 1, 1)
    assert close(b.get_volume(), 8)
    assert b.is_inside(Vector(0, 1, 4)) and not b.is_inside(Vector(1, 1, 4)) and b.is_inside_or_on(Vector(1, 1, 4))
    assert Box([(0, 0, 0), (1, 2, 3)]) == Box(Vector(0, 0, 0), Vector(1, 2, 3))
    assert b.intersect(Box((1, 1, 1), (3, 3, 3)))
    assert not b.overlap(Box((5, 5, 5), (6, 6, 6))).is_valid
    assert b.transform_by(Transform(translation=(10, 0, 0))).equals(b.shift_by(Vector(10, 0, 0)))
    assert len(b.get_vertices()) == 8
    center, extent = b.get_center_and_extents()
    assert center == Vector(0, 1, 4)
    assert pickle.loads(pickle.dumps(b)) == b
    assert repr(Box()) == "Box(min=Vector(x=0.0, y=0.0, z=0.0), max=Vector(x=0.0, y=0.0, z=0.0), is_valid=False)"

    p = Plane(Vector(0, 5, 0), Vector(0, 2, 0))
    assert p.get_normal() == Vector(0, 1, 0) and close(p.w, 5)
    assert close(p.plane_dot(Vector(3, 7, 1)), 2)
    assert close(Vector(3, 7, 1).mirror_by_plane(p), Vector(3, 3, 1))
    assert close(lm.line_plane_intersection(Vector(), Vector(0, 10, 0), p), Vector(0, 5, 0))
    assert Plane(Vector(), Vector(1, 0, 0), Vector(0, 1, 0)).get_normal().equals(Vector(0, 0, 1))
    assert pickle.loads(pickle.dumps(p)) == p


def test_small_types():
    v2 = Vector2(3, 4)
    assert close(v2.size(), 5) and repr(v2) == "Vector2(x=3.0, y=4.0)"
    assert close(Vector2(1, 0).get_rotated(90), Vector2(0, 1))
    assert (Vector2(1, 0) ^ Vector2(0, 1)) == 1.0
    assert Vector2((1, 2)) == Vector2(1, 2)
    v4 = Vector4(Vector(1, 2, 3))
    assert v4.w == 1.0 and v4.xyz() == Vector(1, 2, 3)
    assert IntPoint(1, 2) + IntPoint(3, 4) == IntPoint(4, 6)
    assert IntVector(Vector(1.9, -1.9, 0.5)) == IntVector(1, -1, 0)
    assert repr(IntVector(1, 2, 3)) == "IntVector(x=1, y=2, z=3)"


def test_numpy():
    if not lm.has_numpy():
        print("numpy missing; skipping numpy tests")
        return
    import numpy as np

    vs = [Vector(1, 2, 3), Vector(4, 5, 6)]
    arr = lm.vectors_to_array(vs)
    assert isinstance(arr, np.ndarray) and arr.shape == (2, 3) and arr.dtype == np.float64
    assert arr.tolist() == [[1, 2, 3], [4, 5, 6]]
    assert lm.array_to_vectors(arr) == vs
    assert lm.array_to_vectors(np.array([[1, 2], [3, 4]], dtype=np.float32)) == [Vector2(1, 2), Vector2(3, 4)]
    # Strided input reads correctly.
    big = np.arange(18, dtype=np.float64).reshape(3, 6)
    assert lm.array_to_vectors(big[:, ::2]) == [Vector(0, 2, 4), Vector(6, 8, 10), Vector(12, 14, 16)]
    assert Vector(np.array([1.0, 2.0, 3.0])) == Vector(1, 2, 3)
    assert Vector(*np.array([1.0, 2.0, 3.0])) == Vector(1, 2, 3)

    t = Transform(Rotator(10, 20, 30), Vector(1, 2, 3), Vector(2, 2, 2))
    pts = np.random.default_rng(0).uniform(-10, 10, (100, 3))
    out = t.transform_positions(pts)
    for i in range(100):
        assert close(Vector(out[i]), t.transform_position(Vector(pts[i])))
    inplace = pts.copy()
    t.transform_positions_in_place(inplace)
    assert np.allclose(inplace, out)
    try:
        t.transform_positions_in_place(pts.astype(np.float32))
        assert False, "float32 must not silently copy"
    except TypeError:
        pass
    assert np.allclose(t.to_matrix().transform_positions(pts), out)
    assert np.allclose(lm.from_unreal_array(lm.to_unreal_array(pts)), pts)


def test_array_fallback_without_numpy():
    assert lm.array_to_vectors([(1, 2, 3), (4, 5, 6)]) == [Vector(1, 2, 3), Vector(4, 5, 6)]


def main():
    tests = [(k, v) for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for name, fn in tests:
        fn()
        print("ok  " + name)
    print("lila.math: %d tests passed" % len(tests))


main()
