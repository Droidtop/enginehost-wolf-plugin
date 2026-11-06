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
- **Not built yet:** the message window, the special
  names (`<SCREENSHOT>`, `<MAP_SCREENSHOT>`, `<CUT/...>`, `<SQUARE>`, `<GRADX/Y-...>`,
  `<LINE...>`), free transform, map tiles and characters.

## Text (`core/src/textlay.c`)

String pictures (150's text type) are laid out by the core and drawn as one quad per glyph,
through the same placement as any picture: the text's measured size is the picture's size for
its anchor. The message window will use the same layout.

**Glyphs come from the plugin** (`wolf_render_source.glyph`: a font, a size and a Unicode
character in; a white glyph image with its offsets, advance and the font's ascent out). Fonts are
pluggable (Droidtop/tracker#815): the game's own (Game.dat names its main font and up to three
sub-fonts, texts 3-6, and games ship `.ttf` files), then a font the core provides, then a default
set with CJK coverage. Rasterising them belongs in the confined asset worker with images. Until
that exists, the plugin rasterises in its own process, as it decodes images.

What each code does is from the manual, help/06specialword.html [V]:
- `\c[n]`: the colour from row n of the system database's type 12 (R, G, B). Before any `\c`, the
  text has row 0's colour [V: "デフォルト色" is row 0 in the owner's games [D]]. A row the
  table lacks gives white [M].
- `\f[n]` sets the size, and `\f+[n]` adds to it (a negative n subtracts) [V]. The size before
  any `\f` is 18 [M: the runtime's default is **Unknown**; games set it, e.g.
  `\f[\cself[18]]`].
- `\font[n]`: sub-font n (0 the main font) [V].
- `\space[n]`: the gap between lines [V]. Before any `\space` it is 4 [M]. A line holding nothing
  at all takes no gap [V: the manual's note on string pictures].
- `\ax[n]`, `\ay[n]`: the pen's place [V]. `\mx[n]`, `\my[n]` move the next character only [V].
  `\-[n]` narrows every following advance by n [V].
- `\\` draws a backslash [V]. `\r[text,ruby]` draws the text; the ruby is not drawn yet [M].
- **Lines** sit on one baseline: the largest ascent on the line below its top. A line is as tall
  as the largest size on it [M].
- **Not drawn yet, skipped, logged once:** edges and shadows (`\E`, `\N`, `\wE`, `\cE`, `\cS`),
  icons and images (`\i`, `\iS`, `\img`), `\m`, `\bx`, anti-aliasing switches, ruby, and the
  message-timing codes (`\!`, `\.`, `\^`, `\>`, `\<`, `\sp`), which only mean something in the
  message window.
- **Encodings:** UTF-8 (3.x) is decoded fully. In Shift-JIS (2.x), ASCII and half-width
  katakana are converted; double-byte characters need a conversion table that is not built yet,
  so they ask for U+FFFD (logged once).
- A character no font has is drawn as "?", or not at all when "?" is missing too [M].

