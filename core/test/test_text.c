/* Tests for text layout (textlay.c) and text pictures (render.c), with a fake font source and a
 * synthetic system database; no game data or real font is used. */
#include "../src/vm.h"
#include "../src/vm_internal.h"
#include "../src/wolf_cmd.h"
#include "../src/wolf_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void u8(buf *b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u32(buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void text(buf *b, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; u32(b, n); put(b, s, n); }

/* ---- a fake font: a glyph of size s is s/2 wide and s tall, ascent 3s/4, advance s/2 + 1 ---- */

typedef struct {
    uint32_t font, size, cp;
    char key[48];
    wolf_image img;
    uint32_t *px;
    wolf_glyph g;
} fake;
static fake glyphs[256];
static int nglyphs;
static uint32_t requests[64];
static int nrequests;
static int has_question = 1;

static const wolf_glyph *glyph(void *ctx, uint32_t font, uint32_t size, uint32_t cp) {
    (void)ctx;
    if (nrequests < 64) requests[nrequests++] = cp;
    if (cp == 0x1F600) return NULL;              /* a character no font has */
    if (cp == '?' && !has_question) return NULL;
    for (int i = 0; i < nglyphs; i++)
        if (glyphs[i].font == font && glyphs[i].size == size && glyphs[i].cp == cp) return &glyphs[i].g;
    fake *f = &glyphs[nglyphs++];
    f->font = font;
    f->size = size;
    f->cp = cp;
    snprintf(f->key, sizeof f->key, "glyph:%u:%u:%u", font, size, cp);
    uint32_t w = size / 2 ? size / 2 : 1;
    f->px = malloc((size_t)w * size * 4);
    for (uint32_t i = 0; i < w * size; i++) f->px[i] = 0xFFFFFFFFu;
    f->img.width = w;
    f->img.height = size;
    f->img.argb = f->px;
    f->g.key = f->key;
    f->g.image = cp == ' ' ? NULL : &f->img;
    f->g.left = 0;
    f->g.top = (int32_t)(size * 3 / 4);
    f->g.advance = (int32_t)(size / 2 + 1);
    f->g.ascent = (int32_t)(size * 3 / 4);
    return &f->g;
}

static const wolf_image *no_image(void *ctx, const char *file) { (void)ctx; (void)file; return NULL; }
static const wolf_render_source SRC = {NULL, no_image, glyph};
static const wolf_render_source NO_FONT = {NULL, no_image, NULL};

typedef struct { uint32_t cp; double x, y; uint32_t rgb; } placed;
static placed out[64];
static int nout;
static void collect(void *ctx, const wolf_glyph *g, double x, double y, uint32_t rgb) {
    (void)ctx;
    if (nout < 64) {
        for (int i = 0; i < nglyphs; i++) if (&glyphs[i].g == g) out[nout].cp = glyphs[i].cp;
        out[nout].x = x;
        out[nout].y = y;
        out[nout].rgb = rgb;
    }
    nout++;
}

static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; }

/* A system database whose type 12 is the colour table: row 0 grey, row 1 red, row 2 green. */
static int make_sysdb(wolf_db *db) {
    buf p = {0}, d = {0};
    u32(&p, 13);
    for (int t = 0; t < 12; t++) {
        text(&p, "");
        u32(&p, 0); u32(&p, 0); text(&p, "");
        u32(&p, 0); u32(&p, 0); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    }
    text(&p, "colours");
    u32(&p, 3); text(&p, "R"); text(&p, "G"); text(&p, "B");
    u32(&p, 3); text(&p, "grey"); text(&p, "red"); text(&p, "green");
    text(&p, "");
    u32(&p, 3); u8(&p, 0); u8(&p, 0); u8(&p, 0);
    u32(&p, 3); text(&p, ""); text(&p, ""); text(&p, "");
    u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    static const uint8_t head[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};
    put(&d, head, sizeof head);
    u8(&d, 0xC1);
    u32(&d, 13);
    for (int t = 0; t < 12; t++) { u32(&d, 0xFFFFFFFEu); u32(&d, 0); u32(&d, 0); u32(&d, 0); }
    u32(&d, 0xFFFFFFFEu); u32(&d, 0);
    u32(&d, 3); u32(&d, 1000); u32(&d, 1001); u32(&d, 1002);
    u32(&d, 3);
    u32(&d, 128); u32(&d, 128); u32(&d, 128);
    u32(&d, 255); u32(&d, 0); u32(&d, 0);
    u32(&d, 0); u32(&d, 255); u32(&d, 0);
    u8(&d, 0xC1);
    int r = wolf_db_load(p.p, p.n, d.p, d.n, db);
    free(p.p);
    free(d.p);
    return r;
}

