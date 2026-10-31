# Grey Card

A lighting reference: an 18% gray ball, a chrome ball, a 90% white ball and a ColorChecker chart on an 18% gray floor. Swap HDRIs and lights and read the colours to check they stay neutral.

## Use it

1. Open Grey Card from the Welcome screen's sample scenes, or open `grey_card.lila`: it looks through `Cam_GreyCard`.
2. Pick an HDRI from the viewport's environment menu. The gray ball and the chart's bottom row should stay gray; the chrome ball shows what the environment looks like from the subject.
3. To check numbers, capture the view (`ops.captureView(lookThrough="Cam_GreyCard", display="shaded", grid=False)`) and read the floor: lit 18% gray lands near 118 of 255.

## Troubleshooting

- The grays pick up a tint: lower Light Color in the environment menu (0 lights in pure gray), or check the `Sun` light's colour.
- Everything is too bright or too dark: the scene's `Post` node sets exposure to -1 stop; the viewport's own exposure adds to it.

## Tune it

At the top of `build_grey_card.py`: `SUN_INTENSITY`, `EXPOSURE` and `BALL_RADIUS`. Rerun it (`lilabatch --batch build_grey_card.py`) to rebuild the scene.
