/*
 * Reading Wolf RPG Editor project files: the shared pieces of docs/behaviour/formats.md,
 * "Reading the project files". Every count, length and offset is checked against the input
 * and a cap; nothing is read past the end.
 */
#ifndef ENGINEHOST_WOLF_READ_H
#define ENGINEHOST_WOLF_READ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caps on what a file may ask for, so a bad file cannot exhaust memory. */
#define WOLF_READ_TEXT_MAX (16u << 20)
#define WOLF_READ_LIST_MAX 1000000u
#define WOLF_READ_UNPACKED_MAX (256u << 20)

typedef struct {
    const uint8_t *p;
    size_t n, at;
    int failed; /* set by the first read that did not fit; every later read fails too */
} wolf_reader;

enum {
    WOLF_READ_OK = 0,
    WOLF_READ_E_FORMAT = -1,    /* the bytes are not what the format says */
    WOLF_READ_E_PROTECTED = -2, /* Pro-protected or 2.x-scrambled: refused */
    WOLF_READ_E_NOMEM = -3,
    WOLF_READ_E_TOO_BIG = -4,
};

void wolf_reader_init(wolf_reader *r, const uint8_t *p, size_t n);
uint32_t wolf_read_u8(wolf_reader *r);
uint32_t wolf_read_u16(wolf_reader *r);
uint32_t wolf_read_u32(wolf_reader *r);
int32_t wolf_read_s32(wolf_reader *r);
/* n bytes in place, or NULL (and failed set) when they are not there. */
const uint8_t *wolf_read_bytes(wolf_reader *r, size_t n);
/* A count, which must also be at most cap and fit the bytes left at min_each bytes apiece. */
uint32_t wolf_read_count(wolf_reader *r, uint32_t cap, size_t min_each);
/* A text: a malloc'd NUL-terminated copy (its length in *len if not NULL), or NULL on failure. */
char *wolf_read_text(wolf_reader *r, uint32_t *len);
/* Skips a byte list, text list or number list, checking it. */
void wolf_skip_texts(wolf_reader *r);
void wolf_skip_numbers(wolf_reader *r);
void wolf_skip_bytes(wolf_reader *r);
/* Expects one byte; a different one fails the reader. */
void wolf_expect_u8(wolf_reader *r, uint32_t want);

/* The envelope and magic: checks the indicator and the magic (9 or 8 bytes, with the UTF-8
 * marker 0x55 allowed at marker_at, or none when marker_at < 0), leaving the reader just past
 * the magic. *utf8 is set from the marker. Answers WOLF_READ_OK, _FORMAT or _PROTECTED. */
int wolf_read_head(wolf_reader *r, const uint8_t *magic, size_t magic_len, int marker_at, int *utf8);

/* A packed body (u32 unpacked size, u32 packed size, an LZ4 block that ends the input) at the
 * reader's position, unpacked into a malloc'd buffer *out of *out_len bytes. */
int wolf_read_unpack(wolf_reader *r, uint8_t **out, size_t *out_len);

/* One LZ4 block (the block format, no frame) into exactly dst_len bytes; 0 or WOLF_READ_E_FORMAT. */
int wolf_lz4_block(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif
