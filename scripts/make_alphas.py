"""Writes Lila's built-in brush alphas into resources/alphas/<category>/ as grayscale PNGs.

White is where the brush acts and black where it doesn't, so every feature is white on black: grooves, pores and holes
are cut in with Ctrl.

Everything is drawn here from shapes and seeded noise, so the set carries no third-party licence. Run from the repo root:
    python scripts/make_alphas.py
"""
from pathlib import Path

import numpy as np
from PIL import Image

OUT = Path(__file__).resolve().parent.parent / "resources" / "alphas"
rng = np.random.default_rng(7)


def grid(n):
    y, x = np.mgrid[0:n, 0:n]
    u = (x + 0.5) / n * 2 - 1
    v = (y + 0.5) / n * 2 - 1
    return u, v


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def edge(d, soft=0.08):
    """1 inside (d < 0), 0 outside, a smooth rim at d = 0."""
    return smooth(0.5 - d / soft)


def disc(u, v, radius=0.92, soft=0.12):
    return edge(np.sqrt(u * u + v * v) - radius, soft)


def value_noise(u, v, cells, octaves=4, seed=0):
    r = np.random.default_rng(seed)
    total = np.zeros_like(u)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        c = cells * 2 ** o
        g = r.random((c + 2, c + 2))
        gx, gy = (u + 1) / 2 * c, (v + 1) / 2 * c
        x0, y0 = np.clip(gx.astype(int), 0, c), np.clip(gy.astype(int), 0, c)
        fx, fy = smooth(gx - x0), smooth(gy - y0)
        a = g[y0, x0] * (1 - fx) + g[y0, x0 + 1] * fx
        b = g[y0 + 1, x0] * (1 - fx) + g[y0 + 1, x0 + 1] * fx
        total += amp * (a * (1 - fy) + b * fy)
        norm += amp
        amp *= 0.5
    return total / norm


def worley(u, v, cells, seed=0, jitter=1.0):
    """F1 and F2 distances to scattered cell points, in cell units."""
    r = np.random.default_rng(seed)
    pts = r.random((cells + 2, cells + 2, 2)) * jitter + (1 - jitter) * 0.5
    gx, gy = (u + 1) / 2 * cells, (v + 1) / 2 * cells
    ix, iy = np.floor(gx).astype(int), np.floor(gy).astype(int)
    f1 = np.full(u.shape, 9.0)
    f2 = np.full(u.shape, 9.0)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cx, cy = ix + dx, iy + dy
            p = pts[np.clip(cy + 1, 0, cells + 1), np.clip(cx + 1, 0, cells + 1)]
            d = np.sqrt((cx + p[..., 0] - gx) ** 2 + (cy + p[..., 1] - gy) ** 2)
            f2 = np.where(d < f1, f1, np.minimum(f2, d))
            f1 = np.minimum(f1, d)
    return f1, f2


def scatter(u, v, count, rmin, rmax, seed, profile=lambda t: t, soft=0.3, box=0.85):
    """Round bumps at random places; profile shapes each bump from rim (0) to centre (1)."""
    r = np.random.default_rng(seed)
    out = np.zeros_like(u)
    for cx, cy, rad in r.uniform([-box, -box, rmin], [box, box, rmax], (count, 3)):
        d = np.sqrt((u - cx) ** 2 + (v - cy) ** 2) / rad
        out = np.maximum(out, profile(smooth((1 - d) / soft)))
    return out


def save(category, name, img):
    d = OUT / category
    d.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.clip(img, 0, 1) * 255).round().astype(np.uint8), "L").save(d / f"{name}.png", optimize=True)


def shapes():
    u, v = grid(256)
    r = np.sqrt(u * u + v * v)
    a = np.arctan2(v, u)
    save("shapes", "round hard", edge(r - 0.9, 0.03))
    save("shapes", "round soft", np.exp(-(r / 0.45) ** 2))
    save("shapes", "dome", np.sqrt(np.clip(1 - (r / 0.92) ** 2, 0, 1)))
    save("shapes", "square", edge(np.maximum(abs(u), abs(v)) - 0.9, 0.06))
    save("shapes", "square soft", smooth(1 - np.maximum(abs(u), abs(v)) / 0.95) ** 0.7)
    save("shapes", "diamond", edge(abs(u) + abs(v) - 0.9, 0.06))
    save("shapes", "triangle", edge(np.maximum(v * 0.5 - 0.45 + abs(u) * 0.866, -v - 0.45), 0.05))
    save("shapes", "cross", edge(np.minimum(np.maximum(abs(u) - 0.22, abs(v) - 0.9), np.maximum(abs(v) - 0.22, abs(u) - 0.9)), 0.05))
    save("shapes", "star", edge(r - (0.55 + 0.35 * np.cos(a * 5)), 0.08))
    save("shapes", "ring", edge(abs(r - 0.65) - 0.2, 0.08))
    save("shapes", "rings", (0.5 + 0.5 * np.cos(r * np.pi * 8)) * disc(u, v))
    save("shapes", "line", edge(abs(v) - 0.12, 0.08) * edge(abs(u) - 0.9, 0.1))
    save("shapes", "ledge", smooth((v + 0.1) / 0.3) * disc(u, v))


