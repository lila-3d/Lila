"""Generates the built-in Painterly stroke sheets in resources/strokes.

Each sheet is a tileable RGBA8 PNG, linear: RG the stroke normal tilt (0.5 flat), B the stroke shade (0.5 neutral) and
A a random value per stroke. Strokes are painted procedurally onto a wrapping canvas, so the sheets are original.

    python tools/strokes/make_stroke_sheets.py [--size 1024]
"""

import argparse
import math
import os

import numpy as np
from PIL import Image

SHEETS = {
    # name: (stroke count per 1024^2, width range px, length range px, bristle depth, bristle frequency, ridge, curve)
    "strokes_soft": (560, (46, 92), (150, 320), 0.10, 0.25, 0.6, 0.6),
    "strokes_bristle": (620, (34, 70), (170, 360), 0.6, 0.45, 0.35, 0.4),
    "strokes_dabs": (1700, (30, 60), (40, 90), 0.2, 0.3, 0.8, 0.0),
}


def flow_angle(x, y, size, rng_params):
    """A smooth, tileable direction field so neighbouring strokes roughly agree."""
    a = 0.0
    for kx, ky, amp, phase in rng_params:
        a += amp * math.sin(2 * math.pi * (kx * x + ky * y) / size + phase)
    return a


def paint_sheet(size, count, widths, lengths, bristle, freq, ridge, curve, seed):
    rng = np.random.default_rng(seed)
    height = np.zeros((size, size), np.float32)
    shade = np.full((size, size), 0.5, np.float32)
    ident = np.full((size, size), 0.5, np.float32)
    field = [(int(rng.integers(1, 3)), int(rng.integers(-2, 3)), float(rng.uniform(0.3, 0.8)), float(rng.uniform(0, 6.283))) for _ in range(3)]
    base_angle = float(rng.uniform(0, math.pi))
    count = int(count * (size / 1024.0) ** 2)
    scale = size / 1024.0

    for _ in range(count):
        cx, cy = rng.uniform(0, size, 2)
        angle = base_angle + flow_angle(cx, cy, size, field) + rng.normal(0, 0.25)
        length = rng.uniform(*lengths) * scale
        width = rng.uniform(*widths) * scale
        bend = rng.normal(0, curve) / max(length, 1.0)
        stroke_shade = float(np.clip(rng.normal(0.5, 0.16), 0.0, 1.0))
        stroke_id = float(rng.uniform(0, 1))
        phase = rng.uniform(0, 100)
        lift = rng.uniform(0.6, 1.0)

        reach = int(length / 2 + width) + 2
        ys, xs = np.mgrid[-reach:reach + 1, -reach:reach + 1].astype(np.float32)
        c, s = math.cos(angle), math.sin(angle)
        u = xs * c + ys * s
        v = -xs * s + ys * c
        v = v - bend * u * u  # a gentle arc
        t = u / (length / 2)  # -1..1 along the stroke
        half = width / 2 * np.sqrt(np.clip(1 - np.clip(np.abs(t), 0, 1) ** 4, 0, 1))
        # Pressure: strokes start full and thin out toward the lift-off end.
        half *= np.clip(1.15 - 0.35 * (t + 1) / 2, 0, 1)
        inside = np.clip((half - np.abs(v)) / 1.5, 0, 1) * (np.abs(t) <= 1)
        if not inside.any():
            continue
        across = np.clip(np.abs(v) / np.maximum(half, 1e-3), 0, 1)
        profile = ridge * (1 - across ** 2)
        # Bristle grooves: smoothed noise across the stroke, wavering slightly along it.
        hairs = np.convolve(rng.uniform(0, 1, 256), np.ones(3) / 3, mode="same")
        coord = (v + 0.8 * np.sin(u * 0.03 + phase)) * freq * (1.0 / scale) + 128
        i0 = np.floor(coord).astype(np.int64)
        f = coord - i0
        bristles = hairs[i0 % 256] * (1 - f) + hairs[(i0 + 1) % 256] * f
        streak = 0.5 + 0.5 * np.sin(u * 0.02 + v * 0.9 + phase * 1.3)
        h = (profile + bristle * bristles * (0.6 + 0.4 * streak)) * lift
        # Paint piles up at the end where the brush lifts.
        h += 0.25 * np.clip(t, 0, 1) ** 3 * (1 - across)
        local_shade = stroke_shade + 0.12 * (bristles - 0.5) * bristle

        iy = (np.arange(-reach, reach + 1) + int(cy)) % size
        ix = (np.arange(-reach, reach + 1) + int(cx)) % size
        region = np.ix_(iy, ix)
        a = inside.astype(np.float32)
        height[region] = height[region] * (1 - a) + h * a
        shade[region] = shade[region] * (1 - a) + local_shade * a
        ident[region] = ident[region] * (1 - a) + stroke_id * a

    return height, shade, ident


def to_normals(height, strength):
    dx = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * 0.5
    dy = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * 0.5
    nx, ny = -dx * strength, -dy * strength
    nz = np.ones_like(nx)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    return nx / length, ny / length


def blur(img, passes):
    for _ in range(passes):
        img = (img + np.roll(img, 1, 0) + np.roll(img, -1, 0) + np.roll(img, 1, 1) + np.roll(img, -1, 1)) / 5.0
    return img


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "..", "resources", "strokes"))
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)

    for i, (name, (count, widths, lengths, bristle, freq, ridge, curve)) in enumerate(SHEETS.items()):
        height, shade, ident = paint_sheet(args.size, count, widths, lengths, bristle, freq, ridge, curve, seed=1234 + i)
        height = blur(height, 1)
        nx, ny = to_normals(height, strength=6.0 * args.size / 1024.0)
        rgba = np.stack([nx * 0.5 + 0.5, ny * 0.5 + 0.5, np.clip(shade, 0, 1), ident], axis=-1)
        Image.fromarray(np.clip(rgba * 255 + 0.5, 0, 255).astype(np.uint8), "RGBA").save(os.path.join(args.out, name + ".png"), optimize=True)
        print(name)


if __name__ == "__main__":
    main()
