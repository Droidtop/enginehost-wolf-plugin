# Formats

Sources: djytw/wolf-rpg-formats (Kaitai specs, MIT) [V], elizagamedev/wolftrans's notes on 2.x
[V], Sinflower/UberWolf's readers for 3.x [V]; the official runtime still to be checked
(static pass). Numbers are little-endian.

## Strings
A u32 byte length including the terminating NUL, then the bytes. Shift-JIS in 2.x; UTF-8 in 3.x
files whose header marker byte is 0x55 [V].

## Version markers
CommonEvent.dat and the database files start `00 57 00 00 4F 4C` then a marker byte (0x00 for
2.x, 0x55 for 3.x) [V]. Footers: 0xC2 (2.x), 0xC3 (3.0), 0xC4 (3.3) [V]. Maps start with ten
zero bytes and `WOLFM\0`; their version byte is 0x65 (2.x) or 0x66 (3.x) [V], and from 3.5 a
newer version (>= 0x67) may be LZ4-compressed [V].

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
DX Library archives, one fixed key per engine era (Enginehost docs/new-engines/wolf.md has the
table) [V]. The core reads unpacked data, 2.x archives and 3.x archives on the engine's fixed key;
it refuses Pro and protect-key games [the owner's rule, #858].