def skin():
    u, v = grid(512)
    f1, _ = worley(u, v, 40, seed=11)
    save("skin", "pores fine", smooth(1 - f1 / 0.35) * disc(u, v))
    f1, _ = worley(u, v, 18, seed=12)
    save("skin", "pores coarse", smooth(1 - f1 / 0.4) * disc(u, v))
    warp = value_noise(u, v, 4, 3, seed=13) - 0.5
    lines = np.abs(np.sin((v + warp * 0.25) * np.pi * 14))
    save("skin", "wrinkles", (1 - lines) ** 6 * disc(u, v))
    a = np.arctan2(v, u)
    rays = np.abs(np.sin(a * 6 + value_noise(u, v, 3, 2, seed=14) * 2))
    r = np.sqrt(u * u + v * v)
    save("skin", "crows feet", (1 - rays) ** 12 * smooth((r - 0.15) / 0.2) * disc(u, v))
    f1, f2 = worley(u, v, 7, seed=15)
    save("skin", "cells", smooth((f2 - f1) / 0.25) * disc(u, v))
    vein = np.zeros_like(u)
    for seed in range(5):
        w = value_noise(u, v, 3, 3, seed=40 + seed) - 0.5
        vein = np.maximum(vein, smooth(1 - np.abs(w) / 0.025))
    save("skin", "veins", vein * disc(u, v))


def organic():
    u, v = grid(512)
    f1, f2 = worley(u, v, 9, seed=21)
    save("organic", "cracks", smooth(1 - (f2 - f1) / 0.06) * disc(u, v))
    f1, f2 = worley(u, v, 6, seed=22)
    n = value_noise(u, v, 8, 3, seed=23)
    save("organic", "dry mud", np.clip(smooth((f2 - f1) / 0.18) * (0.75 + 0.25 * n), 0, 1) * disc(u, v))
    warp = value_noise(u, v, 3, 4, seed=24) - 0.5
    grain = 0.5 + 0.5 * np.sin((u + warp * 0.5) * np.pi * 18)
    save("organic", "wood grain", (0.3 + 0.7 * grain ** 3) * disc(u, v))
    streak = value_noise(u * 6, v * 0.4, 6, 4, seed=25)
    save("organic", "bark", smooth((streak - 0.35) / 0.4) * disc(u, v))
    f1, f2 = worley(u, v, 14, seed=26)
    save("organic", "leather", smooth(f1 / 0.6) ** 0.5 * (1 - smooth(1 - (f2 - f1) / 0.12)) * disc(u, v))
    # Reptile scales: hex-ish cells, each a rounded plate.
    f1, f2 = worley(u, v, 10, seed=27, jitter=0.35)
    save("organic", "reptile scales", np.sqrt(np.clip((f2 - f1) / 0.5, 0, 1)) * disc(u, v))
    r = np.sqrt(u * u + v * v)
    a = np.arctan2(v, u)
    save("organic", "fish scales", np.clip(1 - (np.mod(r * 6 + 0.5 * np.sin(a * 9), 1.0) * 1.4), 0, 1) * disc(u, v))
    save("organic", "sponge", smooth((worley(u, v, 12, seed=28)[0] - 0.25) / 0.3) * disc(u, v))


