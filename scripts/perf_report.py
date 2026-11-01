"""Summarise a Help > Record Performance Trace zip (or a bare .tracy) as text.

    python scripts/perf_report.py <lila-perf-*.zip | trace.tracy> [--top N] [--filter text]

Needs tracy-csvexport from vcpkg_installed (built with the tracy[cli-tools] feature).
"""

import argparse
import csv
import io
import json
import statistics
import subprocess
import sys
import tempfile
import zipfile
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CSVEXPORT = REPO / "vcpkg_installed" / "x64-windows" / "tools" / "tracy" / "tracy-csvexport.exe"


def export(trace, *args):
    out = subprocess.run([str(CSVEXPORT), *args, str(trace)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if out.returncode != 0:
        sys.exit(f"tracy-csvexport {' '.join(args)} failed: {out.stderr.strip() or out.stdout.strip()}")
    return list(csv.DictReader(io.StringIO(out.stdout)))


def ms(ns):
    return float(ns) / 1e6


def pct(values, p):
    values = sorted(values)
    if not values:
        return 0.0
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def table(rows, headers):
    widths = [max(len(str(h)), *(len(str(r[i])) for r in rows)) if rows else len(str(h)) for i, h in enumerate(headers)]
    line = lambda r: "  ".join(str(v).rjust(w) if i else str(v).ljust(w) for i, (v, w) in enumerate(zip(r, widths)))
    print(line(headers))
    for r in rows:
        print(line(r))


def section(title):
    print(f"\n== {title}")


def zone_stats(trace, top, name_filter, self_time):
    args = ["-e"] if self_time else []
    if name_filter:
        args += ["-f", name_filter]
    rows = export(trace, *args)
    rows.sort(key=lambda r: float(r["total_ns"]), reverse=True)
    out = []
    for r in rows[:top]:
        out.append([r["name"][:60], f"{Path(r['src_file']).name}:{r['src_line']}", r["counts"], f"{ms(r['total_ns']):.1f}",
                    f"{ms(r['mean_ns']):.3f}", f"{ms(r['max_ns']):.2f}"])
    table(out, ["zone", "where", "count", "total ms", "mean ms", "max ms"])


def frame_times(trace):
    rows = [r for r in export(trace, "-u", "-f", "Renderer::Impl::render", "-c") if r["name"].endswith("Renderer::Impl::render")]
    if not rows:
        print("no viewport frames")
        return
    record = [ms(r["exec_time_ns"]) for r in rows]
    starts = sorted(int(r["ns_since_start"]) for r in rows)
    gaps = [ms(b - a) for a, b in zip(starts, starts[1:])]
    print(f"{len(rows)} viewport frames drawn")
    print(f"render thread per frame ms: median {statistics.median(record):.2f}  p95 {pct(record, 95):.2f}  max {max(record):.2f}")
    if gaps:
        print(f"gap between frames ms:      median {statistics.median(gaps):.2f}  p95 {pct(gaps, 95):.2f}  max {max(gaps):.2f}")


def gpu_zones(trace):
    rows = export(trace, "-g")
    by = defaultdict(list)
    for r in rows:
        dur = r.get("GPU execution time")
        if dur:
            by[r["name"]].append(ms(dur))
    if not by:
        print("no GPU zones (the GPU has no calibrated timestamps, or nothing drew)")
        return
    out = [[n, len(v), f"{statistics.median(v):.3f}", f"{pct(v, 95):.3f}", f"{max(v):.3f}"] for n, v in by.items()]
    out.sort(key=lambda r: float(r[2]), reverse=True)
    table(out, ["GPU zone", "count", "median ms", "p95 ms", "max ms"])


def plots(trace):
    by = defaultdict(list)
    for r in export(trace, "-u", "-p"):
        if not r["src_file"] and r["value"]:
            by[r["name"]].append(float(r["value"]))
    if not by:
        print("no plots")
        return
    out = [[n, len(v), f"{min(v):.2f}", f"{statistics.median(v):.2f}", f"{pct(v, 95):.2f}", f"{max(v):.2f}"] for n, v in sorted(by.items())]
    table(out, ["plot", "samples", "min", "median", "p95", "max"])


def messages(trace, limit=60):
    rows = export(trace, "-m")
    if not rows:
        print("no messages")
        return
    counts = defaultdict(int)
    for r in rows:
        counts[r.get("MessageName", "")] += 1
    for text, n in sorted(counts.items(), key=lambda kv: -kv[1])[:limit]:
        print(f"{n:6}  {text}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--filter", default="")
    a = ap.parse_args()
    if not CSVEXPORT.exists():
        sys.exit(f"missing {CSVEXPORT}")

    path = Path(a.path)
    with tempfile.TemporaryDirectory() as tmp:
        trace = path
        if path.suffix.lower() == ".zip":
            with zipfile.ZipFile(path) as z:
                names = z.namelist()
                if "info.json" in names:
                    section("info.json")
                    print(json.dumps(json.loads(z.read("info.json")), indent=1))
                if "session.txt" in names:
                    section("session.txt (head)")
                    print("\n".join(z.read("session.txt").decode("utf-8", "replace").splitlines()[:40]))
                z.extract("trace.tracy", tmp)
                trace = Path(tmp) / "trace.tracy"

        section("Viewport frames")
        frame_times(trace)
        section("GPU passes")
        gpu_zones(trace)
        section("Plots")
        plots(trace)
        section(f"Top {a.top} zones by self time")
        zone_stats(trace, a.top, a.filter, True)
        section(f"Top {a.top} zones by total time")
        zone_stats(trace, a.top, a.filter, False)
        section("Commands run (messages)")
        messages(trace)


if __name__ == "__main__":
    main()
