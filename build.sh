#!/usr/bin/env bash
# Configures if needed, then builds every target. Usage: ./build.sh [build dir] [config], defaults build RelWithDebInfo.
# Linux is experimental and untested: Qt comes from vcpkg and Python from scripts/fetch-python.sh, never from the distro.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-build}"
CONFIG="${2:-RelWithDebInfo}"

if [ -z "${VCPKG_ROOT:-}" ]; then
    echo "[failed] Set VCPKG_ROOT to a clone of github.com/microsoft/vcpkg (after running its bootstrap-vcpkg.sh)."
    exit 1
fi
if [ -z "${LILA_PYTHON_DIR:-}" ]; then
    LILA_PYTHON_DIR="$("$ROOT/scripts/fetch-python.sh")"
fi
export LILA_PYTHON_DIR
export PATH="$LILA_PYTHON_DIR/bin:$PATH"

if [ ! -f "$ROOT/$DIR/build.ninja" ]; then
    # The USD port only knows a Windows Python layout.
    cmake --preset linux -S "$ROOT" -B "$ROOT/$DIR" -DLILA_USD=OFF
fi
cmake --build "$ROOT/$DIR" --config "$CONFIG"
