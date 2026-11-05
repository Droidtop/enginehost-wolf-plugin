/*
 * DX Library archive reader, version 8. See dxa.h and docs/behaviour/archive.md.
 *
 * Rules kept throughout: every number from the archive is untrusted. Each offset is checked
 * against the buffer it points into before it is used, sizes are capped, and decoders never read
 * or write outside their buffers.
 */
#include "dxa.h"

#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 64
#define FILE_ENTRY 72
#define DIR_ENTRY 32
#define NONE UINT64_MAX
#define ATTR_DIRECTORY 0x10u
#define FLAG_NO_KEY 0x1u
#define FLAG_INDEX_PLAIN 0x2u
#define KEY_STRING_MAX 63
#define FILE_KEY_STRING_MAX 2040
#define INDEX_CAP (256ull << 20)
#define FILE_CAP (1ull << 30)
#define WALK_DEPTH_MAX 64

static const uint8_t DEFAULT_KEY_STRING[8] = {'D', 'X', 'B', 'D', 'X', 'A', 'R', 'C'};

struct dxa {
    dxa_source src;
    uint8_t key_string[KEY_STRING_MAX];
    size_t key_string_len;
    uint8_t key[7];
    int no_key;
    int code_page;
    uint64_t data_start;
    uint8_t huffman_kb;
    uint8_t *index;
    uint64_t index_size;
    uint64_t file_table;
    uint64_t dir_table;
};

/* ---- little helpers ---------------------------------------------------------------------- */

static uint64_t le(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* True when [off, off + len) lies within [0, total). */
static int fits(uint64_t off, uint64_t len, uint64_t total) {
    return off <= total && len <= total - off;
}

uint32_t dxa_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void dxa_make_key(const uint8_t *s, size_t n, uint8_t key[7]) {
    /* A string shorter than 4 bytes has the default key string appended. The library copies
     * with strcpy, so the source ends at its first NUL. */
    uint8_t tmp[4 + sizeof DEFAULT_KEY_STRING];
    if (n < 4) {
        size_t m = 0;
        while (m < n && s[m] != 0) m++;
        memcpy(tmp, s, m);
        memcpy(tmp + m, DEFAULT_KEY_STRING, sizeof DEFAULT_KEY_STRING);
        s = tmp;
        n = m + sizeof DEFAULT_KEY_STRING;
    }
    uint32_t c[2];
    for (int half = 0; half < 2; half++) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = (size_t)half; i < n; i += 2) {
            crc ^= s[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        c[half] = ~crc;
    }
    for (int i = 0; i < 4; i++) key[i] = (uint8_t)(c[0] >> (8 * i));
    for (int i = 0; i < 3; i++) key[4 + i] = (uint8_t)(c[1] >> (8 * i));
}

static void xor_run(uint8_t *p, size_t n, uint64_t start, const uint8_t key[7]) {
    size_t k = (size_t)(start % 7);
    for (size_t i = 0; i < n; i++) {
        p[i] ^= key[k];
        if (++k == 7) k = 0;
    }
}

static int read_at(const dxa_source *s, uint64_t off, void *buf, size_t len) {
    if (!fits(off, len, s->size)) return DXA_E_FORMAT;
    if (len == 0) return DXA_OK;
    return s->read(s->ctx, off, buf, len) == 0 ? DXA_OK : DXA_E_IO;
}

/* ---- Huffman ---------------------------------------------------------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t byte;
    int bit;
} bits;

/* Most-significant bit first, as the header is written. Returns -1 past the end. */
static int bits_msb(bits *b, int n, uint64_t *out) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        if (b->byte >= b->len) return -1;
        v = (v << 1) | ((b->p[b->byte] >> (7 - b->bit)) & 1u);
        if (++b->bit == 8) { b->bit = 0; b->byte++; }
    }
    *out = v;
    return 0;
}

/*
 * Decodes one Huffman block. The decoded size from its header must equal want; anything else is
 * a format error, so a header cannot ask for more memory than the caller expected.
 * With want == NONE the header's size is accepted up to cap and returned in *got.
 */
