# Drawing

How the core turns its state into pixels. Wolf's own runtime draws through DX Library on
Direct3D. The core draws in software into the frame Enginehost hands its isolated process: an
isolated core has no GL context on a certified device, and the one isolated shape is
`EngineStepDriven` (Enginehost `docs/engine-sandbox.md`, "Consequences, decided here";
Droidtop/tracker#858, comment 6690530804). Markers as in the other pages: [V] from the manual,
[M] our own choice, to be checked by a comparison run's screenshots.

## Pictures (`core/src/render.c`, `wolf_render_pictures`)

The plugin decodes images and hands them over through `wolf_render_source.image`. The core draws
the pictures in a number range, lowest first, onto an opaque frame. The range lets the plugin
put the map layers and the message window between the manual's picture bands (commands.md,
"Pictures").

- **Pieces** [V]: a file split into X by Y shows piece n (1-based), counted along rows. A split
  larger than the image is cut to the image; a piece the split doesn't have shows the first [M].
- **Windows** [V: a base image cut 3x3 into "four corners and the rest"]: the corners are kept
  as they are and the edges and middle are stretched to the window's size [M: stretched, not
  tiled]. The manual says a window smaller than two-thirds of the base "does not show
  correctly". The core shrinks the corners to fit [M].
- **Anchor** [V]: X and Y place the anchor point (top left, centre, bottom left, top right,
  bottom right, top centre, bottom centre).
- **Zoom** [V]: percent of the piece's size, width and height apart when set so. -100 flips
  (the manual). The flip and the turn are both about the anchor point [M].
- **Angle** [V]: tenths of a degree, clockwise on screen [M: the direction]. The manual says
  windows ignore it (always 0). The core draws what the command gives, and 150 gives windows 0.
- **Colour** [V]: R, G and B as percent multipliers of the image's own colour, clamped at 255.
  So 200% on black stays black, as the manual says.
- **Opacity and blends** [V]: opacity 0-255 times the image's own alpha. Normal mixes toward the
  picture; add adds; subtract subtracts; multiply darkens by the picture's colour. Each is
  weighted by that alpha [M: the weighting]. The frame stays opaque.
- **Sampling:** nearest pixel, from each screen pixel's centre [M: DX Library's filtering is
  **Unknown**; a comparison run settles it].
- Everything is clipped to the frame, so a huge zoom costs no more than the frame's size.
- **Not built yet:** text pictures and the message window (they need glyphs), the special
  names (`<SCREENSHOT>`, `<MAP_SCREENSHOT>`, `<CUT/...>`, `<SQUARE>`, `<GRADX/Y-...>`,
  `<LINE...>`), free transform, map tiles and characters.
