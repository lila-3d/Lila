# Level

An eroded valley with a hill town laid out on the flattest site the settlement planner found, a ruined keep on the next best site with rubble piled at its breaches, maples around it, and a market corner of crates and barrels in the town square. Everything was settled with physics.

## Use it

1. Open `level.lila` and look through `Cam_Overview`, `Cam_Town`, `Cam_Keep` or `Cam_Market` (in the view, Panels > Look Through Selected).
2. Open the Level Toolkit (Level > Level Toolkit) and select `Terrain`, `HillTown` or one of the `Keep*` walls. Their settings are on the modifier in the Attribute Editor: change the terrain's seed or erosion, the town's profile, growth or seed, or a wall's bond, height and wear.
3. Select the crates or rubble and use the Level Toolkit's Dress tools (Grab, Push, Wiggle, Settle) to move them around with physics.

## Troubleshooting

- The keep, rubble, trees or market props sit in the air or underground after you change the terrain: they were placed on the ground as it was built. Rerun `build_level.py` (`lilabatch --batch build_level.py`) to place them on the new ground.
- The terrain and town are plain grey: geometry made by their modifiers has no material of its own.

## Tune it

At the top of `build_level.py`: `TERRAIN_SIZE`, `TERRAIN_SEED` (another valley, and the town and keep move to its best sites), `TOWN_RADIUS` and `KEEP_SIZE`. The keep's walls, their heights and the breaches are the `KEEP_WALLS` table.
