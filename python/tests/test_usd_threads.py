"""lila.usd: stages composed on worker threads while a script holds the GIL.

Opening a stage composes its prims on TBB threads, and the first reference to a .mtlx file loads the MaterialX file
format plugin there. With Python running, USD imports a newly loaded library's bindings, which takes the GIL; the
script calling usdStageOpen holds it. This must finish, in a fresh process so the plugin has not loaded yet.

Run with: lilabatch.exe --batch X:/Lila/python/tests/test_usd_threads.py
"""

import os
import shutil
import sys
import tempfile
import time

from lila import ops, usd

DIR = os.path.join(tempfile.gettempdir(), "lila_py_usd_threads")
COUNT = 48

MTLX = """<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="SR_{i}" type="surfaceshader">
    <input name="base_color" type="color3" value="{r}, 0.2, 0.2" />
  </standard_surface>
  <surfacematerial name="M_{i}" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_{i}" />
  </surfacematerial>
</materialx>
"""


def write_stage(name, count):
    """A root prim with count children, each referencing its own .mtlx and its own small .usda."""
    lines = ['#usda 1.0', '(', '    defaultPrim = "Root"', '    metersPerUnit = 0.01', '    upAxis = "Z"', ')', '',
             'def Xform "Root"', '{']
    for i in range(count):
        with open(os.path.join(DIR, f"{name}_m{i}.mtlx"), "w", encoding="utf-8") as f:
            f.write(MTLX.format(i=i, r=i / count))
        with open(os.path.join(DIR, f"{name}_p{i}.usda"), "w", encoding="utf-8") as f:
            f.write('#usda 1.0\n(\n    defaultPrim = "P"\n)\n\ndef Xform "P"\n{\n'
                    f'    double3 xformOp:translate = ({i}, 0, 0)\n'
                    '    uniform token[] xformOpOrder = ["xformOp:translate"]\n}\n')
        lines += [f'    def Xform "Item{i}" (', f'        prepend references = @./{name}_p{i}.usda@', '    )', '    {',
                  f'        def Scope "Look" (', f'            prepend references = @./{name}_m{i}.mtlx@</MaterialX>', '        )',
                  '        {', '        }', '    }']
    lines.append('}')
    path = os.path.join(DIR, name + ".usda")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return path


def main():
    shutil.rmtree(DIR, ignore_errors=True)
    os.makedirs(DIR)
    lila_path = write_stage("lila", COUNT)
    pxr_path = write_stage("pxr", COUNT)
    assert "pxr.UsdMtlx" not in sys.modules

    ops.newScene()
    t = time.time()
    name = ops.usdStageOpen(lila_path)
    print("usdStageOpen took %.1fs" % (time.time() - t))
    # Loading OpenUSD imported every binding up front on this thread, so no worker has one left to import.
    pxr_dir = os.path.join(usd.package_root(), "python", "pxr")
    missing = [d for d in os.listdir(pxr_dir) if os.path.isfile(os.path.join(pxr_dir, d, "__init__.py")) and "pxr." + d not in sys.modules]
    assert not missing, missing
    stage = usd.stage(name)
    assert stage.GetPrimAtPath(f"/Root/Item{COUNT - 1}").IsValid()
    assert stage.GetPrimAtPath("/Root/Item0/Look").GetChildren(), "the .mtlx reference composed nothing"
    print("ok  usdStageOpen composing .mtlx references on worker threads")

    # The same through pxr straight from the script, which holds the GIL throughout.
    from pxr import Usd

    s = Usd.Stage.Open(pxr_path)
    assert len(s.GetPrimAtPath("/Root").GetChildren()) == COUNT
    print("ok  Usd.Stage.Open composing on worker threads")

    ops.newScene()
    shutil.rmtree(DIR, ignore_errors=True)
    print("lila.usd threads: 2 tests passed")


main()