static wolf_vm *vm;
static wolf_db sys;

static void lay(const char *s, double *w, double *h) {
    nout = 0;
    nrequests = 0;
    wolf_text_layout(vm, &SRC, s, 20, collect, NULL, w, h);
}

static void test_layout(void) {
    double w, h;
    /* size 20: glyphs 10x20, advance 11, ascent 15; lines 20 apart plus 4 spacing */
    lay("AB\nC", &w, &h);
    CHECK(nout == 3);
    CHECK(out[0].cp == 'A' && out[0].x == 0 && out[0].y == 0);
    CHECK(out[1].cp == 'B' && out[1].x == 11 && out[1].y == 0);
    CHECK(out[2].cp == 'C' && out[2].x == 0 && out[2].y == 24);
    CHECK(w == 22 && h == 44);
    CHECK(out[0].rgb == 0x808080u); /* the colour table's first row */
    /* an empty line takes no spacing (the manual's note on string pictures) */
    lay("A\n\nB", &w, &h);
    CHECK(out[1].y == 24 + 20);
    /* colours, sizes, a line holding a bigger size sits lower on a shared baseline */
    lay("\\c[1]A\\c[2]B\\c[9]C", NULL, NULL);
    CHECK(out[0].rgb == 0xFF0000u && out[1].rgb == 0x00FF00u && out[2].rgb == 0xFFFFFFu);
    lay("a\\f[40]B\\f+[-20]c\nd", &w, &h);
    /* line 1: sizes 20, 40, 20; baseline at ascent 30; line height 40 */
    CHECK(out[0].y == 30 - 15 && out[1].y == 0 && out[2].y == 30 - 15);
    CHECK(out[1].x == 11 && out[2].x == 11 + 21);
    CHECK(out[3].y == 44);
    /* \space, \ax, \ay, \mx, \my, \- */
    lay("\\space[0]A\nB", NULL, NULL);
    CHECK(out[1].y == 20);
    lay("A\\ax[50]B\\mx[3]C\\my[2]D\\-[1]EF", NULL, NULL);
    CHECK(out[1].x == 50 && out[2].x == 64 && out[3].x == 72 && out[3].y == 2);
    CHECK(out[4].x == 83 && out[5].x == 93);
    lay("A\\ay[100]B", NULL, NULL);
    CHECK(out[0].y == 0 && out[1].y == 100);
    /* ruby: the text drawn, the ruby skipped */
    lay("\\r[XY,abc]Z", NULL, NULL);
    CHECK(nout == 3 && out[0].cp == 'X' && out[1].cp == 'Y' && out[2].cp == 'Z');
    /* codes not built are skipped; a doubled backslash draws one; a space has no image */
    lay("\\E\\i[3]\\!A\\\\ B\\A+C", NULL, NULL);
    CHECK(nout == 4 && out[0].cp == 'A' && out[1].cp == '\\' && out[2].cp == 'B' && out[3].cp == 'C');
    CHECK(out[2].x == 33);
    /* UTF-8: a three-byte character and a four-byte one no font has ('?' instead) */
    lay("\xE3\x81\x82\xF0\x9F\x98\x80", NULL, NULL);
    CHECK(nout == 2 && out[0].cp == 0x3042 && out[1].cp == '?');
    has_question = 0;
    lay("\xF0\x9F\x98\x80", NULL, NULL);
    CHECK(nout == 0);
    has_question = 1;
    /* no font source: nothing laid out */
    nout = 0;
    wolf_text_layout(vm, &NO_FONT, "AB", 20, collect, NULL, &w, &h);
    CHECK(nout == 0 && w == 0 && h == 0);
}

