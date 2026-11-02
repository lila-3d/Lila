"""Nightly regression run: builds every target in build-night, runs the full test suite, and on Sundays (or with
--coverage) measures line coverage with OpenCppCoverage. Results go to build-night\\nightly\\latest.txt, one line
per run is appended to history.txt.

pythonw scripts\\nightly.py [--coverage] [--no-build]

Registered as the LilaNightly scheduled task by scripts\\nightly_task.ps1. Runs without a console window.
"""

import argparse
import datetime
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = "build-night"
CONFIG = "RelWithDebInfo"
# Unoptimized RelWithDebInfo (/Od): instrumenting optimized code corrupts it, and Debug needs a debug Python.
COVERAGE_BUILD_DIR = "build-coverage"
OUT = os.path.join(REPO, BUILD_DIR, "nightly")
COVERAGE_DIR = os.path.join(REPO, "build-coverage", "report")
OPENCPPCOVERAGE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "OpenCppCoverage", "OpenCppCoverage.exe")
NO_WINDOW = 0x08000000


def shell(command):
    """Runs a command line after scripts\\env.cmd (compiler, cmake, ctest on PATH) and returns (code, output)."""
    full = f'cmd /c "scripts\\env.cmd >nul 2>&1 && {command}"'
    process = subprocess.run(full, cwd=REPO, capture_output=True, text=True, errors="replace", creationflags=NO_WINDOW)
    return process.returncode, process.stdout + process.stderr


def build_night_busy():
    """True when another session is building or testing in build-night, which must not be shared concurrently."""
    query = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*build-night*' -and "
             "$_.Name -match '^(cmake|ninja|ctest|cl|lld-link|python|lila.*)\\.exe$' } | Select-Object -ExpandProperty ProcessId")
    process = subprocess.run(["powershell", "-NoProfile", "-Command", query], capture_output=True, text=True,
                             creationflags=NO_WINDOW)
    others = [pid for pid in process.stdout.split() if pid.strip() and int(pid) != os.getpid()]
    return bool(others)


def failed_tests(log_path):
    try:
        with open(log_path, encoding="utf-8", errors="replace") as log:
            return [line.strip() for line in log if re.match(r"\s*\d+ - .*\((Failed|Timeout|SEGFAULT|Exception|Not Run)", line)]
    except OSError:
        return []


def area_of(path):
    """...\\src\\geo\\Mesh.cpp -> src/geo, ...\\extensions\\Tailor\\Pattern.cpp -> extensions/Tailor.

    Matched anywhere in the path: sccache hits keep the path of whichever checkout or worktree compiled them.
    """
    path = path.replace("\\", "/")
    match = re.search(r"/(src|extensions)/([^/]+)/", path)
    return f"{match.group(1)}/{match.group(2)}" if match else "other"


def coverage_by_area(xml_path):
    totals = defaultdict(lambda: [0, 0])
    root = ET.parse(xml_path).getroot()
    source = root.findtext("sources/source", "")
    for cls in root.iter("class"):
        filename = "/" + os.path.join(source, cls.get("filename", ""))
        lines = cls.findall("lines/line")
        area = totals[area_of(filename)]
        area[0] += sum(1 for line in lines if int(line.get("hits", "0")) > 0)
        area[1] += len(lines)
    return totals


def run_coverage():
    os.makedirs(COVERAGE_DIR, exist_ok=True)
    xml_path = os.path.join(COVERAGE_DIR, "coverage.xml")
    code, output = shell(f"python scripts\\quiet.py build {COVERAGE_BUILD_DIR} {CONFIG}")
    if code != 0:
        return [f"coverage FAILED: {COVERAGE_BUILD_DIR} build failed", output.strip()]
    jobs = max(1, (os.cpu_count() or 2) // 2)
    command = (f'"{OPENCPPCOVERAGE}" --quiet --cover_children --continue_after_cpp_exception'
               r' --sources \src\ --sources \extensions\ --excluded_sources vcpkg --excluded_sources \_work\ '
               r' --excluded_sources \tests\ --excluded_sources "Windows Kits" --excluded_sources "Microsoft Visual Studio"'
               r' --excluded_sources minkernel --excluded_sources \_deps\ '
               f' --export_type html:{COVERAGE_DIR}\\html --export_type cobertura:{xml_path}'
               f' -- ctest --test-dir {COVERAGE_BUILD_DIR} -C {CONFIG} -j {jobs} --timeout 900 -LE known_failing')
    _, output = shell(command)
    with open(os.path.join(COVERAGE_DIR, "ctest.log"), "w", encoding="utf-8") as log:
        log.write(output)
    if not os.path.exists(xml_path):
        return ["coverage FAILED: no report written"]
    totals = coverage_by_area(xml_path)
    covered = sum(c for c, _ in totals.values())
    total = sum(t for _, t in totals.values())
    lines = [f"coverage {100 * covered / max(total, 1):.1f}% ({covered}/{total} lines), report {COVERAGE_DIR}\\html\\index.html"]
    # A link that couldn't replace a locked DLL leaves a PDB that no longer matches it, and the module drops out.
    if "src/geo" not in totals:
        lines.insert(1, "  WARNING: LilaScene missing, its PDB doesn't match the DLL; rebuild with nothing running from build-coverage")
    for area, (c, t) in sorted(totals.items(), key=lambda item: item[1][0] / max(item[1][1], 1)):
        lines.append(f"  {100 * c / max(t, 1):5.1f}%  {t:7d}  {area}")
    with open(os.path.join(COVERAGE_DIR, "summary.txt"), "w", encoding="utf-8") as summary:
        summary.write("\n".join(lines) + "\n")
    return lines[:1] + ["  lowest areas first:"] + lines[1:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--coverage", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    os.makedirs(OUT, exist_ok=True)
    now = datetime.datetime.now()
    _, head = shell("git rev-parse --short HEAD")
    _, dirty = shell("git status --porcelain")
    report = [f"Lila nightly {now:%Y-%m-%d %H:%M}, HEAD {head.strip()}, {len(dirty.splitlines())} uncommitted files"]

    if build_night_busy():
        report.append("skipped: another session is using build-night")
        status = "SKIPPED"
    else:
        build_code = 0
        if not args.no_build:
            build_code, output = shell(f"python scripts\\quiet.py build {BUILD_DIR} {CONFIG}")
            report.append(output.strip())
        if build_code != 0:
            status = "BUILD FAILED"
        else:
            test_code, output = shell(f"python scripts\\quiet.py test {BUILD_DIR} {CONFIG} -LE known_failing")
            report.append(output.strip().splitlines()[-1] if output.strip() else "no test output")
            report += failed_tests(os.path.join(REPO, BUILD_DIR, "last_test.log"))
            status = "OK" if test_code == 0 else "TESTS FAILED"
            if args.coverage or now.weekday() == 6:
                report += run_coverage() if os.path.exists(OPENCPPCOVERAGE) else [f"coverage skipped: {OPENCPPCOVERAGE} missing"]

    report.insert(1, f"status: {status}")
    with open(os.path.join(OUT, "latest.txt"), "w", encoding="utf-8") as latest:
        latest.write("\n".join(report) + "\n")
    with open(os.path.join(OUT, "history.txt"), "a", encoding="utf-8") as history:
        history.write(f"{now:%Y-%m-%d %H:%M}  {head.strip()}  {status}\n")
    return 0 if status in ("OK", "SKIPPED") else 1


if __name__ == "__main__":
    sys.exit(main())
