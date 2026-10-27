"""Bake benchmark scene, run with lilabatch --batch: ~200 small bevelled boxes and cylinders over 8 materials, each
uvAuto'd into a random third of the sheet (so a texel sees ~10% of the meshes, as on a shared vehicle sheet), saved as
bake_bench.lila. Env: BENCH_DIR (folder, default the temp dir), BENCH_COUNT (meshes, default 200)."""
import os, random, tempfile, time

from lila import ops

DIR = os.environ.get("BENCH_DIR", tempfile.gettempdir())
COUNT = int(os.environ.get("BENCH_COUNT", "200"))

t0 = time.time()
ops.newScene()
rng = random.Random(7)
mats = [ops.createMaterial("M_Part%d" % i, baseColor=(rng.random(), rng.random(), rng.random())) for i in range(8)]
for i in range(COUNT):
    pos = (rng.uniform(-400, 400), rng.uniform(0, 200), rng.uniform(-400, 400))
    if i % 5 == 4:
        m = ops.polyCylinder(name="part%d" % i, position=pos, radius=rng.uniform(5, 20), height=rng.uniform(10, 60), generator=False)[0]
    else:
        m = ops.polyCube(name="part%d" % i, position=pos, width=rng.uniform(10, 80), height=rng.uniform(10, 60), depth=rng.uniform(10, 80),
                         subdivisionsX=rng.randint(1, 3), subdivisionsY=rng.randint(1, 3), subdivisionsZ=rng.randint(1, 3), generator=False)[0]
    ops.selectFoldEdges(m, minAngle=60.0)
    ops.polyBevel(offset=rng.uniform(0.5, 2.0), segments=2)
    ops.selectMode("object")
    ops.select(m)
    u0, v0 = rng.uniform(0, 0.7), rng.uniform(0, 0.7)
    ops.uvAuto(region=[u0, v0, u0 + 0.3, v0 + 0.3])
    ops.assignMaterial(mats[i % len(mats)], m)
ops.saveScene(os.path.join(DIR, "bake_bench.lila"))
print("generated %d meshes in %.1f s" % (COUNT, time.time() - t0))
