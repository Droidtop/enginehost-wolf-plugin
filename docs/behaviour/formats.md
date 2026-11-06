# Formats

Sources: djytw/wolf-rpg-formats (Kaitai specs, MIT) [V], elizagamedev/wolftrans's notes on 2.x
[V], Sinflower/UberWolf's readers for 3.x [V]; the official runtime still to be checked
(the local lane's static pass). Numbers are little-endian.

## Strings
A u32 byte length including the terminating NUL, then the bytes. Shift-JIS in 2.x; UTF-8 in 3.x
files whose header marker byte is 0x55 [V].

## Version markers
CommonEvent.dat and the database files start `00 57 00 00 4F 4C` then a marker byte (0x00 for
2.x, 0x55 for 3.x) [V]. Footers: 0xC2 (2.x), 0xC3 (3.0), 0xC4 (3.3) [V]. Maps start with ten
zero bytes and `WOLFM\0`; their version is 0x65 (2.x) or 0x66 (3.x) [V], and from 3.5 a
newer version (>= 0x67) may be LZ4-compressed [V]; "Maps (.mps)" has the whole layout.

## Event commands
A command list is a u32 count followed by that many commands [V]. One command:

| Field | Type | Notes |
|---|---|---|
| argc+1 | u8 | number of integer arguments plus one |
| code | u32 | the command, e.g. 101 message, 121 set variable, 150 picture [V] |
| args | argc x s32 | integer arguments; variable references are "magic numbers" [M] |
| indent | u8 | nesting depth for branches and loops |
| nstr | u8 | number of string arguments |
| strings | nstr x string | |
| tail | u8 | 0, or 1 when a move route follows |

A move route (tail 1): 5 bytes not yet understood, a u8 of flags, a u32 step count, then the
steps, each `u8 id, u8 argc, argc x u32, 01 00` [V].

Whether every list ends with the blank command (code 0) is not confirmed [M]; the survey tool
counts strictly first and flags files it had to count loosely.

## Archives (.wolf)
Wolf 3.x archives are DX Library archives, version 8; archive.md describes them. The key string is
derived from the game when an archive is opened and never stored (the owner's rule, #858). Pro and
protect-key games are refused.

## Reading the project files (shared by every section below)
[R] rpg-maker-clone 75b1c34 (`mruby-wolf/mrblib`, MIT), read only, never built, run or copied;
it was itself checked against the editor's bundled sample game. The editor's terms of use (9.1)
allow analysing these files' formats; Pro-protected data (9.2) is excluded and refused here.

- Integers are 32-bit little-endian; "count" fields are unsigned and every one is bounded.
- **Text:** a u32 byte length that counts the terminating NUL (so at least 1), then the bytes;
  the last byte must be 0. Shift-JIS or UTF-8 by the file's marker, as in "Strings".
- **Text list:** u32 count, then that many texts. **Number list:** u32 count, then s32 each.
  **Byte list:** u32 count, then one byte each.
- **Envelope:** byte 0 is an indicator, 0 for a plain file. A file whose byte 1 is 0x50 is
  Pro-protected and is refused. A plain file has its magic right after the indicator.
  A 2.x Game.dat or database .dat with a non-zero indicator is scrambled by the editor's
  optional 2.x scrambler; it is **refused** here for now (owner to decide; the reference
  reads it).
- **Packed body (3.5+):** after the version byte, a u32 unpacked size, a u32 packed size,
  then one LZ4 *block* (no frame header) of exactly that packed size, ending the file. The
  rest of the file is read from the unpacked bytes.

## CommonEvent.dat
[R] as above.

| Part | Layout |
|---|---|
| head | indicator 0; magic `57 00 00 4F 4C m 46 43 00` where m is 0x00 (Shift-JIS) or 0x55 (UTF-8); version byte |
| packed | version 0x93 or 0xCC: the body is packed (above), and every command carries a trailing byte list (below) |
| body | u32 event count, the events, a footer byte (at least 0x89), and nothing after it |

One common event, in order:
- 0x8E; u32 number; a byte whose low 4 bits are the run condition (0 called only, 1 automatic,
  2 parallel, 3 parallel always) and high 4 bits the condition's operator; s32 condition
  variable; s32 condition value; u8 number-argument count; u8 string-argument count;
- text: name; the command list (as in "Event commands"); text: Unknown; text: memo;
- 0x8F; text list: argument names; byte list: argument kinds; u32 n then n text lists
  (argument choices); u32 n then n number lists (choice values); number list: argument
  defaults;
- 0x90; s32 colour; 100 texts: the self variables' names;
- 0x91; text: Unknown; then either 0x91 (end), or 0x92, text: the return value's name, s32: the
  self variable (0-99) returned to a caller, and 0x92.

In packed files every command ends with one more byte, a count n, then n bytes (meaning
Unknown). Events are numbered by their place in the file; whether the stored number can differ
from the place is Unknown (the reference indexes by place).

## Databases (DataBase, CDataBase, SysDataBase)
[R] as above. Each is a `.project` (the schema) and a `.dat` (the values).

`.project`: u32 type count, then per type:
- text: name; u32 n then n texts (field names); u32 n then n texts (row names); text: memo;
- byte list: each field's kind (0 number, 1 file name, 2 database reference, 3 choices);
- u32 n then n texts; u32 n then n text lists; u32 n then n number lists (each field's extra
  strings, choices and values); u32 n then n s32 (each field's default);
- nothing after the last type.

`.dat`: indicator 0; magic `57 00 00 4F 4C m 46 4D 00`; version byte (0xC4: packed body).
Then u32 type count (the project's), then per type:
- `FE FF FF FF`; s32 how row numbers are set (meaning Unknown); u32 field count;
- per field a u32 position: 1000 x block + slot, block 1 a number, 2 a text;
- u32 row count; per row, one s32 per number field, then one text per text field.

Then a footer byte equal to the version, and nothing after (in a packed `.dat` the footer is
the last byte of the unpacked body). The `.dat` is authoritative on
counts: a project with more field or row names than the `.dat` holds keeps only those it has.

## Game.dat
[R] as above.

- indicator 0; magic `57 00 00 4F 4C 00 46 4D`; version byte, 0x55 for UTF-8;
- byte list: settings (0 tile size, 1 and 2 character directions, 4 frames per second,
  5 shadows, 7 animation patterns, 8 movement width, ... Unknown beyond these);
- text list: 0 the game's title, 1 always "0000-0000", 2 a key string (UberWolf labels it
  "decrypt key"; in all three survey games with unpacked data it is present, 18-30 bytes [D];
  its value is never logged or stored), 3 the font, 4-6 sub-fonts,
  7 the hero's graphic, 8 the subtitle; others Unknown;
- s32 file size; s32 Unknown; u32 n then n u16 settings (16 screen width, 17 height,
  18 the editor version; 0 width or height means 320 x 240);
- any remaining bytes: Unknown, not read.

**Variable counts** (how many normal, string and system variables a game has) are not in
Game.dat, and the reference keeps them unbounded. Where the runtime takes them from is
Unknown; the core uses fixed defaults until it is read.

## Maps (.mps)
[R] as above, and [V] where marked. Loaded by `core/src/wolf_map.c`.

**Head.** A file whose byte 1 is 0x50 is Pro-protected and refused. Otherwise the file starts
with a 20-byte magic: ten zero bytes, `WOLFM\0`, four zero bytes, where the first of those four
is 0x55 in a UTF-8 map (the text encoding, as in "Strings"). There is no indicator byte. The
file's last byte is always 0x66. After the magic: u32 version; a u8 (meaning Unknown).

**Packing.** The sources disagree: the reference unpacks every map from version 0x65 on, while
the local lane saw plain 0x65 and 0x66 maps and LZ4 only from 0x67 [V]. So the body is taken as
packed (u32 unpacked size, u32 packed size, an LZ4 block that ends the file, as in "Reading the
project files") when the version is at least 0x65 and the packed size is exactly the bytes
left after those two words; otherwise it is read plain. Which versions really pack is
**Unknown**; a plain body whose first eight bytes happen to look like that would be misread
(and then refused, since the LZ4 block must produce a well-formed body).

**Body**, in order:
- text: Unknown; s32 tileset number; s32 width; s32 height (in tiles); u32 event count;
- from version 0x67: s32 Unknown, then s32 layer count; before it, 3 layers;
- the tiles: layer count x height x width s32, layer by layer, row by row. In a UTF-8 map an
  s32 of -1 in their place means the map stores no tiles (the -1 is consumed). A map whose
  first tile really is -1 cannot be told from that; whether it can exist is Unknown;
- the events, each starting with 0x6F, then 0x66, and nothing after it. The number of events
  must equal the count.

Negative sizes are refused; the loader also caps width and height at 100,000, layers at 64,
events at 100,000, pages per event at 10,000 and route steps at 100,000 (our caps, not the
editor's).

**Tile values.** From 100,000 on, an autotile: value / 100,000 is its autotile slot plus one,
and value % 100,000 is four decimal digits, one per corner (top left, top right, bottom left,
bottom right), choosing each quarter's shape. Below 100,000, a chip of the tileset's base
sheet, 8 chips to a row. How they are drawn is in "Drawing tiles" below.

**Drawing tiles** (`wolf_tile_pieces`, `core/src/wolf_map.c`).
- A **base chip sheet** is 8 chips wide and any number of chips tall, at the game's tile size
  (16, 32, 40 or 48 pixels) [V, manual 06material.html]. Value v is the chip at column v % 8,
  row v / 8.
- An **autotile sheet** is 5 chips tall and one chip wide per animation frame [V, 06material].
  Its rows, top to bottom, are the manual's five joins: toward the centre, vertical,
  horizontal, outward, and surrounded on all sides.
- **The editor stores each quarter's join in the map**, so a tile drawn from the map needs no
  look at its neighbours. The four digits pick the row for the top-left, top-right, bottom-left
  and bottom-right quarter, and each quarter copies the matching quarter of the chip in that
  row. Evidence [D]: in the 244 maps of the survey games, 99.9% of the 1.4 million autotile
  corner digits are 0 to 4, the five rows. 4 is the most common (surrounded, the inside of a
  body of water). The few others come from maps the quick scan misread. Digit d taking row d is
  [M]: the counts fit it, and a comparison run's screenshot settles it. A digit past 4 draws
  nothing.
- **Animation:** the column is the animation frame, stepping every 20 frames, or every N for a
  sheet whose file name ends in `_FRAME=N` before the extension (3.50 on) [V, 06material].
  When the step counter starts, and whether all autotiles share it, is **Unknown**.
- Autotile slots seen in real maps run 1 to 16, with a few strays [D]; TileSetData.dat holds
  15 sheets in 2.x and 31 in 3.x [R].
- **Unknown:** how tiles changed at run time (240/241) get their joins. The editor's stored
  digits cover only the map as saved. A changed autotile needs the joins worked out from its
  neighbours, and the rule for that is not in the manual. Until a comparison run settles it,
  the core will give a changed autotile "surrounded" (4) on every quarter [M] (240 and 241 are
  not built yet).

**One event:** `39 30 00 00`; s32 id; text name; s32 x; s32 y; u32 page count; four zero
bytes; the pages, each starting with 0x79; then 0x70. The number of pages must equal the
count. The id picks the event's self-variable slots: a thread started on one of its pages reads
1,100,000 + X as that event's self variable X (commands.md, "Call values"). The loader does not
require ids to be in order or unique; whether the editor ever writes them otherwise is Unknown.

**One page:**
- s32 Unknown; text graphic (a file name, possibly empty);
- u8 direction (the sheet row is direction / 2 - 1), u8 frame, u8 opacity, u8 blend mode;
- u8 trigger: 0 when the hero checks it, 1 automatic, 2 parallel, 3 hero touch, 4 event touch;
- four conditions as four bytes, then four s32 variables, then four s32 values. In each byte
  bit 0 says the condition is used (an unused one still holds a variable, typically
  1,000,000) and the high 4 bits are the comparison, numbered as the command comparisons are;
  the other low bits are Unknown. How they decide the page an event shows is maps.md;
- u8 animation speed, u8 move speed, u8 move frequency, u8 move type (0 none, 1 custom route,
  2 random, 3 toward the hero), u8 options (0x01 idle animation, 0x02 move animation, 0x04
  fixed direction, 0x08 passes through, 0x10 above the hero, 0x20 square hit box, 0x40 half a
  tile up, 0x80 half a tile left), u8 route options (bits Unknown);
- u32 route step count, then the steps, each `u8 id, u8 argc, argc x s32, 01 00` (the same
  steps as a command's move route in "Event commands");
- the page's command list, as in "Event commands"; from version 0x67 every command carries the
  3.5+ trailing byte list ("CommonEvent.dat");
- s32 features (meaning Unknown beyond the next point); u8 shadow; u8 contact range x; u8
  contact range y; only when features is more than 3, a u8 (the reference calls it transfer;
  meaning Unknown);
- 0x7A.

**Running pages.** A loaded map widens the VM's self-variable slots to its highest event id
(`wolf_map_config`) and can start any page's list on a thread with that event's self variables
(`wolf_map_start_page`). Choosing pages and running the automatic and parallel ones is maps.md.

## TileSetData.dat
[R] as above. Loaded by `wolf_tilesets_load` (`core/src/wolf_data.c`).

The envelope and magic of a database file (`W 00 00 O L 00 F M 00`, the UTF-8 marker 0x55 at the
magic's sixth byte), then a u8 version and a u32 tileset count. Each tileset:
- text name; text base chip sheet (a file name);
- the autotile sheets: texts, with no count, up to a 0xFF byte. A text's length word cannot
  start with 0xFF (no text is that long), so the byte is unambiguous. The reference expects 15
  in a 2.x file and 31 in a 3.x one; the loader takes up to 256 (our cap) and checks no number;
- 0xFF; a byte list: one tag number per base chip;
- 0xFF; a u32 count, which must equal the tag count, then a u32 of flags per base chip.

Then 0xCF, which ends the file. Chip flags [R]: 0x01, 0x02, 0x04, 0x08 the hero cannot leave
downward, left, right, up (all four: impassable); 0x10 drawn above characters; 0x40 a
character on it has its lower half see-through; 0x80 counter; 0x100 passable but hides a
character behind it; 0x200 passability follows the layer below. Other bits are kept, meaning
Unknown. How an autotile's passability is stored is Unknown here (not in these flags).
**Unknown:** whether a 3.5+ TileSetData.dat is ever LZ4-packed like the other files; the
reference reads it plain, and so does the loader (a packed one would be refused as malformed).
Caps: 10,000 tilesets, 256 autotile sheets each (ours).

## MapTree.dat
[R] as above. Loaded by `wolf_map_tree_load` (`core/src/wolf_data.c`).

Ten bytes whose meaning is Unknown (not checked), a u8 version, a u32 entry count, then per
entry an s32 parent map (-1 at the root) and an s32 map number, in the order the editor lists
maps; then 0xBC, which ends the file. A map number indexes the system database's map settings
type, which names the .mps file [R]; the loader does not follow that link. No envelope byte is
read; a file whose byte 1 is 0x50 is still refused as Pro-protected, as every other file is
[M: that the rule reaches this file is our assumption]. Cap: 100,000 entries (ours).