static void test_shift_jis(void) {
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {10, 0, 10, 10, 10, 0, 1, 1};
    wolf_vm *j = wolf_vm_create(&host, &cfg);
    nout = 0;
    nrequests = 0;
    /* "A", a double-byte character (not convertible yet), half-width katakana 0xB1 */
    wolf_text_layout(j, &SRC, "A\x82\xA0\xB1", 20, collect, NULL, NULL, NULL);
    CHECK(nout == 3 && out[0].cp == 'A' && out[1].cp == 0xFFFD && out[2].cp == 0xFF71);
    wolf_vm_destroy(j);
}

typedef struct { int n; wolf_quad q[8]; } quads;
static void take(void *ctx, const wolf_quad *q) {
    quads *qs = ctx;
    if (qs->n < 8) qs->q[qs->n] = *q;
    qs->n++;
}

static void test_text_picture(void) {
    /* a text picture at the default size 18 (glyphs 9x18, advance 10), centred on 100,50: the
     * text is 20x18, so it starts at 90,41 */
    buf b = {0};
    u32(&b, 3);
    int32_t opt = (int32_t)(0u | 2u << 4 | 1u << 12);
    int32_t ints[] = {opt, 1, 0, 1, 1, 1, 255, 100, 50, 100, 0, 0, 0x2000000, 0, 0, 50, 100, 100};
    u8(&b, 19); u32(&b, 150);
    for (int i = 0; i < 18; i++) u32(&b, (uint32_t)ints[i]);
    u8(&b, 0); u8(&b, 1); text(&b, "\\c[1]A\\c[0]B");
    u8(&b, 0);
    u8(&b, 1); u32(&b, 0); u8(&b, 0); u8(&b, 0); u8(&b, 0);
    /* the list's count was written as 3 for room; fix it to 2 */
    b.p[0] = 2;
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    quads qs = {0};
    wolf_picture_quads(vm, &SRC, 0, 10, take, &qs);
    CHECK(qs.n == 2);
    CHECK(qs.q[0].x[0] == 90 && qs.q[0].y[0] == 41 && qs.q[0].x[2] == 99 && qs.q[0].y[2] == 59);
    CHECK(qs.q[1].x[0] == 100);
    CHECK(!strcmp(qs.q[0].file, "glyph:0:18:65"));
    /* the text colour times the picture's colour (red 50%, green 100%, blue 100%) */
    CHECK(qs.q[0].r == 0.5 && qs.q[0].g == 0 && qs.q[0].b == 0);
    CHECK(qs.q[1].r > 0.25 && qs.q[1].r < 0.26 && qs.q[1].g > 0.50 && qs.q[1].g < 0.51);
    /* without a font source, a text picture draws nothing */
    qs.n = 0;
    wolf_picture_quads(vm, &NO_FONT, 0, 10, take, &qs);
    CHECK(qs.n == 0);
    free(t);
    wolf_list_free(&l);
    free(b.p);
}

int main(void) {
    CHECK(make_sysdb(&sys) == WOLF_READ_OK);
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {10, 0, 10, 10, 10, 0, 1, 0};
    vm = wolf_vm_create(&host, &cfg);
    wolf_vm_set_databases(vm, NULL, NULL, &sys);
    test_layout();
    test_shift_jis();
    test_text_picture();
    wolf_vm_destroy(vm);
    wolf_db_free(&sys);
    for (int i = 0; i < nglyphs; i++) free(glyphs[i].px);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("text: all checks passed");
    return 0;
}
