# The `.wolf` archive (Wolf 3.x): DX Library's DXA, version 8

Sources:
- the local lane's static pass on Droidtop/tracker#858, 2026-11-05 (the Wolf 3.191 runtime embeds DX
  Library's archive reader unchanged in structure) [V];
- DX Library's own published archive reader (`DxArchive_.cpp`/`.h`, as mirrored in
  regomne/chinesize `DxLib/exBin/DxLib`), read for the layout. This is a third-party library's
  format, not Wolf's runtime; the core's reader is written from this description [V].

Wolf 2.x packed archives are a different, older scheme (the local lane: the 2.24 `Data.wolf` does
not start with `DX`). They are not covered here, and the core reports them as unsupported.

## Header (64 bytes at offset 0, never obfuscated)

All numbers are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | `DX` |
| 2 | 2 | version, 8 |
| 4 | 4 | index size: the size of the decoded index |
| 8 | 8 | data start: the file offset file data addresses count from |
| 16 | 8 | index start: the file offset of the index (the name table) |
| 24 | 8 | file table, relative to the decoded index |
| 32 | 8 | directory table, relative to the decoded index |
| 40 | 4 | code page of the names (932 Shift-JIS on real files) |
| 44 | 4 | flags: bit 0 no key, bit 1 index not compressed |
| 48 | 1 | Huffman span in KB for file data (0xFF: whole file) |
| 49 | 15 | reserved |

The code page is honoured from a whitelist (932, 936, 949, 950, 1252, 1200, 1201, 65001, and
the 32-bit Unicode forms), and anything else means 932, as the local lane found in the runtime.

## Obfuscation (XOR, not cryptography)

- **Key.** A key string makes a 7-byte key. Take the bytes at even positions and, separately, those
  at odd positions, and compute the standard CRC-32 (reflected polynomial 0xEDB88320) of each.
  The key is the 4 little-endian bytes of the first CRC, then the low 3 bytes of the second. A
  string shorter than 4 bytes has the 8 bytes `DXBDXARC` appended first (the source ends at its first
  NUL, as it is copied as a C string). An archive's key string is at most 63 bytes; a longer one is
  cut to 63. With no key string at all, the library uses `DXBDXARC` itself.
- **XOR.** Byte *i* of a run is XORed with `key[(start + i) % 7]`, where `start` is a position the
  reader chooses per run (below). With flag bit 0 set nothing is XORed.
- **Where the key string comes from** is the open question on #858; the owner's rule is to derive it
  from the game and never store it. The reader takes it as an input.

## Names and lookup

A path is looked up one component at a time from the root, components separated by `/` or `\`. A
component is upper-cased one character at a time in the archive's code page, changing only ASCII
`a`–`z`, so the second byte of a Shift-JIS character is never changed. Its parity is the 16-bit sum
of the upper-cased bytes, and its length is rounded up to the 4-byte units of the name table with
room for at least one NUL. An entry matches when the length, the parity and the upper-cased bytes
are all equal. The core handles the byte code pages (932, 936, 949, 950, 1252, 65001); an archive
in a UTF-16 or UTF-32 code page is reported as unsupported until one is seen.

## The index

- Not compressed (flag bit 1): `index size` bytes at `index start`, XORed with the archive key
  from position 0.
- Compressed: everything from `index start` to the end of the file, XORed with the archive key from
  position 0, then Huffman-decoded, then LZ-decoded; the result is `index size` bytes.

Inside the decoded index:
- **Name table**, at index offset 0. An entry is a 16-bit length *n* (in units of 4 bytes) and a
  16-bit parity, then the name upper-cased in *n*·4 bytes, then the name as written in *n*·4 bytes,
  both NUL-padded.
- **File table** entries, 72 bytes each: name offset (into the name table), attributes (bit 0x10
  marks a directory), three 8-byte times, data offset, data size, LZ size (all ones: not
  LZ-compressed), Huffman size (all ones: not Huffman-compressed). For a directory, the data offset
  is an offset into the directory table.
- **Directory table** entries, 32 bytes each: its own file entry's offset in the file table (the
  root has all ones), its parent directory's offset (all ones for the root), the number of files,
  and the offset of its first file entry. The root directory is at offset 0.

## A file's data, and its key

- **Each file has its own key**, made from the archive key string, then the file's upper-cased name,
  then the upper-cased name of each enclosing directory up to (not including) the root, concatenated
  without separators, at most 2040 bytes (the core refuses a longer one rather than guess how the
  library cuts it).
- Let *stage* be the LZ size when the file is LZ-compressed, else the data size.
- **With Huffman:** read *huff* (the Huffman size) bytes at `data start + data offset`, XOR from
  position *data size*, and Huffman-decode them.
  - When the Huffman span is not 0xFF and *stage* is larger than twice the span, only the first and
    last *span* KB of the stage were Huffman-coded. Their decoded bytes are the front and back. The
    middle, *stage* − 2·span KB, follows the Huffman bytes in the file raw, XORed from position
    *data size* + *huff*.
  - Otherwise the decoded bytes are the whole stage.
- **Without Huffman:** read *stage* bytes, XOR from position *data size*.
- **Then**, when LZ-compressed, LZ-decode the stage to *data size* bytes.

## Huffman coding

- **Header**, read most-significant bit first: a 6-bit width *w* then *w*+1 bits of decoded size,
  the same for the coded size (unused), then 256 weights. Each weight is coded as a 3-bit width *k*,
  a sign bit, and 2(*k*+1) bits of difference from the previous weight; the first is absolute.
  Weights are 16-bit.
- **Tree**: 256 leaves (node *i* = byte *i*) and 255 joins (nodes 256…510). Repeatedly take, among
  the nodes not yet joined in index order, the lowest weight (a strictly lower weight replaces the
  current lowest, which becomes second) and the second lowest, and join them in a new node whose
  child 0 is the lowest and child 1 the second. The root is node 510.
- **Body**, after the header, rounded up to a byte: bits are read least-significant first. Each byte
  of output walks from the root, taking child 0 or 1 per bit, to a leaf.

## LZ coding

- A 9-byte header: decoded size (4), coded size including the header (4), escape byte.
- Then, for the coded bytes after the header:
  - a byte other than the escape is a literal;
  - escape, escape is a literal escape byte;
  - escape, *c* (with *c* reduced by 1 when it is above the escape) is a copy: length = (*c* >> 3)
    plus, when bit 2 is set, a following byte shifted left by 5, plus 4; the distance width is
    *c* & 3 (0: one byte, 1: two bytes, 2: three bytes), and the distance read is one less than the
    real one. Copies may overlap their own output and are done byte by byte forward.

## Safety (the core's own rules, from the risk list)

Every size, offset and count comes from the file. The reader checks each against the file size, the
decoded index and fixed caps (an index of at most 256 MB, a file of at most 1 GB, names within the
name table), checks every LZ copy against the bytes produced, and never reads or writes outside a
buffer. A name with `..` or an absolute path is refused. Directory walks stop at 64 levels, so a
directory table that loops cannot recurse forever.

The core's reader is `core/src/dxa.c`. Its tests (`core/test/test_dxa.c`) build archives from this
description with their own Huffman and LZ encoders, check the key against vectors computed
separately with Python's `zlib`, and read thousands of mutated archives under the address and
undefined-behaviour sanitizers. They show the reader matches this description; whether this
description matches real `.wolf` files is the local lane's check on #858.
