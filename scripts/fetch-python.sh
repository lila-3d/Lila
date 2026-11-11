#!/usr/bin/env bash
# Prints a repo-local Python 3.14 folder, downloading python-build-standalone's build (headers and libpython included)
# on first use, so the build never depends on the distro's Python.
set -euo pipefail
LILA_PY_MINOR=3.14
DEST="$(cd "$(dirname "$0")/.." && pwd)/.deps/python-$LILA_PY_MINOR-linux"
if [ -x "$DEST/bin/python3" ]; then
    echo "$DEST"
    exit 0
fi

ARCH="$(uname -m)"
URL="$(curl -sSfL https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest \
    | grep -oE "https://[^\"]*cpython-$LILA_PY_MINOR\.[0-9]+(\+|%2B)[0-9]+-$ARCH-unknown-linux-gnu-install_only\.tar\.gz" | head -n 1)" || true
if [ -z "$URL" ]; then
    echo "[failed] Could not find a Python $LILA_PY_MINOR download. Install Python $LILA_PY_MINOR with headers and set LILA_PYTHON_DIR to it instead." >&2
    exit 1
fi

echo "Downloading $URL" >&2
TMP="$DEST.tmp"
rm -rf "$TMP"
mkdir -p "$TMP"
curl -sSfL "$URL" | tar -xz -C "$TMP"
"$TMP/python/bin/python3" -m pip install --quiet --disable-pip-version-check --no-warn-script-location numpy >&2
mv "$TMP/python" "$DEST"
rm -rf "$TMP"
echo "$DEST"