static int huffman_decode(const uint8_t *in, size_t in_len, uint64_t want, uint64_t cap,
                          uint8_t **out, uint64_t *got) {
    bits b = {in, in_len, 0, 0};
    uint64_t w, original, unused;
    if (bits_msb(&b, 6, &w) || bits_msb(&b, (int)w + 1, &original)) return DXA_E_FORMAT;
    if (bits_msb(&b, 6, &w) || bits_msb(&b, (int)w + 1, &unused)) return DXA_E_FORMAT;
    if (want != NONE ? original != want : original > cap) return DXA_E_FORMAT;

    uint64_t weight[511];
    int child[511][2];
    int joined[511];
    uint16_t prev = 0;
    for (int i = 0; i < 256; i++) {
        uint64_t k, minus, diff;
        if (bits_msb(&b, 3, &k) || bits_msb(&b, 1, &minus) || bits_msb(&b, (int)(k + 1) * 2, &diff))
            return DXA_E_FORMAT;
        uint16_t v = i == 0 ? (uint16_t)diff : (uint16_t)(minus ? prev - (uint16_t)diff : prev + (uint16_t)diff);
        weight[i] = v;
        prev = v;
    }
    for (int i = 0; i < 511; i++) { joined[i] = 0; child[i][0] = child[i][1] = -1; }
    for (int node = 256; node < 511; node++) {
        int min1 = -1, min2 = -1;
        for (int i = 0; i < node; i++) {
            if (joined[i]) continue;
            if (min1 == -1 || weight[min1] > weight[i]) { min2 = min1; min1 = i; }
            else if (min2 == -1 || weight[min2] > weight[i]) min2 = i;
        }
        weight[node] = weight[min1] + weight[min2];
        child[node][0] = min1;
        child[node][1] = min2;
        joined[min1] = joined[min2] = 1;
    }

    size_t body = b.byte + (b.bit != 0);
    uint8_t *dst = malloc(original ? (size_t)original : 1);
    if (!dst) return DXA_E_NOMEM;
    size_t pos = body;
    int bit = 0;
    for (uint64_t o = 0; o < original; o++) {
        int node = 510;
        while (node >= 256) {
            if (pos >= in_len) { free(dst); return DXA_E_FORMAT; }
            int x = (in[pos] >> bit) & 1;
            if (++bit == 8) { bit = 0; pos++; }
            node = child[node][x];
        }
        dst[o] = (uint8_t)node;
    }
    *out = dst;
    if (got) *got = original;
    return DXA_OK;
}

/* ---- LZ --------------------------------------------------------------------------------- */

/* Decodes one LZ block whose header must say it decodes to exactly want bytes. */
static int lz_decode(const uint8_t *in, size_t in_len, uint64_t want, uint8_t **out) {
    if (in_len < 9) return DXA_E_FORMAT;
    uint64_t dest = le(in, 4), src = le(in + 4, 4);
    uint8_t esc = in[8];
    if (dest != want || src < 9 || src > in_len) return DXA_E_FORMAT;
    uint8_t *o = malloc(dest ? (size_t)dest : 1);
    if (!o) return DXA_E_NOMEM;
    size_t p = 9, n = 0;
    while (p < src) {
        uint8_t c = in[p++];
        if (c != esc) {
            if (n >= dest) goto bad;
            o[n++] = c;
            continue;
        }
        if (p >= src) goto bad;
        c = in[p++];
        if (c == esc) {
            if (n >= dest) goto bad;
            o[n++] = c;
            continue;
        }
        if (c > esc) c--;
        size_t length = c >> 3;
        if (c & 4) {
            if (p >= src) goto bad;
            length |= (size_t)in[p++] << 5;
        }
        length += 4;
        size_t dist;
        switch (c & 3) {
        case 0:
            if (p + 1 > src) goto bad;
            dist = in[p]; p += 1; break;
        case 1:
            if (p + 2 > src) goto bad;
            dist = (size_t)le(in + p, 2); p += 2; break;
        case 2:
            if (p + 3 > src) goto bad;
            dist = (size_t)le(in + p, 3); p += 3; break;
        default:
            goto bad;
        }
        dist += 1;
        if (dist > n || length > dest - n) goto bad;
        for (size_t i = 0; i < length; i++, n++) o[n] = o[n - dist];
    }
    if (n != dest) goto bad;
    *out = o;
    return DXA_OK;
bad:
    free(o);
    return DXA_E_FORMAT;
}