def stone():
    u, v = grid(512)
    save("stone", "noise", np.clip((value_noise(u, v, 8, seed=31) - 0.2) * 1.6, 0, 1) * disc(u, v))
    save("stone", "fine noise", np.clip((value_noise(u, v, 32, 3, seed=32) - 0.15) * 1.4, 0, 1) * disc(u, v))
    save("stone", "rock", np.clip((value_noise(u, v, 3, 6, seed=33) - 0.25) * 2.0, 0, 1) * disc(u, v))
    ridged = 1 - np.abs(value_noise(u, v, 4, 5, seed=34) * 2 - 1)
    save("stone", "ridged", ridged ** 2 * disc(u, v))
    save("stone", "gravel", scatter(u, v, 260, 0.03, 0.08, seed=35, profile=np.sqrt) * disc(u, v))
    save("stone", "pebbles", scatter(u, v, 45, 0.08, 0.2, seed=36, profile=np.sqrt) * disc(u, v))
    f1, f2 = worley(u, v, 5, seed=37)
    save("stone", "cobbles", np.sqrt(smooth((f2 - f1) / 0.35)) * disc(u, v))
    save("stone", "erosion", smooth((value_noise(u * 0.3, v * 3, 10, 4, seed=38) - 0.3) / 0.5) * disc(u, v))


def fabric():
    u, v = grid(512)
    k = 22
    over = np.sin(u * np.pi * k) * np.sin(v * np.pi * k)
    warp = 0.5 + 0.5 * np.cos(v * np.pi * k * 2)
    weft = 0.5 + 0.5 * np.cos(u * np.pi * k * 2)
    save("fabric", "plain weave", np.where(over > 0, warp, weft) * disc(u, v))
    diag = np.mod((u + v) * k * 0.5, 1.0)
    save("fabric", "twill", (0.25 + 0.75 * smooth(1 - np.abs(diag - 0.5) / 0.4)) * (0.8 + 0.2 * weft) * disc(u, v))
    cx = np.mod(u * k * 0.5, 1.0) - 0.5
    cy = np.mod(v * k * 0.5 + np.abs(cx), 1.0)
    save("fabric", "knit", smooth(1 - np.abs(cy - 0.5) / 0.45) * smooth(1 - np.abs(cx) / 0.55) * disc(u, v))
    coarse = np.where(np.sin(u * np.pi * 9) * np.sin(v * np.pi * 9) > 0, 0.5 + 0.5 * np.cos(v * np.pi * 18), 0.5 + 0.5 * np.cos(u * np.pi * 18))
    save("fabric", "burlap", np.clip(coarse * (0.7 + 0.5 * value_noise(u, v, 20, 2, seed=41)), 0, 1) * disc(u, v))
    dash = smooth(1 - np.abs(np.mod(u * 5, 1.0) - 0.5) / 0.32) * edge(np.abs(v) - 0.12, 0.1)
    save("fabric", "stitches", dash * edge(np.abs(u) - 0.92, 0.08))
    quilt = np.minimum(np.abs(np.mod((u + v) * 3, 1.0) - 0.5), np.abs(np.mod((u - v) * 3, 1.0) - 0.5))
    save("fabric", "quilting", smooth(quilt / 0.5) ** 0.6 * disc(u, v))
    folds = np.abs(np.sin((u + 0.3 * (value_noise(u, v, 2, 2, seed=42) - 0.5)) * np.pi * 4))
    save("fabric", "folds", folds ** 0.7 * disc(u, v))


