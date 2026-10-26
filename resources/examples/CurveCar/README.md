# Curve Car

A 1960s Cobra 427 roadster built from curves. The body is a loft through twelve section curves, and one live boolean on it cuts the wheel wells, cockpit, grille mouth, lamp buckets, side vents and panel gaps. Wheels are revolved from profiles, the windscreen is lofted between two rails, and the pipes, trim and seats are piped and bevelled from curves.

## Use it

1. Open `curve_car.lila` and look through `Cam_Car` (the view's Panels > Look Through Selected).
2. Unhide `BodySection0` to `BodySection11` under `CurveCar/Shell` and move or scale their points: the body and everything cut into it update.

## Troubleshooting

- The cockpit, arches or gaps fill in: the cutters under `CurveCar/Shell` (`WellCutter*`, `CockpitCutter`, `MouthCutter`, `LampCutter*`, `VentCutter*`, `*Gap*`) are hidden, not deleted; the boolean modifier on `Body` needs them.
- Trim, lamps and panel gaps sit where the build script found the body's skin. After reshaping the sections, rerun `build_curve_car.py` (`lilabatch --batch build_curve_car.py`) to place them again.

## Tune it

At the top of `build_curve_car.py`: `WHEELBASE`, `FRONT_TRACK`, `REAR_TRACK`, and `FRONT_TYRE` and `REAR_TYRE` (radius, width). The body shape is the `BODY` table: one row per section, nose to tail, giving the bottom, the bonnet or deck height on the centre line, the fender crown's height and offset, the hip's half width and height, and the sill's half width.