/* ---- names -------------------------------------------------------------------------------- */

/* Bytes in the character starting at s (of n remaining), for the archive's code page. */
static size_t char_bytes(int cp, const uint8_t *s, size_t n) {
    uint8_t c = s[0];
    size_t w = 1;
    switch (cp) {
    case 932: w = ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) ? 2 : 1; break;
    case 936: case 949: case 950: w = (c >= 0x81 && c <= 0xFE) ? 2 : 1; break;
    case 65001: w = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; break;
    default: w = 1; break;
    }
    return w <= n ? w : n;
}

/* A name entry at off in the name table: *pack (in 4-byte units), and where its two forms are. */
static int name_at(const dxa *a, uint64_t off, uint16_t *pack, uint16_t *parity,
                   const uint8_t **upper, const uint8_t **written) {
    if (!fits(off, 4, a->index_size)) return DXA_E_FORMAT;
    const uint8_t *p = a->index + off;
    uint64_t n = le(p, 2) * 4ull;
    if (!fits(off + 4, 2 * n, a->index_size)) return DXA_E_FORMAT;
    *pack = (uint16_t)le(p, 2);
    *parity = (uint16_t)le(p + 2, 2);
    *upper = p + 4;
    *written = p + 4 + n;
    return DXA_OK;
}