def hard_surface():
    u, v = grid(256)
    r = np.sqrt(u * u + v * v)
    head = np.sqrt(np.clip(1 - (r / 0.75) ** 2, 0, 1))
    save("hard surface", "rivet", head)
    slot = edge(np.maximum(np.abs(v) - 0.07, np.abs(u) - 0.55), 0.04)
    save("hard surface", "screw", head * (1 - 0.8 * slot))
    save("hard surface", "phillips screw", head * (1 - 0.8 * np.maximum(slot, edge(np.maximum(np.abs(u) - 0.07, np.abs(v) - 0.55), 0.04))))
    hexd = np.maximum(np.abs(u) * 0.866 + np.abs(v) * 0.5, np.abs(v)) - 0.72
    save("hard surface", "hex bolt", edge(hexd, 0.04) * (0.75 + 0.25 * edge(r - 0.35, 0.05)))
    save("hard surface", "rivet row", np.max([np.sqrt(np.clip(1 - (((u - cx) ** 2 + v * v) / 0.12 ** 2), 0, 1)) for cx in np.linspace(-0.75, 0.75, 5)], axis=0))
    save("hard surface", "button", np.minimum(1.0, smooth((0.7 - r) / 0.12)) * (0.85 + 0.15 * edge(r - 0.45, 0.05)))
    save("hard surface", "panel", smooth((0.85 - np.maximum(np.abs(u), np.abs(v))) / 0.15))
    save("hard surface", "panel line", edge(np.abs(v) - 0.03, 0.03) * edge(np.abs(u) - 0.92, 0.06))
    vents = edge(np.abs(np.mod(v * 3.5, 1.0) - 0.5) - 0.22, 0.08) * edge(np.abs(u) - 0.75, 0.08) * edge(np.abs(v) - 0.85, 0.05)
    save("hard surface", "vent", vents)
    hx = np.mod(u * 6, 1.0) - 0.5
    hy = np.mod(v * 6, 1.0) - 0.5
    save("hard surface", "grate", edge(np.maximum(np.abs(hx), np.abs(hy)) - 0.32, 0.06) * disc(u, v, 0.9, 0.05))
    save("hard surface", "perforated", edge(np.sqrt(hx * hx + hy * hy) - 0.28, 0.06) * disc(u, v, 0.9, 0.05))
    # Hexagonal honeycomb walls.
    q = u * 7
    s = v * 7 / 0.866
    row = np.floor(s)
    qx = np.mod(q + 0.5 * np.mod(row, 2), 1.0) - 0.5
    sy = np.mod(s, 1.0) - 0.5
    save("hard surface", "honeycomb", (1 - edge(np.maximum(np.abs(qx), np.abs(sy) * 0.866 + np.abs(qx) * 0.5) - 0.42, 0.05)) * disc(u, v, 0.9, 0.05))
    a = np.mod(u * 5 + v * 5, 2.0)
    b = np.mod(u * 5 - v * 5, 2.0)
    tread = np.where((np.floor(u * 2.5 + 10) + np.floor(v * 2.5 + 10)) % 2 == 0, edge(np.abs(a - 1) - 0.18, 0.12) * edge(np.abs(np.mod(b, 2.0) - 1) - 0.7, 0.1),
                     edge(np.abs(b - 1) - 0.18, 0.12) * edge(np.abs(np.mod(a, 2.0) - 1) - 0.7, 0.1))
    save("hard surface", "tread plate", tread * disc(u, v, 0.9, 0.05))
    save("hard surface", "knurling", np.maximum(0.5 + 0.5 * np.cos((u + v) * np.pi * 7), 0.5 + 0.5 * np.cos((u - v) * np.pi * 7)) ** 3 * disc(u, v))
    save("hard surface", "stripes", (0.5 + 0.5 * np.cos(u * np.pi * 8)) * disc(u, v))


def damage():
    u, v = grid(512)
    lines = np.zeros_like(u)
    r = np.random.default_rng(51)
    for _ in range(26):
        x0, y0 = r.uniform(-0.8, 0.8, 2)
        ang = r.uniform(-0.5, 0.5) + 0.6
        length = r.uniform(0.2, 0.9)
        dx, dy = np.cos(ang), np.sin(ang)
        t = np.clip((u - x0) * dx + (v - y0) * dy, 0, length)
        d = np.sqrt((u - x0 - t * dx) ** 2 + (v - y0 - t * dy) ** 2)
        lines = np.maximum(lines, smooth(1 - d / r.uniform(0.004, 0.012)) * smooth(np.minimum(t, length - t) / 0.08))
    save("damage", "scratches", lines * disc(u, v))
    save("damage", "dents", scatter(u, v, 70, 0.06, 0.16, seed=52, profile=lambda t: t * t) * disc(u, v))
    save("damage", "hammered", scatter(u, v, 220, 0.04, 0.09, seed=53, profile=lambda t: t) * disc(u, v))
    chips = np.clip(value_noise(u, v, 6, 4, seed=54), 0, 1)
    save("damage", "chips", smooth((0.58 - chips) / 0.06) * disc(u, v))
    f1, f2 = worley(u, v, 4, seed=55)
    hole = edge(np.sqrt(u * u + v * v) - 0.18, 0.05)
    rays = 1 - smooth(1 - (f2 - f1) / 0.05)
    save("damage", "impact", np.clip(hole + 0.6 * (1 - rays) * smooth(1 - np.sqrt(u * u + v * v) / 0.9), 0, 1) * disc(u, v))
    f1, f2 = worley(u * 0.6, v * 0.6, 12, seed=56)
    save("damage", "fine cracks", smooth(1 - (f2 - f1) / 0.05) * smooth((value_noise(u, v, 4, 3, seed=57) - 0.35) / 0.15) * disc(u, v))


if __name__ == "__main__":
    for gen in (shapes, skin, organic, stone, fabric, hard_surface, damage):
        gen()
    files = sorted(OUT.rglob("*.png"))
    print(len(files), "alphas,", sum(f.stat().st_size for f in files) // 1024, "KB")
