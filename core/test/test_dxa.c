/*
 * Tests for the DXA v8 reader. Archives are built here, from docs/behaviour/archive.md, by a
 * small writer with its own Huffman and LZ encoders. The key vectors were computed separately
 * with Python's zlib.crc32. No official archive is used or needed.
 *
 * The encoders are written from the same description as the reader, so a round trip shows the
 * two agree with each other and with the description; agreement with real archives is the local
 * lane's check (#858).
 */
#include "../src/dxa.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- a growable buffer -------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; } buf;

static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put_le(buf *b, uint64_t v, int n) {
    uint8_t t[8];
    for (int i = 0; i < n; i++) t[i] = (uint8_t)(v >> (8 * i));
    put(b, t, (size_t)n);
}
static void set_le(uint8_t *p, uint64_t v, int n) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* ---- Huffman encoder --------------------------------------------------------------------- */

typedef struct { buf b; int bit; } bitw;

static void w_msb(bitw *w, int n, uint64_t v) {
    for (int i = n - 1; i >= 0; i--) {
        if (w->bit == 0) { uint8_t z = 0; put(&w->b, &z, 1); }
        w->b.p[w->b.n - 1] |= (uint8_t)(((v >> i) & 1u) << (7 - w->bit));
        w->bit = (w->bit + 1) & 7;
    }
}
static int bit_width(uint64_t v) {
    int i = 1;
    while (i < 64 && v >= (1ull << i)) i++;
    return i;
}

static buf huffman_encode(const uint8_t *in, size_t n) {
    uint64_t count[256] = {0};
    for (size_t i = 0; i < n; i++) count[in[i]]++;
    uint64_t max = 0;
    for (int i = 0; i < 256; i++) if (count[i] > max) max = count[i];
    uint16_t weight[256];
    for (int i = 0; i < 256; i++) {
        uint64_t v = count[i];
        if (max > 65535) v = v * 65535 / max + (v != 0);
        if (v > 65535) v = 65535;
        weight[i] = (uint16_t)v;
    }
    bitw w = {{0}, 0};
    w_msb(&w, 6, (uint64_t)bit_width(n) - 1);
    w_msb(&w, bit_width(n), n);
    w_msb(&w, 6, 0);
    w_msb(&w, 1, 0);
    for (int i = 0; i < 256; i++) {
        uint16_t prev = i ? weight[i - 1] : 0;
        int minus = i && weight[i] < prev;
        uint16_t d = i == 0 ? weight[i] : (uint16_t)(minus ? prev - weight[i] : weight[i] - prev);
        int k = 0;
        while ((k + 1) * 2 < bit_width(d)) k++;
        w_msb(&w, 3, (uint64_t)k);
        w_msb(&w, 1, (uint64_t)minus);
        w_msb(&w, (k + 1) * 2, d);
    }
    /* the same tree the reader builds */
    uint64_t wt[511];
    int parent[511], side[511], joined[511] = {0};
    for (int i = 0; i < 256; i++) wt[i] = weight[i];
    for (int node = 256; node < 511; node++) {
        int m1 = -1, m2 = -1;
        for (int i = 0; i < node; i++) {
            if (joined[i]) continue;
            if (m1 == -1 || wt[m1] > wt[i]) { m2 = m1; m1 = i; }
            else if (m2 == -1 || wt[m2] > wt[i]) m2 = i;
        }
        wt[node] = wt[m1] + wt[m2];
        parent[m1] = node; side[m1] = 0;
        parent[m2] = node; side[m2] = 1;
        joined[m1] = joined[m2] = 1;
    }
    /* body, least significant bit first, root to leaf */
    buf body = {0};
    int bit = 0;
    for (size_t i = 0; i < n; i++) {
        int path[600], depth = 0;
        for (int x = in[i]; x != 510; x = parent[x]) path[depth++] = side[x];
        for (int d = depth - 1; d >= 0; d--) {
            if (bit == 0) { uint8_t z = 0; put(&body, &z, 1); }
            body.p[body.n - 1] |= (uint8_t)(path[d] << bit);
            bit = (bit + 1) & 7;
        }
    }
    put(&w.b, body.p, body.n);
    free(body.p);
    return w.b;
}

