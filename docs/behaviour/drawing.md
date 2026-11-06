# Drawing

How the core turns its state into something drawn. Wolf's own runtime draws through DX Library
on Direct3D. The core turns pictures into **quads** once, and two renderers draw the same list
(Droidtop/tracker#858, comment 6691411927; the owner: "doing things in software is a battery
hog"):
- **GLES**, in the plugin: the default, running under allocate-then-clamp now and through the
  Fibrewall GPU proxy once it lands (#815);
- **software**, in the core (`wolf_render_pictures`): the fallback where no GPU path is available
  (an isolated launch), and what the host tests check pixels against.

Markers as in the other pages: [V] from the manual, [M] our own choice, to be checked by a
comparison run's screenshots.

## Quads (`core/src/render.c`, `wolf_picture_quads`)

Each quad is four screen corners (the source's top left, top right, bottom right, bottom left),
a source rectangle in image pixels, opacity 0-1, colour multipliers and a blend. A file picture
is one quad; a window is nine. Both renderers follow the rules below, so they can't disagree
about geometry.

## Pictures

The plugin decodes images and hands them over through `wolf_render_source.image`. Pictures are
drawn for a number range, lowest first. The range lets the plugin put the map layers and the
message window between the manual's picture bands (commands.md, "Pictures").

- **Pieces** [V]: a file split into X by Y shows piece n (1-based), counted along rows. A split
  larger than the image is cut to the image; a piece the split doesn't have shows the first [M].
- **Windows** [V: a base image cut 3x3 into "four corners and the rest"]: nine quads. The corners
  keep their size, and the edges and middle are stretched to the window's size [M: stretched,
  not tiled]. The manual says a window smaller than two-thirds of the base "does not show
  correctly". The core shrinks the corner quads to fit [M].
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
- **Sampling:** nearest pixel, from each screen pixel's centre, in both renderers [M: DX
  Library's filtering is **Unknown**; a comparison run settles it]. The software renderer maps
  each pixel centre back through the quad's parallelogram.
- Everything is clipped to the frame, so a huge zoom costs no more than the frame's size.
- **Not built yet:** text pictures and the message window (they need glyphs), the special
  names (`<SCREENSHOT>`, `<MAP_SCREENSHOT>`, `<CUT/...>`, `<SQUARE>`, `<GRADX/Y-...>`,
  `<LINE...>`), free transform, map tiles and characters.