static size_t bounded_len(const uint8_t *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

typedef struct {
    uint8_t name[1024];
    uint16_t pack;
    uint16_t parity;
} search;

/* Upper-cases one path component as the library does and computes its parity. */
static int make_search(const dxa *a, const uint8_t *s, size_t n, search *out) {
    if (n + 4 > sizeof out->name) return DXA_E_NAME;
    uint16_t parity = 0;
    size_t i = 0;
    while (i < n) {
        size_t w = char_bytes(a->code_page, s + i, n - i);
        if (w == 1 && s[i] >= 'a' && s[i] <= 'z') {
            out->name[i] = (uint8_t)(s[i] - 'a' + 'A');
        } else {
            memcpy(out->name + i, s + i, w);
        }
        for (size_t j = 0; j < w; j++) parity = (uint16_t)(parity + out->name[i + j]);
        i += w;
    }
    size_t padded = (n + 1 + 3) / 4 * 4;
    memset(out->name + n, 0, padded - n);
    out->pack = (uint16_t)(padded / 4);
    out->parity = parity;
    return DXA_OK;
}

static int dir_at(const dxa *a, uint64_t off, uint64_t *self, uint64_t *parent, uint64_t *count,
                  uint64_t *first) {
    if (!fits(a->dir_table, off, a->index_size) || !fits(a->dir_table + off, DIR_ENTRY, a->index_size))
        return DXA_E_FORMAT;
    const uint8_t *p = a->index + a->dir_table + off;
    *self = le(p, 8);
    *parent = le(p + 8, 8);
    *count = le(p + 16, 8);
    *first = le(p + 24, 8);
    uint64_t table = a->index_size - a->file_table;
    if (*first > table || *count > (table - *first) / FILE_ENTRY) return DXA_E_FORMAT;
    return DXA_OK;
}

typedef struct {
    uint64_t name, attributes, data, size, lz, huffman;
} entry;

static int entry_at(const dxa *a, uint64_t off, entry *e) {
    if (!fits(a->file_table, off, a->index_size) || !fits(a->file_table + off, FILE_ENTRY, a->index_size))
        return DXA_E_FORMAT;
    const uint8_t *p = a->index + a->file_table + off;
    e->name = le(p, 8);
    e->attributes = le(p + 8, 8);
    e->data = le(p + 40, 8);
    e->size = le(p + 48, 8);
    e->lz = le(p + 56, 8);
    e->huffman = le(p + 64, 8);
    return DXA_OK;
}

/* ---- open --------------------------------------------------------------------------------- */

static int known_code_page(uint32_t cp) {
    switch (cp) {
    case 932: case 936: case 949: case 950: case 1252: case 65001:
    case 1200: case 1201: case 12000: case 12001:
        return 1;
    default:
        return 0;
    }
}

int dxa_open(const dxa_source *src, const char *key, size_t key_len, dxa **out) {
    *out = NULL;
    uint8_t h[HEADER_SIZE];
    if (src->size < HEADER_SIZE) return DXA_E_FORMAT;
    int r = read_at(src, 0, h, HEADER_SIZE);
    if (r) return r;
    if (h[0] != 'D' || h[1] != 'X') return DXA_E_FORMAT;
    if (le(h + 2, 2) != 8) return DXA_E_VERSION;

    dxa *a = calloc(1, sizeof *a);
    if (!a) return DXA_E_NOMEM;
    a->src = *src;
    if (key == NULL) {
        key = (const char *)DEFAULT_KEY_STRING;
        key_len = sizeof DEFAULT_KEY_STRING;
    }
    a->key_string_len = key_len > KEY_STRING_MAX ? KEY_STRING_MAX : key_len;
    memcpy(a->key_string, key, a->key_string_len);
    dxa_make_key(a->key_string, a->key_string_len, a->key);

    a->index_size = le(h + 4, 4);
    a->data_start = le(h + 8, 8);
    uint64_t index_start = le(h + 16, 8);
    a->file_table = le(h + 24, 8);
    a->dir_table = le(h + 32, 8);
    uint32_t cp = (uint32_t)le(h + 40, 4);
    uint32_t flags = (uint32_t)le(h + 44, 4);
    a->huffman_kb = h[48];
    a->no_key = (flags & FLAG_NO_KEY) != 0;
    a->code_page = known_code_page(cp) ? (int)cp : 932;

    r = DXA_E_UNSUPPORTED;
    if (a->code_page == 1200 || a->code_page == 1201 || a->code_page == 12000 || a->code_page == 12001)
        goto fail;
    r = DXA_E_TOO_BIG;
    if (a->index_size > INDEX_CAP) goto fail;
    r = DXA_E_FORMAT;
    if (a->index_size == 0 || a->data_start > src->size || index_start >= src->size) goto fail;
    if (a->file_table > a->index_size || a->dir_table > a->index_size) goto fail;
    if (!fits(a->dir_table, DIR_ENTRY, a->index_size)) goto fail;

    if (flags & FLAG_INDEX_PLAIN) {
        if (!fits(index_start, a->index_size, src->size)) goto fail;
        a->index = malloc((size_t)a->index_size);
        r = DXA_E_NOMEM;
        if (!a->index) goto fail;
        r = read_at(src, index_start, a->index, (size_t)a->index_size);
        if (r) goto fail;
        if (!a->no_key) xor_run(a->index, (size_t)a->index_size, 0, a->key);
    } else {
        uint64_t packed_len = src->size - index_start;
        r = DXA_E_TOO_BIG;
        if (packed_len > INDEX_CAP) goto fail;
        uint8_t *packed = malloc((size_t)packed_len);
        r = DXA_E_NOMEM;
        if (!packed) goto fail;
        r = read_at(src, index_start, packed, (size_t)packed_len);
        if (r) { free(packed); goto fail; }
        if (!a->no_key) xor_run(packed, (size_t)packed_len, 0, a->key);
        uint8_t *lz;
        uint64_t lz_len;
        r = huffman_decode(packed, (size_t)packed_len, NONE, INDEX_CAP + INDEX_CAP / 8, &lz, &lz_len);
        free(packed);
        if (r) goto fail;
        r = lz_decode(lz, (size_t)lz_len, a->index_size, &a->index);
        free(lz);
        if (r) goto fail;
    }
    *out = a;
    return DXA_OK;
fail:
    dxa_close(a);
    return r;
}

void dxa_close(dxa *a) {
    if (!a) return;
    free(a->index);
    free(a);
}

int dxa_code_page(const dxa *a) { return a->code_page; }

/* ---- find --------------------------------------------------------------------------------- */

static int lookup(const dxa *a, uint64_t dir, const search *s, uint64_t *found, entry *e) {
    uint64_t self, parent, count, first;
    int r = dir_at(a, dir, &self, &parent, &count, &first);
    if (r) return r;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t off = first + i * FILE_ENTRY;
        if ((r = entry_at(a, off, e))) return r;
        uint16_t pack, parity;
        const uint8_t *upper, *written;
        if ((r = name_at(a, e->name, &pack, &parity, &upper, &written))) return r;
        if (pack == s->pack && parity == s->parity && memcmp(upper, s->name, pack * 4u) == 0) {
            *found = off;
            return DXA_OK;
        }
    }
    return DXA_E_NOT_FOUND;
}

