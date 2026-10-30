"""Viewport shimmer check: renders a slow orbit of glossy primitives and measures how much each pixel flickers.

Run with lilabatch from the build folder:
    lilabatch --batch -c "exec(open(r'X:/Lila/scripts/viewport_crawl.py').read())"

Prints, per antialiasing setting, the pixels whose brightness jumps frame to frame by more than smooth motion explains
(the second difference over time). Lower is better; 0 is a clean turntable.
"""
import os
import tempfile

import numpy as np
from PySide6.QtGui import QImage

from lila import ops

FRAMES = 16
SIZE = 768


def load(path):
    img = QImage(path).convertToFormat(QImage.Format_Grayscale8)
    ptr = img.constBits()
    return np.frombuffer(ptr, np.uint8, img.sizeInBytes()).reshape(img.height(), img.bytesPerLine())[:, : img.width()].astype(np.float32)


def orbit(folder, tag, samples, grid):
    frames = []
    for i in range(FRAMES):
        path = os.path.join(folder, f"{tag}_{i:02d}.png")
        ops.captureView(path=path, view="persp", yaw=30 + i * 0.1, pitch=-20, width=SIZE, height=SIZE, display="shaded", grid=grid,
                        samples=samples)
        frames.append(load(path))
    f = np.stack(frames)
    f[:, :40, :260] = f[:1, :40, :260]  # the view label
    return np.abs(f[2:] - 2 * f[1:-1] + f[:-2]).mean(0)


def main():
    ops.newScene()
    ops.polyTorus(name="tor", position=[-60, 50, 0])
    ops.polySphere(name="sph", position=[60, 50, 0])
    ops.polyCube(name="cub", position=[0, 50, -60])
    ops.createNode("material", name="chrome")
    for o in ("tor", "sph"):
        ops.assignMaterial("chrome", o)
    ops.setAttr("chrome.metallic", 1.0)
    ops.setAttr("chrome.roughness", 0.15)
    folder = tempfile.mkdtemp(prefix="lila_crawl_")
    for tag, samples, grid in (("1 sample", 1, False), ("16 samples", 16, False), ("grid, 1 sample", 1, True), ("grid, 16 samples", 16, True)):
        crawl = orbit(folder, tag.replace(" ", "_").replace(",", ""), samples, grid)
        print(f"{tag:18s} shimmering pixels {int((crawl > 8).sum()):6d}   mean {crawl.mean():.3f}")


main()
