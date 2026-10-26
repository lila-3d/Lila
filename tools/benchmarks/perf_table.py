"""Turn benchmark results into markdown tables for docs/performance.md.

Usage: python perf_table.py results.json > tables.md

Input: {"apps": [...], "tables": [{"title", "unit", "rows": [{"task", "values", "notes"?}]}]}.
Values are times (lower is better). null means not measured; a string such as
"crashed" is shown as text and always loses.
"""

import json
import math
import sys
from decimal import Decimal, ROUND_HALF_UP

GREEN = (0x22, 0xA4, 0x47)
YELLOW = (0xC9, 0xA4, 0x00)
RED = (0xE0, 0x45, 0x3A)
TIE = 1.05


def lerp(a, b, t):
    return "#" + "".join("%02x" % round(x + (y - x) * t) for x, y in zip(a, b))


def solid(c):
    return lerp(c, c, 0)


def span(colour, text, bold=False):
    weight = ";font-weight:700" if bold else ""
    return f'<span style="color:{colour}{weight}">{text}</span>'


LEGEND = (
    "Times are coloured by how far they are from the best on their row: "
    f"{span(solid(GREEN), 'green')} is the fastest (brighter for a bigger lead), "
    f"{span(solid(YELLOW), 'yellow')} is close to it and "
    f"{span(solid(RED), 'red')} is 10x slower or worse; results within 5% count as even."
)


def intensity(ratio):
    return min(max(math.log(ratio) / math.log(10), 0.0), 1.0)


def fmt(v):
    """Three significant figures."""
    places = max(0, 2 - math.floor(math.log10(abs(v)))) if v else 0
    return str(Decimal(str(v)).quantize(Decimal(1).scaleb(-places), ROUND_HALF_UP))


def fmt_factor(r):
    return f"{r:.1f}" if r < 10 else f"{r:.0f}"


def short(app):
    return app.split()[0]


APP_COLOURS = {}  # short app name -> (start, end) of its name's gradient, from the input's "appColors"


def app_name(app, full=True):
    """The app's name, in its own gradient when the input gives one, so each app reads apart from the timing colours."""
    text = app if full else short(app)
    colours = APP_COLOURS.get(short(app))
    if not colours:
        return text
    style = f"background:linear-gradient(90deg,{colours[0]},{colours[1]});-webkit-background-clip:text;background-clip:text;color:transparent;font-weight:700"
    return f'<span style="{style}">{text}</span>'


def render_row(apps, row, unit):
    values = row["values"]
    nums = sorted((v, i) for i, v in enumerate(values) if isinstance(v, (int, float)))
    failed = [i for i, v in enumerate(values) if isinstance(v, str)]
    cells = []

    best, best_i = nums[0] if nums else (None, None)
    runner_ratio = nums[1][0] / best if len(nums) > 1 else (math.inf if failed else None)
    tie = runner_ratio is not None and runner_ratio <= TIE

    for i, v in enumerate(values):
        if v is None:
            cells.append("-")
        elif isinstance(v, str):
            cells.append(span(solid(RED), v))
        elif runner_ratio is None:
            cells.append(f"{fmt(v)} {unit}")
        else:
            r = v / best
            bold = False
            if tie and r <= TIE:
                colour = solid(YELLOW)
            elif i == best_i:
                colour = lerp(YELLOW, GREEN, intensity(runner_ratio))
                bold = True
            else:
                colour = lerp(YELLOW, RED, intensity(r))
            cells.append(span(colour, f"{fmt(v)} {unit}", bold))

    if runner_ratio is None:
        diff = ""
    elif tie:
        diff = "≈ even"
    elif len(nums) > 1:
        diff = f"**{app_name(apps[best_i], False)} {fmt_factor(runner_ratio)}x faster**"
    else:
        diff = "**" + ", ".join(f"{app_name(apps[i], False)} {values[i]}" for i in failed) + "**"

    task = row["task"]
    if row.get("notes"):
        task += f"<br><small>{row['notes']}</small>"
    return "| " + " | ".join([task, *cells, diff]) + " |"


def render_table(apps, table):
    lines = [
        f"### {table['title']}",
        "",
        "| Task | " + " | ".join(app_name(a) for a in apps) + " | Difference |",
        "|" + "---|" * (len(apps) + 2),
    ]
    lines += [render_row(apps, row, table["unit"]) for row in table["rows"]]
    return "\n".join(lines)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: perf_table.py results.json")
    with open(sys.argv[1], encoding="utf-8") as f:
        data = json.load(f)
    apps = data["apps"]
    APP_COLOURS.update(data.get("appColors", {}))
    if len(apps) not in (2, 3):
        sys.exit("expected 2 or 3 apps")
    sys.stdout.reconfigure(encoding="utf-8")
    print(LEGEND + "\n")
    print("\n\n".join(render_table(apps, t) for t in data["tables"]))


if __name__ == "__main__":
    main()