/* ---- LZ encoder --------------------------------------------------------------------------- */

static buf lz_encode(const uint8_t *in, size_t n) {
    size_t count[256] = {0};
    for (size_t i = 0; i < n; i++) count[in[i]]++;
    int esc = 0;
    for (int i = 1; i < 256; i++) if (count[i] < count[esc]) esc = i;
    buf o = {0};
    put_le(&o, n, 4);
    put_le(&o, 0, 4);
    uint8_t e = (uint8_t)esc;
    put(&o, &e, 1);
    /* last position of each 4-byte hash, plus two fixed candidates so every distance width
     * (one, two and three bytes) is exercised */
    static size_t last[1 << 16];
    for (size_t h = 0; h < (1u << 16); h++) last[h] = SIZE_MAX;
    size_t i = 0;
    while (i < n) {
        size_t best = 0, best_dist = 0;
        size_t cand[3] = {SIZE_MAX, i >= 100000 ? i - 100000 : SIZE_MAX, i >= 1 ? i - 1 : SIZE_MAX};
        uint32_t hash = 0;
        if (i + 4 <= n) {
            hash = (uint32_t)(in[i] | in[i + 1] << 8 | in[i + 2] << 16 | (uint32_t)in[i + 3] << 24) * 2654435761u >> 16;
            cand[0] = last[hash];
        }
        for (int k = 0; k < 3; k++) {
            size_t s = cand[k];
            if (s == SIZE_MAX || s >= i || i - s > 0xFFFFFF) continue;
            size_t l = 0;
            while (i + l < n && l < 8195 && in[s + l] == in[i + l]) l++;
            if (l > best) { best = l; best_dist = i - s; }
        }
        if (i + 4 <= n) last[hash] = i;
        if (best >= 4) {
            size_t len = best - 4, dist = best_dist - 1;
            int width = dist < 256 ? 0 : dist < 65536 ? 1 : 2;
            int c = (int)((len & 31) << 3) | (len > 31 ? 4 : 0) | width;
            uint8_t cb = (uint8_t)(c >= esc ? c + 1 : c);
            put(&o, &e, 1);
            put(&o, &cb, 1);
            if (len > 31) put_le(&o, len >> 5, 1);
            put_le(&o, dist, width + 1);
            i += best;
        } else {
            put(&o, &in[i], 1);
            if (in[i] == e) put(&o, &e, 1);
            i++;
        }
    }
    set_le(o.p + 4, o.n, 4);
    return o;
}

/* ---- archive writer ----------------------------------------------------------------------- */

enum { RAW = 0, LZ = 1, HUFF = 2 };

typedef struct node {
    const char *name;  /* as written */
    const char *upper; /* NULL: ASCII upper-case of name */
    int is_dir;
    const uint8_t *data;
    size_t size;
    int coding;
    struct node *children;
    int n_children;
    /* filled in while writing */
    uint64_t name_off, entry_off, dir_off;
} node;

typedef struct {
    const char *key; /* NULL: default; ignored with no_key */
    size_t key_len;
    int no_key, plain_index;
    uint8_t huffman_kb;
    uint32_t code_page;
} options;

