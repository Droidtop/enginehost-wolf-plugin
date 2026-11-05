#include "wolf_read.h"

#include <stdlib.h>
#include <string.h>

void wolf_reader_init(wolf_reader *r, const uint8_t *p, size_t n) {
    r->p = p;
    r->n = n;
    r->at = 0;
    r->failed = 0;
}

const uint8_t *wolf_read_bytes(wolf_reader *r, size_t n) {
    if (r->failed || n > r->n - r->at) {
        r->failed = 1;
        return NULL;
    }
    const uint8_t *b = r->p + r->at;
    r->at += n;
    return b;
}

uint32_t wolf_read_u8(wolf_reader *r) {
    const uint8_t *b = wolf_read_bytes(r, 1);
    return b ? b[0] : 0;
}

uint32_t wolf_read_u16(wolf_reader *r) {
    const uint8_t *b = wolf_read_bytes(r, 2);
    return b ? (uint32_t)b[0] | (uint32_t)b[1] << 8 : 0;
}

uint32_t wolf_read_u32(wolf_reader *r) {
    const uint8_t *b = wolf_read_bytes(r, 4);
    return b ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24 : 0;
}

int32_t wolf_read_s32(wolf_reader *r) { return (int32_t)wolf_read_u32(r); }

uint32_t wolf_read_count(wolf_reader *r, uint32_t cap, size_t min_each) {
    uint32_t n = wolf_read_u32(r);
    if (r->failed) return 0;
    if (n > cap || (min_each && (size_t)n > (r->n - r->at) / min_each)) {
        r->failed = 1;
        return 0;
    }
    return n;
}

char *wolf_read_text(wolf_reader *r, uint32_t *len) {
    uint32_t n = wolf_read_u32(r);
    if (r->failed || n == 0 || n > WOLF_READ_TEXT_MAX) {
        r->failed = 1;
        return NULL;
    }
    const uint8_t *b = wolf_read_bytes(r, n);
    if (!b || b[n - 1] != 0) {
        r->failed = 1;
        return NULL;
    }
    char *s = malloc(n);
    if (!s) {
        r->failed = 1;
        return NULL;
    }
    memcpy(s, b, n);
    if (len) *len = n - 1;
    return s;
}

void wolf_skip_texts(wolf_reader *r) {
    uint32_t n = wolf_read_count(r, WOLF_READ_LIST_MAX, 5);
    for (uint32_t i = 0; i < n && !r->failed; i++) {
        uint32_t len = wolf_read_u32(r);
        if (len == 0 || len > WOLF_READ_TEXT_MAX) r->failed = 1;
        if (r->failed) break;
        const uint8_t *b = wolf_read_bytes(r, len);
        if (b && b[len - 1] != 0) r->failed = 1;
    }
}

void wolf_skip_numbers(wolf_reader *r) {
    uint32_t n = wolf_read_count(r, WOLF_READ_LIST_MAX, 4);
    wolf_read_bytes(r, (size_t)n * 4);
}

void wolf_skip_bytes(wolf_reader *r) {
    uint32_t n = wolf_read_count(r, WOLF_READ_LIST_MAX, 1);
    wolf_read_bytes(r, n);
}

void wolf_expect_u8(wolf_reader *r, uint32_t want) {
    if (wolf_read_u8(r) != want) r->failed = 1;
}

int wolf_read_head(wolf_reader *r, const uint8_t *magic, size_t magic_len, int marker_at, int *utf8) {
    *utf8 = 0;
    if (r->n >= 2 && r->p[1] == 0x50) return WOLF_READ_E_PROTECTED; /* Pro-protected */
    uint32_t indicator = wolf_read_u8(r);
    if (r->failed) return WOLF_READ_E_FORMAT;
    if (indicator != 0) return WOLF_READ_E_PROTECTED; /* the 2.x scrambler: refused for now */
    const uint8_t *m = wolf_read_bytes(r, magic_len);
    if (!m) return WOLF_READ_E_FORMAT;
    for (size_t i = 0; i < magic_len; i++) {
        if ((int)i == marker_at && m[i] == 0x55) {
            *utf8 = 1;
            continue;
        }
        if (m[i] != magic[i]) return WOLF_READ_E_FORMAT;
    }
    return WOLF_READ_OK;
}

int wolf_lz4_block(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len) {
    size_t si = 0, di = 0;
    while (si < src_len) {
        uint32_t token = src[si++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint32_t b;
            do {
                if (si >= src_len) return WOLF_READ_E_FORMAT;
                b = src[si++];
                lit += b;
                if (lit > dst_len) return WOLF_READ_E_FORMAT;
            } while (b == 255);
        }
        if (lit > src_len - si || lit > dst_len - di) return WOLF_READ_E_FORMAT;
        memcpy(dst + di, src + si, lit);
        si += lit;
        di += lit;
        if (si == src_len) break; /* the last sequence has literals only */
        if (src_len - si < 2) return WOLF_READ_E_FORMAT;
        size_t offset = (size_t)src[si] | (size_t)src[si + 1] << 8;
        si += 2;
        if (offset == 0 || offset > di) return WOLF_READ_E_FORMAT;
        size_t len = (token & 15u) + 4;
        if ((token & 15u) == 15) {
            uint32_t b;
            do {
                if (si >= src_len) return WOLF_READ_E_FORMAT;
                b = src[si++];
                len += b;
                if (len > dst_len) return WOLF_READ_E_FORMAT;
            } while (b == 255);
        }
        if (len > dst_len - di) return WOLF_READ_E_FORMAT;
        for (size_t k = 0; k < len; k++, di++) dst[di] = dst[di - offset]; /* may overlap */
    }
    return di == dst_len ? 0 : WOLF_READ_E_FORMAT;
}

int wolf_read_unpack(wolf_reader *r, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    uint32_t unpacked = wolf_read_u32(r), packed = wolf_read_u32(r);
    if (r->failed) return WOLF_READ_E_FORMAT;
    if (unpacked > WOLF_READ_UNPACKED_MAX) return WOLF_READ_E_TOO_BIG;
    if (packed != r->n - r->at) return WOLF_READ_E_FORMAT; /* the block ends the file */
    const uint8_t *src = wolf_read_bytes(r, packed);
    if (!src) return WOLF_READ_E_FORMAT;
    uint8_t *dst = malloc(unpacked ? unpacked : 1);
    if (!dst) return WOLF_READ_E_NOMEM;
    if (wolf_lz4_block(src, packed, dst, unpacked)) {
        free(dst);
        return WOLF_READ_E_FORMAT;
    }
    *out = dst;
    *out_len = unpacked;
    return WOLF_READ_OK;
}
