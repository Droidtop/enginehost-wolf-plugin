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
- **Not built yet:** the special
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
  any `\f` is system variable 8, "the base font size for messages and string pictures" [V,
  06systemvalue.html], or 18 when the game leaves it at 0 [M].
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

## The message window (`core/src/msgwin.c`)

The core answers 101 and 102 itself, through a `wolf_ui` of its own (`wolf_vm_use_window`). The
player's keys come in once a frame (`wolf_vm_input`: confirm, cancel, sub, the four directions).
What the runtime draws, from the manual [V]:
- **The window's background is the game's.** The FAQ: the text's place is set by the game's
  parallel common event "X[共]メッセージウィンドウ", and its frame is drawn with pictures. The
  core runs those common events, so it draws only the text and the cursors.
- **Text** at system variables 1 and 2 (the text's top left, read when the message opens), in the
  base size of system variable 8 [V]. -1 there means the runtime's default place, **Unknown**;
  the core uses the bottom left, four lines up [M].
- **Typing out** at system variable 9 characters a second [V]. 0 or less shows it at once [M]. A
  confirm key while it types shows the rest; one after closes the message [M]. `\^` ends the
  message without a key once it is shown [V]. `\!`, `\.`, `\>`, `\<` and system variable 10's
  opening wait are not built yet.
- **System variable 12** is 1 while a message shows [V].
- **The pause cursor** is the system database's type 10, row 0: its file and its vertical split.
  It is drawn at the window's top left plus system variables 5 and 6 once the text is shown,
  cycling its pieces every 8 frames [M: the speed].
- **Choices** [V]:
  - the window image is type 10 row 1, drawn when system variable 14 is 1, with its top centre at
    system variables 3 and 4;
  - the cursor image is type 10 row 2;
  - system variable 7 is the choice the cursor starts on.

  The window wraps the widest choice with 8 pixels of padding, and the cursor is a window around
  its line [M]. Up and down move past hidden (empty) choices and wrap round [M]. Confirm picks,
  and cancel works when the list allows it. -1 in variables 3 and 4 centres the list [M].
- **Faces** (system variable 0, the `@N` line; system database type 24: 0 no window, 1 window
  only, 2 up a face file [V]) are not drawn yet.
- The window draws between pictures 0-99999 and 100000 up [V, 04ev_picture.html's bands].

