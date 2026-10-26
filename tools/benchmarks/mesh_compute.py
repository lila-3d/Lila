"""The mesh compute benchmark's maths, shared by every app's benchmark script so they all time the same work.

Plain numpy and the math module only: nothing app-specific. Each vertex gets a 4x4 matrix built from its position
(rotate about Y by atan2(z, x), about X by asin of the normalised y, scale by its length, translate by the position),
that matrix transforms the vertex, the result is normalised, its angle to a fixed axis goes through some sin, cos and
tan, and the weighted directions are reduced to a mean and a 3x3 covariance.

    mean, cov = compute(points)                      # points: (N, 3) float64
    mean, cov = compute_threaded(points, pool, workers)
    n, s, ss = python_partial(points[:100_000])      # the same maths per vertex in pure Python
"""

import math

import numpy as np

AXIS = np.array([1.0, 2.0, 3.0]) / math.sqrt(14.0)
CHUNK = 1 << 16


def _matrices(p):
    """(n, 4, 4) per-vertex matrices T(p) * Ry(yaw) * Rx(pitch) * S(|p|)."""
    x, y, z = p[:, 0], p[:, 1], p[:, 2]
    length = np.sqrt(x * x + y * y + z * z)
    safe = np.where(length > 0.0, length, 1.0)
    yaw = np.arctan2(z, x)
    pitch = np.arcsin(np.clip(y / safe, -1.0, 1.0))
    cy, sy = np.cos(yaw), np.sin(yaw)
    cp, sp = np.cos(pitch), np.sin(pitch)
    m = np.zeros((len(p), 4, 4))
    m[:, 0, 0] = cy * length
    m[:, 0, 1] = sy * sp * length
    m[:, 0, 2] = sy * cp * length
    m[:, 1, 1] = cp * length
    m[:, 1, 2] = -sp * length
    m[:, 2, 0] = -sy * length
    m[:, 2, 1] = cy * sp * length
    m[:, 2, 2] = cy * cp * length
    m[:, :3, 3] = p
    m[:, 3, 3] = 1.0
    return m


def partial(p):
    """(count, sum, sum of outer products) of the weighted directions of one chunk of points."""
    h = np.empty((len(p), 4))
    h[:, :3] = p
    h[:, 3] = 1.0
    q = np.einsum("nij,nj->ni", _matrices(p), h)[:, :3]
    norm = np.linalg.norm(q, axis=1)
    qn = q / np.where(norm > 0.0, norm, 1.0)[:, None]
    angle = np.arccos(np.clip(qn @ AXIS, -1.0, 1.0))
    mix = np.sin(angle) * np.cos(2.0 * angle) + np.tan(0.25 * angle)
    r = qn * mix[:, None]
    return len(r), r.sum(axis=0), r.T @ r


def combine(parts):
    """Mean and sample covariance from partial() results."""
    n = sum(c for c, _, _ in parts)
    s = np.sum([s for _, s, _ in parts], axis=0)
    ss = np.sum([ss for _, _, ss in parts], axis=0)
    mean = s / n
    cov = (ss - n * np.outer(mean, mean)) / (n - 1)
    return mean, cov


def _chunks(n, size):
    return [(lo, min(lo + size, n)) for lo in range(0, n, size)]


def compute(points):
    """Mean and 3x3 covariance of the weighted directions, on this thread, a chunk at a time."""
    p = np.asarray(points, dtype=np.float64)
    return combine([partial(p[lo:hi]) for lo, hi in _chunks(len(p), CHUNK)])


def compute_threaded(points, pool, workers):
    """compute() split across a concurrent.futures pool; numpy releases the GIL, so the chunks run in parallel."""
    p = np.asarray(points, dtype=np.float64)
    size = max(1024, min(CHUNK, -(-len(p) // (workers * 4))))
    return combine(list(pool.map(lambda r: partial(p[r[0]:r[1]]), _chunks(len(p), size))))


def python_partial(points):
    """partial() vertex by vertex with the math module, for the pure Python timing."""
    ax, ay, az = (float(v) for v in AXIS)
    n = 0
    s = [0.0, 0.0, 0.0]
    ss = [[0.0] * 3 for _ in range(3)]
    for x, y, z in points:
        length = math.sqrt(x * x + y * y + z * z)
        yaw = math.atan2(z, x)
        pitch = math.asin(max(-1.0, min(1.0, y / length))) if length > 0.0 else 0.0
        cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
        m = ((cy * length, sy * sp * length, sy * cp * length, x),
             (0.0, cp * length, -sp * length, y),
             (-sy * length, cy * sp * length, cy * cp * length, z))
        q = [row[0] * x + row[1] * y + row[2] * z + row[3] for row in m]
        norm = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]) or 1.0
        qn = [c / norm for c in q]
        angle = math.acos(max(-1.0, min(1.0, qn[0] * ax + qn[1] * ay + qn[2] * az)))
        mix = math.sin(angle) * math.cos(2.0 * angle) + math.tan(0.25 * angle)
        r = [c * mix for c in qn]
        n += 1
        for i in range(3):
            s[i] += r[i]
            for j in range(3):
                ss[i][j] += r[i] * r[j]
    return n, np.array(s), np.array(ss)


def checksum(mean, cov):
    """One line to compare across apps: the mean and the covariance's trace."""
    return f"mean {mean[0]:.9f} {mean[1]:.9f} {mean[2]:.9f}  cov trace {np.trace(cov):.9f}"
