"""Unreal-flavoured vector math.

    from lila.math import Vector, Rotator, Transform
    v = Vector(1, 2, 3)
    t = Transform(Rotator(yaw=90), Vector(0, 0, 100))
    t.transform_position(v)

Lila space is right-handed, cm, Y-up by default (set_up_axis("z") for Z-up scenes):

    up axis   FORWARD   RIGHT   UP
    y         +Z        -X      +Y
    z         +X        -Y      +Z

Rotator angles mean what they mean in Unreal on those axes: yaw turns forward toward right, pitch
turns forward toward up, roll turns up toward right. Quat, axis-angle and rotate_angle_axis are
plain right-handed math. Transform and Matrix compose like Unreal: a * b applies a, then b.

to_unreal / from_unreal convert vectors, quats, rotators, transforms, matrices and boxes to and
from Unreal space (left-handed Z-up, X forward, Y right): FORWARD -> +X, RIGHT -> +Y, UP -> +Z.
"""

from _lila.math import *  # noqa: F401,F403
from _lila.math import Rotator, Transform, Vector


def has_numpy():
    """True if numpy imports; the *_array helpers need it."""
    try:
        import numpy  # noqa: F401
    except ImportError:
        return False
    return True


def make_transform(location=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), scale=(1.0, 1.0, 1.0)):
    """UE Python argument order: location, rotation (Rotator or pitch/yaw/roll), scale."""
    return Transform(rotation=Rotator(*rotation) if isinstance(rotation, (tuple, list)) else rotation,
                     translation=Vector(location), scale3d=Vector(scale))
