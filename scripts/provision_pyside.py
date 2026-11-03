"""Installs PySide6 Essentials at Lila's Qt version, plus the Qt.py shim, keeping only the modules Lila's Qt DLLs back.

provision_pyside.py <python> <qt version> <out dir> <Qt module>...

The wheel's Qt DLLs, plugins, QML, tools and translations are removed: PySide's modules bind to the Qt that Lila has
already loaded, so the bundle is the bindings alone.
"""

import os
import shutil
import subprocess
import sys

# Studio tools written against Qt.py run unchanged on the bundled PySide6.
QT_PY_VERSION = "2.0.7"
KEEP_FILES = {"__init__.py", "_config.py", "_git_pyside_version.py", "py.typed", "pyside6.abi3.dll"}
KEEP_DIRS = {"support"}


def strip_pyside(package, modules):
    keep = set(KEEP_FILES)
    for module in modules:
        keep.update({f"Qt{module}.pyd", f"Qt{module}.pyi"})
    for name in os.listdir(package):
        path = os.path.join(package, name)
        if os.path.isdir(path):
            if name not in KEEP_DIRS:
                shutil.rmtree(path)
        elif name not in keep:
            os.remove(path)


def strip_shiboken(package):
    for name in os.listdir(package):
        path = os.path.join(package, name)
        if name in ("include", "lib", "__pycache__"):
            shutil.rmtree(path)
        elif name.endswith(".lib") or (name.endswith(".dll") and not name.startswith("shiboken6")):
            os.remove(path)


def main():
    python, version, out, modules = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    staging = out + ".tmp"
    shutil.rmtree(staging, ignore_errors=True)
    subprocess.run([python, "-m", "pip", "install", "--quiet", "--disable-pip-version-check", "--no-warn-script-location",
                    "--no-compile", "--no-deps", "--target", staging,
                    f"pyside6-essentials=={version}", f"shiboken6=={version}", f"Qt.py=={QT_PY_VERSION}"], check=True)
    # Console-script launchers (pyside6-uic and friends) need the tools stripped below.
    shutil.rmtree(os.path.join(staging, "bin"), ignore_errors=True)
    strip_pyside(os.path.join(staging, "PySide6"), modules)
    strip_shiboken(os.path.join(staging, "shiboken6"))
    shutil.rmtree(out, ignore_errors=True)
    os.replace(staging, out)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError:
        sys.exit("PySide6 could not be installed with pip. Configure with -DLILA_PYSIDE=OFF to build without it.")