int dxa_find(const dxa *a, const char *path, dxa_file *out) {
    const uint8_t *p = (const uint8_t *)path;
    size_t n = strlen(path);
    if (n == 0 || p[0] == '/' || p[0] == '\\' || (n >= 2 && p[1] == ':')) return DXA_E_NAME;
    uint64_t dir = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && p[j] != '/' && p[j] != '\\') {
            j += char_bytes(a->code_page, p + j, n - j);
        }
        size_t len = j - i;
        if (len == 2 && p[i] == '.' && p[i + 1] == '.') return DXA_E_NAME;
        int last = j >= n;
        if (len == 0 || (len == 1 && p[i] == '.')) {
            if (last) return DXA_E_NAME;
            i = j + 1;
            continue;
        }
        search s;
        int r = make_search(a, p + i, len, &s);
        if (r) return r;
        uint64_t off;
        entry e;
        if ((r = lookup(a, dir, &s, &off, &e))) return r;
        int is_dir = (e.attributes & ATTR_DIRECTORY) != 0;
        if (last) {
            out->entry = off;
            out->directory = dir;
            out->size = is_dir ? 0 : e.size;
            out->is_dir = is_dir;
            return DXA_OK;
        }
        if (!is_dir) return DXA_E_NOT_FOUND;
        dir = e.data;
        i = j + 1;
    }
    return DXA_E_NOT_FOUND;
}

/* ---- read --------------------------------------------------------------------------------- */

/* The per-file key string: archive key string, the file's upper-cased name, then each enclosing
 * directory's upper-cased name up to the root. */
static int file_key(const dxa *a, uint64_t dir, const entry *e, uint8_t key[7]) {
    uint8_t s[FILE_KEY_STRING_MAX];
    size_t n = a->key_string_len;
    memcpy(s, a->key_string, n);
    uint16_t pack, parity;
    const uint8_t *upper, *written;
    int r = name_at(a, e->name, &pack, &parity, &upper, &written);
    if (r) return r;
    size_t m = bounded_len(upper, pack * 4u);
    if (m > FILE_KEY_STRING_MAX - n) return DXA_E_NAME;
    memcpy(s + n, upper, m);
    n += m;
    for (int depth = 0;; depth++) {
        uint64_t self, parent, count, first;
        if (depth > WALK_DEPTH_MAX) return DXA_E_FORMAT;
        if ((r = dir_at(a, dir, &self, &parent, &count, &first))) return r;
        if (parent == NONE) break;
        entry d;
        if ((r = entry_at(a, self, &d))) return r;
        if ((r = name_at(a, d.name, &pack, &parity, &upper, &written))) return r;
        m = bounded_len(upper, pack * 4u);
        if (m > FILE_KEY_STRING_MAX - n) return DXA_E_NAME;
        memcpy(s + n, upper, m);
        n += m;
        dir = parent;
    }
    dxa_make_key(s, n, key);
    return DXA_OK;
}