static void upper_of(const node *nd, char *out) {
    if (nd->upper) { strcpy(out, nd->upper); return; }
    size_t i = 0;
    for (; nd->name[i]; i++) {
        char c = nd->name[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out[i] = 0;
}

static void write_names(node *nd, buf *names) {
    char up[256];
    upper_of(nd, up);
    size_t len = strlen(up), pack = (len + 1 + 3) / 4;
    uint16_t parity = 0;
    for (size_t i = 0; i < len; i++) parity = (uint16_t)(parity + (uint8_t)up[i]);
    nd->name_off = names->n;
    put_le(names, pack, 2);
    put_le(names, parity, 2);
    uint8_t block[1024] = {0};
    memcpy(block, up, len);
    put(names, block, pack * 4);
    memset(block, 0, sizeof block);
    memcpy(block, nd->name, strlen(nd->name));
    put(names, block, pack * 4);
    for (int i = 0; i < nd->n_children; i++) write_names(&nd->children[i], names);
}

/* Assigns entry offsets: each directory's children are contiguous. */
static void place(node *dir, uint64_t *next_entry, uint64_t *next_dir) {
    for (int i = 0; i < dir->n_children; i++) {
        dir->children[i].entry_off = *next_entry;
        *next_entry += 72;
    }
    for (int i = 0; i < dir->n_children; i++) {
        node *c = &dir->children[i];
        if (c->is_dir) {
            c->dir_off = *next_dir;
            *next_dir += 32;
            place(c, next_entry, next_dir);
        }
    }
}

static void key_for(const options *o, const char *const *parts, int n_parts, uint8_t key[7]) {
    uint8_t s[4096];
    size_t n = 0;
    const char *k = o->key ? o->key : "DXBDXARC";
    size_t kl = o->key ? o->key_len : 8;
    if (kl > 63) kl = 63;
    memcpy(s, k, kl);
    n = kl;
    for (int i = n_parts - 1; i >= 0; i--) { /* file first, then directories outward */
        size_t l = strlen(parts[i]);
        memcpy(s + n, parts[i], l);
        n += l;
    }
    dxa_make_key(s, n, key);
}

static void xor_with(uint8_t *p, size_t n, uint64_t start, const uint8_t key[7]) {
    for (size_t i = 0; i < n; i++) p[i] ^= key[(start + i) % 7];
}

/* Writes file data and both tables, depth first. parts holds upper-cased names from the root. */
static void write_tree(node *dir, const options *o, buf *data, buf *files, buf *dirs, const char **parts,
                       int depth, uint64_t parent_dir) {
    /* this directory's entry */
    uint8_t de[32];
    set_le(de, depth == 0 ? UINT64_MAX : dir->entry_off, 8);
    set_le(de + 8, depth == 0 ? UINT64_MAX : parent_dir, 8);
    set_le(de + 16, (uint64_t)dir->n_children, 8);
    set_le(de + 24, dir->n_children ? dir->children[0].entry_off : 0, 8);
    memcpy(dirs->p + dir->dir_off, de, 32);

    for (int i = 0; i < dir->n_children; i++) {
        node *c = &dir->children[i];
        uint8_t fe[72] = {0};
        set_le(fe, c->name_off, 8);
        set_le(fe + 8, c->is_dir ? 0x10 : 0x20, 8);
        uint64_t lz_size = UINT64_MAX, huff_size = UINT64_MAX;
        if (c->is_dir) {
            set_le(fe + 40, c->dir_off, 8);
        } else {
            char up[256];
            upper_of(c, up);
            parts[depth] = up;
            uint8_t key[7];
            key_for(o, parts, depth + 1, key);
            /* stage */
            buf stage = {0};
            if (c->coding & LZ) {
                stage = lz_encode(c->data, c->size);
                lz_size = stage.n;
            } else {
                put(&stage, c->data, c->size);
            }
            buf stored = {0};
            if (c->coding & HUFF) {
                uint64_t span = (uint64_t)o->huffman_kb * 1024;
                if (o->huffman_kb != 0xFF && stage.n > 2 * span) {
                    buf ends = {0};
                    put(&ends, stage.p, span);
                    put(&ends, stage.p + stage.n - span, span);
                    buf h = huffman_encode(ends.p, ends.n);
                    huff_size = h.n;
                    put(&stored, h.p, h.n);
                    put(&stored, stage.p + span, stage.n - 2 * span);
                    free(h.p);
                    free(ends.p);
                } else {
                    buf h = huffman_encode(stage.p, stage.n);
                    huff_size = h.n;
                    put(&stored, h.p, h.n);
                    free(h.p);
                }
            } else {
                put(&stored, stage.p, stage.n);
            }
            if (!o->no_key) xor_with(stored.p, stored.n, c->size, key);
            set_le(fe + 40, data->n, 8);
            put(data, stored.p, stored.n);
            free(stored.p);
            free(stage.p);
            set_le(fe + 48, c->size, 8);
        }
        set_le(fe + 56, lz_size, 8);
        set_le(fe + 64, huff_size, 8);
        memcpy(files->p + c->entry_off, fe, 72);
    }
    for (int i = 0; i < dir->n_children; i++) {
        node *c = &dir->children[i];
        if (c->is_dir) {
            char up[256];
            upper_of(c, up);
            /* up lives on this frame, so it outlives the recursion below */
            parts[depth] = up;
            write_tree(c, o, data, files, dirs, parts, depth + 1, dir->dir_off);
        }
    }
}

static buf build(node *root, const options *o) {
    buf names = {0};
    for (int i = 0; i < root->n_children; i++) write_names(&root->children[i], &names);
    while (names.n % 4) put_le(&names, 0, 1);
    uint64_t next_entry = 0, next_dir = 32;
    root->dir_off = 0;
    place(root, &next_entry, &next_dir);
    buf files = {0}, dirs = {0}, data = {0};
    put(&files, NULL, 0);
    files.p = calloc(1, next_entry + 1);
    files.n = next_entry;
    dirs.p = calloc(1, next_dir);
    dirs.n = next_dir;
    const char *parts[64];
    write_tree(root, o, &data, &files, &dirs, parts, 0, 0);

    buf index = {0};
    put(&index, names.p, names.n);
    uint64_t file_table = index.n;
    put(&index, files.p, files.n);
    uint64_t dir_table = index.n;
    put(&index, dirs.p, dirs.n);

    uint8_t archive_key[7];
    const char *k = o->key ? o->key : "DXBDXARC";
    size_t kl = o->key ? (o->key_len > 63 ? 63 : o->key_len) : 8;
    dxa_make_key((const uint8_t *)k, kl, archive_key);

    buf out = {0};
    uint8_t h[64] = {0};
    put(&out, h, 64);
    uint64_t data_start = out.n;
    put(&out, data.p, data.n);
    uint64_t index_start = out.n;
    if (o->plain_index) {
        buf x = {0};
        put(&x, index.p, index.n);
        if (!o->no_key) xor_with(x.p, x.n, 0, archive_key);
        put(&out, x.p, x.n);
        free(x.p);
    } else {
        buf l = lz_encode(index.p, index.n);
        buf hz = huffman_encode(l.p, l.n);
        if (!o->no_key) xor_with(hz.p, hz.n, 0, archive_key);
        put(&out, hz.p, hz.n);
        free(l.p);
        free(hz.p);
    }
    out.p[0] = 'D'; out.p[1] = 'X';
    set_le(out.p + 2, 8, 2);
    set_le(out.p + 4, index.n, 4);
    set_le(out.p + 8, data_start, 8);
    set_le(out.p + 16, index_start, 8);
    set_le(out.p + 24, file_table, 8);
    set_le(out.p + 32, dir_table, 8);
    set_le(out.p + 40, o->code_page ? o->code_page : 932, 4);
    set_le(out.p + 44, (o->no_key ? 1u : 0u) | (o->plain_index ? 2u : 0u), 4);
    out.p[48] = o->huffman_kb;
    free(names.p); free(files.p); free(dirs.p); free(data.p); free(index.p);
    return out;
}

/* ---- a memory source ------------------------------------------------------------------------ */

typedef struct { const uint8_t *p; size_t n; } mem;
static int mem_read(void *ctx, uint64_t off, void *b, size_t len) {
    mem *m = ctx;
    if (off > m->n || len > m->n - off) return -1;
    memcpy(b, m->p + off, len);
    return 0;
}
static int open_mem(const buf *b, mem *m, const options *o, dxa **a) {
    m->p = b->p;
    m->n = b->n;
    dxa_source s = {mem_read, m, b->n};
    return dxa_open(&s, o->key, o->key_len, a);
}

/* ---- content -------------------------------------------------------------------------------- */

static uint8_t text[9000], noise[5000], big[300000], sjis_data[64];
static uint32_t rng = 12345;
static uint32_t next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static void make_content(void) {
    const char *words[] = {"Wolf ", "event ", "common ", "map ", "switch ", "variable ", "\n"};
    size_t n = 0;
    while (n < sizeof text) {
        const char *w = words[next_rand() % 7];
        for (size_t i = 0; w[i] && n < sizeof text; i++) text[n++] = (uint8_t)w[i];
    }
    for (size_t i = 0; i < sizeof noise; i++) noise[i] = (uint8_t)next_rand();
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = i >= 150000 ? big[i - 100000] : (uint8_t)(i % 251 == 0 ? next_rand() : text[i % sizeof text]);
    for (size_t i = 0; i < sizeof sjis_data; i++) sjis_data[i] = (uint8_t)i;
}

/* The library upper-cases ASCII letters only, a character at a time: under Shift-JIS, 0x83 0x61
 * is one character and stays as it is; under UTF-8, 0x83 is a byte of its own and the 0x61 after
 * it is an 'a'. */
static uint32_t tree_code_page = 932;

static node *tree(int coding_a, int coding_b) {
    static node basic_children[3], data_children[3], root_children[3], root;
    basic_children[0] = (node){"Game.dat", NULL, 0, text, sizeof text, coding_a, NULL, 0, 0, 0, 0};
    basic_children[1] = (node){"CommonEvent.dat", NULL, 0, big, sizeof big, coding_b, NULL, 0, 0, 0, 0};
    basic_children[2] = (node){"empty.dat", NULL, 0, text, 0, RAW, NULL, 0, 0, 0, 0};
    data_children[0] = (node){"BasicData", NULL, 1, NULL, 0, 0, basic_children, 3, 0, 0, 0};
    data_children[1] = (node){"Map001.mps", NULL, 0, noise, sizeof noise, coding_b, NULL, 0, 0, 0, 0};
    data_children[2] = (node){"\x83\x61x.txt", tree_code_page == 65001 ? "\x83" "AX.TXT" : "\x83\x61X.TXT", 0, sjis_data, sizeof sjis_data, RAW, NULL, 0, 0, 0, 0};
    root_children[0] = (node){"readme.txt", NULL, 0, text, 100, coding_a, NULL, 0, 0, 0, 0};
    root_children[1] = (node){"Data", NULL, 1, NULL, 0, 0, data_children, 3, 0, 0, 0};
    root_children[2] = (node){"a", NULL, 0, text, 3, coding_a, NULL, 0, 0, 0, 0};
    root = (node){"", NULL, 1, NULL, 0, 0, root_children, 3, 0, 0, 0};
    return &root;
}

static int read_eq(dxa *a, const char *path, const uint8_t *want, size_t n) {
    dxa_file f;
    int r = dxa_find(a, path, &f);
    if (r) { fprintf(stderr, "find %s: %s\n", path, dxa_error(r)); return 0; }
    uint8_t *d;
    size_t len;
    r = dxa_read(a, &f, &d, &len);
    if (r) { fprintf(stderr, "read %s: %s\n", path, dxa_error(r)); return 0; }
    int ok = len == n && memcmp(d, want, n) == 0;
    if (!ok) fprintf(stderr, "read %s: wrong bytes (%zu, want %zu)\n", path, len, n);
    free(d);
    return ok;
}

static int count_visit(void *ctx, const char *path, const dxa_file *f) {
    (void)path; (void)f;
    (*(int *)ctx)++;
    return 0;
}

static void round_trip(const options *o, int coding_a, int coding_b) {
    tree_code_page = o->code_page;
    buf b = build(tree(coding_a, coding_b), o);
    mem m;
    dxa *a;
    int r = open_mem(&b, &m, o, &a);
    CHECK(r == DXA_OK);
    if (r) { fprintf(stderr, "open: %s\n", dxa_error(r)); free(b.p); return; }
    CHECK(read_eq(a, "readme.txt", text, 100));
    CHECK(read_eq(a, "a", text, 3));
    CHECK(read_eq(a, "Data/BasicData/Game.dat", text, sizeof text));
    CHECK(read_eq(a, "data\\basicdata\\commonevent.DAT", big, sizeof big));
    CHECK(read_eq(a, "Data/BasicData/empty.dat", text, 0));
    CHECK(read_eq(a, "DATA/map001.mps", noise, sizeof noise));
    CHECK(read_eq(a, "Data/\x83\x61x.txt", sjis_data, sizeof sjis_data));
    dxa_file f;
    /* a Shift-JIS trail byte 0x61 is not a letter and must not be upper-cased */
    CHECK(dxa_find(a, "Data/\x83\x41X.TXT", &f) == (o->code_page == 65001 ? DXA_OK : DXA_E_NOT_FOUND));
    CHECK(dxa_find(a, "Data/nothing", &f) == DXA_E_NOT_FOUND);
    CHECK(dxa_find(a, "readme.txt/x", &f) == DXA_E_NOT_FOUND);
    CHECK(dxa_find(a, "../readme.txt", &f) == DXA_E_NAME);
    CHECK(dxa_find(a, "Data/../readme.txt", &f) == DXA_E_NAME);
    CHECK(dxa_find(a, "/readme.txt", &f) == DXA_E_NAME);
    CHECK(dxa_find(a, "C:\\readme.txt", &f) == DXA_E_NAME);
    CHECK(dxa_find(a, "Data/BasicData", &f) == DXA_OK && f.is_dir);
    uint8_t *d;
    size_t len;
    CHECK(dxa_read(a, &f, &d, &len) == DXA_E_IS_DIR);
    int visited = 0;
    CHECK(dxa_walk(a, count_visit, &visited) == DXA_OK);
    CHECK(visited == 9);
    dxa_close(a);
    free(b.p);
}

/* Every mutation of a valid archive must fail cleanly or read; never crash (run under ASan). */
static int any_visit(void *ctx, const char *path, const dxa_file *f) {
    dxa *a = ctx;
    (void)path;
    if (!f->is_dir) {
        uint8_t *d;
        size_t len;
        if (dxa_read(a, f, &d, &len) == DXA_OK) free(d);
    }
    return 0;
}

static void fuzz(const options *o, int coding_a, int coding_b, int rounds) {
    tree_code_page = o->code_page;
    buf b = build(tree(coding_a, coding_b), o);
    for (int round = 0; round < rounds; round++) {
        buf c = {0};
        put(&c, b.p, b.n);
        int edits = 1 + (int)(next_rand() % 8);
        for (int e = 0; e < edits; e++) {
            size_t at = next_rand() % c.n;
            /* bias towards the header and the index, where the structure is */
            if (next_rand() % 2) at = next_rand() % 64;
            c.p[at] ^= (uint8_t)(1u << (next_rand() % 8));
        }
        if (next_rand() % 8 == 0) c.n = next_rand() % c.n;
        mem m;
        dxa *a;
        if (open_mem(&c, &m, o, &a) == DXA_OK) {
            dxa_walk(a, any_visit, a);
            dxa_file f;
            if (dxa_find(a, "Data/BasicData/Game.dat", &f) == DXA_OK) {
                uint8_t *d;
                size_t len;
                if (dxa_read(a, &f, &d, &len) == DXA_OK) free(d);
            }
            dxa_close(a);
        }
        free(c.p);
    }
    free(b.p);
}

#ifndef FUZZ_ROUNDS
#define FUZZ_ROUNDS 300
#endif

int main(void) {
    make_content();

    /* the CRC is standard CRC-32 */
    CHECK(dxa_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    /* key vectors from Python: zlib.crc32 of the even and odd bytes */
    struct { const char *s; size_t n; const char *hex; } kv[] = {
        {"DXBDXARC", 8, "5cae2bb123b3d9"},
        {"", 0, "5cae2bb123b3d9"},
        {"ab", 2, "a0d81ff20fbf4d"},
        {"WolfRPGEditorKey", 16, "f9002f1aada0e3"},
        {"abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0", 63, "f5fafcf5a3958c"},
    };
    for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) {
        uint8_t k[7];
        char hex[15];
        dxa_make_key((const uint8_t *)kv[i].s, kv[i].n, k);
        for (int j = 0; j < 7; j++) sprintf(hex + 2 * j, "%02x", k[j]);
        CHECK(strcmp(hex, kv[i].hex) == 0);
    }

    static const char long_key[] = "a key string longer than sixty-three bytes is cut to sixty-three, as the library does";
    options opts[] = {
        {"WolfRPGEditorKey", 16, 0, 0, 0xFF, 932},
        {"WolfRPGEditorKey", 16, 0, 1, 0xFF, 932},
        {"ab", 2, 0, 0, 1, 932},
        {NULL, 0, 0, 0, 1, 65001},
        {NULL, 0, 1, 1, 0xFF, 932},
        {long_key, sizeof long_key - 1, 0, 0, 2, 932},
        {"k", 1, 0, 1, 0, 9999}, /* an unknown code page means 932 */
    };
    int codings[][2] = {{RAW, RAW}, {LZ, LZ}, {HUFF, HUFF}, {LZ | HUFF, LZ | HUFF}, {RAW, LZ | HUFF}, {HUFF, LZ}};
    for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++)
        for (size_t j = 0; j < sizeof codings / sizeof codings[0]; j++) {
            fprintf(stderr, "round trip: options %zu, codings %d/%d\n", i, codings[j][0], codings[j][1]);
            round_trip(&opts[i], codings[j][0], codings[j][1]);
        }

    /* the code page is reported after the whitelist */
    {
        buf b = build(tree(RAW, RAW), &opts[6]);
        mem m;
        dxa *a;
        CHECK(open_mem(&b, &m, &opts[6], &a) == DXA_OK);
        if (a) { CHECK(dxa_code_page(a) == 932); dxa_close(a); }
        free(b.p);
    }

    /* a wrong key does not read the archive and does not crash */
    {
        buf b = build(tree(LZ | HUFF, LZ), &opts[0]);
        mem m;
        dxa *a;
        options wrong = opts[0];
        wrong.key = "WolfRPGEditorKez";
        int r = open_mem(&b, &m, &wrong, &a);
        CHECK(r != DXA_OK);
        if (r == DXA_OK) dxa_close(a);
        free(b.p);
    }

    /* other versions and non-archives are refused */
    {
        buf b = build(tree(RAW, RAW), &opts[1]);
        mem m;
        dxa *a;
        b.p[2] = 6;
        CHECK(open_mem(&b, &m, &opts[1], &a) == DXA_E_VERSION);
        b.p[0] = 'X';
        CHECK(open_mem(&b, &m, &opts[1], &a) == DXA_E_FORMAT);
        b.n = 10;
        CHECK(open_mem(&b, &m, &opts[1], &a) == DXA_E_FORMAT);
        free(b.p);
    }

    /* wide code pages are reported as unsupported */
    {
        options wide = opts[1];
        wide.code_page = 1200;
        buf b = build(tree(RAW, RAW), &wide);
        mem m;
        dxa *a;
        CHECK(open_mem(&b, &m, &wide, &a) == DXA_E_UNSUPPORTED);
        free(b.p);
    }

    fuzz(&opts[0], LZ | HUFF, LZ | HUFF, FUZZ_ROUNDS);
    fuzz(&opts[1], LZ, HUFF, FUZZ_ROUNDS);
    fuzz(&opts[2], HUFF, LZ | HUFF, FUZZ_ROUNDS);
    fuzz(&opts[4], RAW, LZ, FUZZ_ROUNDS);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    fprintf(stderr, "all DXA checks passed\n");
    return 0;
}
