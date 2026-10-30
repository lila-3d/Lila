"""Turn the rendered passes into Steam's 256x256 achieved and unachieved PNGs, and write the definitions and
localization CSVs beside them. Needs numpy and Pillow:

    python compose.py [passes dir] [upload dir]

The folders default to lila-achievements/passes and lila-achievements/upload in the temp folder.
"""

import csv
import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageFilter, ImageOps

APP_ID = 5520720
N, SIZE = 256, 1024
CENTRE = np.array([62, 74, 92]) / 255
EDGE = np.array([24, 29, 38]) / 255
GLOW = np.array([61, 139, 255]) / 255
BORDER = np.array([200, 222, 250]) / 255


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255


def cutout(beauty, matte):
    """The matte pass is the backdrop scaled by (1 - alpha), so beauty - matte is the subject premultiplied."""
    lum = matte.mean(axis=2)
    bg = np.asarray(Image.fromarray((lum * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(31)), dtype=np.float32) / 255
    alpha = np.clip(1 - lum / np.maximum(bg, 0.05), 0, 1)
    alpha[alpha < 0.01] = 0
    return np.clip(beauty - matte, 0, 1), alpha


def fit(premul, alpha, fill=0.68):
    """Centre the subject's solid bounds and scale them to fill the frame."""
    ys, xs = np.nonzero(alpha > 0.5)
    cy, cx = (ys.min() + ys.max()) // 2, (xs.min() + xs.max()) // 2
    half = int(max(ys.max() - ys.min(), xs.max() - xs.min()) / fill / 2)

    def one(a):
        a = a if a.ndim == 3 else a[..., None]
        a = np.pad(a, ((half, half), (half, half), (0, 0)))[cy:cy + 2 * half, cx:cx + 2 * half]
        img = Image.fromarray((a * 255).astype(np.uint8).squeeze()).resize((SIZE, SIZE), Image.LANCZOS)
        a = np.asarray(img, dtype=np.float32) / 255
        return a if a.ndim == 3 else a[..., None]

    return one(premul), one(alpha)


def compose(premul, alpha, glow):
    yy, xx = np.mgrid[0:SIZE, 0:SIZE] / SIZE - 0.5
    r = np.sqrt(xx * xx + (yy + 0.1) ** 2)
    img = CENTRE + (EDGE - CENTRE) * np.clip(r / 0.75, 0, 1)[..., None]
    sh = Image.fromarray((alpha[..., 0] * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(SIZE * 0.06))
    img = img + GLOW * (np.asarray(sh, dtype=np.float32)[..., None] / 255) * glow
    img = premul + img * (1 - alpha)
    # Square frame: a pale outer line with a faint inner one, like the logo tile's rim.
    d = np.minimum(np.minimum(xx + 0.5, 0.5 - xx), np.minimum(yy + 0.5, 0.5 - yy))[..., None]
    img = np.where(d < 0.02, BORDER, img)
    img = np.where((d >= 0.035) & (d < 0.042), img + (BORDER - img) * 0.25, img)
    return Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).resize((N, N), Image.LANCZOS)


def main(passes, upload):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from table import ACHIEVEMENTS

    os.makedirs(upload, exist_ok=True)
    for api, *_ in ACHIEVEMENTS:
        premul, alpha = fit(*cutout(load(os.path.join(passes, f"{api}_beauty.png")),
                                    load(os.path.join(passes, f"{api}_matte.png"))))
        compose(premul, alpha, 0.3).save(os.path.join(upload, f"{api}_ACHIEVED.png"))
        locked = ImageOps.grayscale(compose(premul, alpha, 0.0)).point(lambda v: int(v * 0.75))
        locked.convert("RGB").save(os.path.join(upload, f"{api}_UNACHIEVED.png"))

    with open(os.path.join(upload, f"{APP_ID}-achievements-definitions.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["api_name (required, unique; the name your game passes to the API)",
                    "setid (optional; the id of a set that already exists)",
                    "permission (one of Client, GameServer, OfficialGameServer)", "spoiler (true or false)",
                    "archived (true or false)",
                    "progress_stat_name (optional; the name of a stat that already exists, blank for no progress bar)",
                    "progress_stat_min (number, no lower than the stat's own min)",
                    "progress_stat_max (number, above min and no higher than the stat's own max)"])
        for api, _name, _desc, hidden in ACHIEVEMENTS:
            w.writerow([api, "", "Client", "true" if hidden else "false", "false", "", 0, 0])

    with open(os.path.join(upload, f"{APP_ID}-achievements-localization.csv"), "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["api_name", "field", "english"])
        for api, name, desc, _hidden in ACHIEVEMENTS:
            w.writerow([api, "name", name])
            w.writerow([api, "description", desc])


if __name__ == "__main__":
    root = os.path.join(tempfile.gettempdir(), "lila-achievements")
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "passes"),
         sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "upload"))