int dxa_read(const dxa *a, const dxa_file *f, uint8_t **data, size_t *len) {
    *data = NULL;
    *len = 0;
    entry e;
    int r = entry_at(a, f->entry, &e);
    if (r) return r;
    if (e.attributes & ATTR_DIRECTORY) return DXA_E_IS_DIR;
    if (e.size > FILE_CAP) return DXA_E_TOO_BIG;
    int has_lz = e.lz != NONE, has_huffman = e.huffman != NONE;
    uint64_t stage = has_lz ? e.lz : e.size;
    if (stage > FILE_CAP || (has_huffman && e.huffman > FILE_CAP)) return DXA_E_TOO_BIG;
    if (e.size == 0) {
        *data = malloc(1);
        return *data ? DXA_OK : DXA_E_NOMEM;
    }
    if (e.data > a->src.size || a->data_start > a->src.size - e.data) return DXA_E_FORMAT;
    uint64_t base = a->data_start + e.data;

    uint8_t key[7];
    if (!a->no_key && (r = file_key(a, f->directory, &e, key))) return r;

    uint64_t span = (uint64_t)a->huffman_kb * 1024;
    int partial = has_huffman && a->huffman_kb != 0xFF && stage > 2 * span;
    uint64_t raw_len = has_huffman ? e.huffman + (partial ? stage - 2 * span : 0) : stage;
    if (!fits(base, raw_len, a->src.size)) return DXA_E_FORMAT;
    uint8_t *raw = malloc(raw_len ? (size_t)raw_len : 1);
    if (!raw) return DXA_E_NOMEM;
    if ((r = read_at(&a->src, base, raw, (size_t)raw_len))) { free(raw); return r; }
    if (!a->no_key) xor_run(raw, (size_t)raw_len, e.size, key);

    uint8_t *staged = raw;
    if (has_huffman) {
        uint8_t *decoded;
        r = huffman_decode(raw, (size_t)e.huffman, partial ? 2 * span : stage, 0, &decoded, NULL);
        if (r) { free(raw); return r; }
        if (partial) {
            staged = malloc((size_t)stage);
            if (!staged) { free(decoded); free(raw); return DXA_E_NOMEM; }
            memcpy(staged, decoded, (size_t)span);
            memcpy(staged + span, raw + e.huffman, (size_t)(stage - 2 * span));
            memcpy(staged + stage - span, decoded + span, (size_t)span);
            free(decoded);
        } else {
            staged = decoded;
        }
        free(raw);
    }
    if (has_lz) {
        uint8_t *plain;
        r = lz_decode(staged, (size_t)stage, e.size, &plain);
        free(staged);
        if (r) return r;
        staged = plain;
    }
    *data = staged;
    *len = (size_t)e.size;
    return DXA_OK;
}

/* ---- walk --------------------------------------------------------------------------------- */

static int walk_dir(const dxa *a, uint64_t dir, char *path, size_t at, size_t cap, int depth,
                    dxa_visit_fn visit, void *ctx) {
    if (depth > WALK_DEPTH_MAX) return DXA_E_FORMAT;
    uint64_t self, parent, count, first;
    int r = dir_at(a, dir, &self, &parent, &count, &first);
    if (r) return r;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t off = first + i * FILE_ENTRY;
        entry e;
        if ((r = entry_at(a, off, &e))) return r;
        uint16_t pack, parity;
        const uint8_t *upper, *written;
        if ((r = name_at(a, e.name, &pack, &parity, &upper, &written))) return r;
        size_t m = bounded_len(written, pack * 4u);
        if (at + m + 2 > cap) return DXA_E_NAME;
        memcpy(path + at, written, m);
        path[at + m] = 0;
        dxa_file f = {off, dir, 0, (e.attributes & ATTR_DIRECTORY) != 0};
        if (!f.is_dir) f.size = e.size;
        if ((r = visit(ctx, path, &f))) return r;
        if (f.is_dir) {
            path[at + m] = '/';
            if ((r = walk_dir(a, e.data, path, at + m + 1, cap, depth + 1, visit, ctx))) return r;
        }
    }
    return DXA_OK;
}

int dxa_walk(const dxa *a, dxa_visit_fn visit, void *ctx) {
    char path[4096];
    path[0] = 0;
    return walk_dir(a, 0, path, 0, sizeof path, 0, visit, ctx);
}

const char *dxa_error(int code) {
    switch (code) {
    case DXA_OK: return "ok";
    case DXA_E_IO: return "the archive could not be read";
    case DXA_E_FORMAT: return "not a valid DX archive";
    case DXA_E_VERSION: return "a DX archive of an unsupported version";
    case DXA_E_UNSUPPORTED: return "a DX archive feature this reader does not handle";
    case DXA_E_NOMEM: return "out of memory";
    case DXA_E_NOT_FOUND: return "not in the archive";
    case DXA_E_TOO_BIG: return "over the size cap";
    case DXA_E_NAME: return "a refused name";
    case DXA_E_IS_DIR: return "a directory";
    default: return "unknown error";
    }
}
