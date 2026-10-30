# Achievements

The models behind Lila's Steam achievement icons, one per achievement, built from primitives, booleans, pipes and text by script.

## Use it

1. Open `Achievements.lila`. Each model is a group named `<Name>Achievement` with its label below it.
2. To make the icons themselves, run `render.py` in lilabatch (`lilabatch --batch render.py`), then `python compose.py` with numpy and Pillow installed. The 256×256 achieved and unachieved PNGs and Steam's definitions and localization CSVs land in `lila-achievements/upload` in your temp folder.

| File | Holds |
| --- | --- |
| `table.py` | API names, display names, descriptions and which are hidden |
| `achievements.py` | The model and camera angle for each icon |
| `render.py` | Renders each model twice: lit, and flat black for the cut-out |
| `compose.py` | Cuts each model out, frames it on the badge and writes the PNGs and CSVs |
| `build_achievements.py` | Rebuilds `Achievements.lila` and its thumbnail |

## Troubleshooting

- `compose.py` runs in your own Python, not Lila's: install numpy and Pillow there.
- To redo some icons only, set `ACH_ONLY` to their API names, comma-separated, before running `render.py`.
