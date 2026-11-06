/* Tests for the software picture drawing (render.c). Pictures are set up through real 150
 * commands and drawn from synthetic images; no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t *p; size_t n, cap; uint32_t count; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void u8(buf *b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u32(buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void text(buf *b, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; u32(b, n); put(b, s, n); }

static void cmd(buf *b, const int32_t *ints, int nint, const char *s) {
    u8(b, (uint32_t)nint + 1);
    u32(b, 150);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, 0);
    u8(b, s ? 1 : 0);
    if (s) text(b, s);
    u8(b, 0);
    b->count++;
}

static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; }

#define W 16
#define H 12
static uint32_t frame[W * H];
static wolf_vm *vm;

static void run(buf *b) {
    buf whole = {0};
    u8(b, 1); u32(b, 0); u8(b, 0); u8(b, 0); u8(b, 0);
    b->count++;
    u32(&whole, b->count);
    put(&whole, b->p, b->n);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(whole.p, whole.n, &used, &l) == WOLF_LIST_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    free(t);
    wolf_list_free(&l);
    free(whole.p);
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* Images: "quad" is 4x2, each pixel its own colour; "red" is 2x2 opaque red; "half" is 2x2 white at
 * alpha 128; "win" is 6x6 where each 2x2 cell holds its cell number in blue. */
static uint32_t quad_px[8], red_px[4], half_px[4], win_px[36];
static const wolf_image quad = {4, 2, quad_px}, red = {2, 2, red_px}, half = {2, 2, half_px}, win = {6, 6, win_px};

static const wolf_image *load(void *ctx, const char *file) {
    (void)ctx;
    if (!strcmp(file, "quad.png")) return &quad;
    if (!strcmp(file, "red.png")) return &red;
    if (!strcmp(file, "half.png")) return &half;
    if (!strcmp(file, "win.png")) return &win;
    return NULL;
}

static void images(void) {
    for (int i = 0; i < 8; i++) quad_px[i] = 0xFF000000u | (uint32_t)(i + 1) * 0x10u;
    for (int i = 0; i < 4; i++) red_px[i] = 0xFFFF0000u;
    for (int i = 0; i < 4; i++) half_px[i] = 0x80FFFFFFu;
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 6; x++) win_px[y * 6 + x] = 0xFF000000u | (uint32_t)((y / 2) * 3 + x / 2 + 1);
}

static int32_t opt(int op, int type, int blend, int anchor, int zoom) {
    return (int32_t)((uint32_t)op | (uint32_t)type << 4 | (uint32_t)blend << 8 | (uint32_t)anchor << 12 | (uint32_t)zoom << 20);
}

/* Show picture n: file, split dx x dy, piece, opacity, x, y, zoom, angle; anchor and blend. */
static void show(buf *b, int32_t n, const char *file, int dx, int dy, int piece, int opacity, int x, int y, int zoom,
                 int angle, int anchor, int blend) {
    cmd(b, (int32_t[]){opt(0, 0, blend, anchor, 0), n, 0, dx, dy, piece, opacity, x, y, zoom, angle}, 11, file);
}

static uint32_t at(int x, int y) { return frame[y * W + x]; }

static void clear(void) {
    for (int i = 0; i < W * H; i++) frame[i] = 0xFF000000u;
}

static void fresh(void) {
    if (vm) wolf_vm_destroy(vm);
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {10, 0, 10, 10, 10, 0, 1, 0};
    vm = wolf_vm_create(&host, &cfg);
    clear();
}

static const wolf_render_source SRC = {NULL, load};

static void test_place_and_pieces(void) {
    fresh();
    buf b = {0};
    show(&b, 1, "quad.png", 1, 1, 1, 255, 2, 3, 100, 0, 0, 0);       /* top left at 2,3 */
    show(&b, 2, "quad.png", 2, 1, 2, 255, 10, 5, 100, 0, 1, 0);      /* the right half, centred on 10,5 */
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, -1000000, 1000000);
    CHECK(at(2, 3) == (0xFF000000u | 0x10u) && at(5, 4) == (0xFF000000u | 0x80u));
    CHECK(at(1, 3) == 0xFF000000u && at(6, 3) == 0xFF000000u);
    /* piece 2 of a 2x1 split is the right 2x2; centred on 10,5 it covers 9..10, 4..5 */
    CHECK(at(9, 4) == (0xFF000000u | 0x30u) && at(10, 5) == (0xFF000000u | 0x80u));
    CHECK(at(8, 4) == 0xFF000000u && at(11, 4) == 0xFF000000u);
    /* the number range leaves the other out */
    clear();
    wolf_render_pictures(vm, &SRC, frame, W, H, 2, 2);
    CHECK(at(2, 3) == 0xFF000000u && at(9, 4) != 0xFF000000u);
}

static void test_zoom_flip_turn(void) {
    fresh();
    buf b = {0};
    show(&b, 1, "quad.png", 1, 1, 1, 255, 0, 0, 200, 0, 0, 0);       /* 8x4 */
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, 1, 1);
    CHECK(at(0, 0) == (0xFF000000u | 0x10u) && at(1, 1) == (0xFF000000u | 0x10u) && at(7, 3) == (0xFF000000u | 0x80u));
    CHECK(at(8, 0) == 0xFF000000u && at(0, 4) == 0xFF000000u);
    /* a flip: zoom -100 (both axes) about the top-left anchor mirrors up and to the left */
    fresh();
    show(&b, 1, "quad.png", 1, 1, 1, 255, 8, 2, -100, 0, 0, 0);
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, 1, 1);
    CHECK(at(7, 1) == (0xFF000000u | 0x10u) && at(4, 1) == (0xFF000000u | 0x40u) && at(8, 1) == 0xFF000000u);
    CHECK(at(7, 0) == (0xFF000000u | 0x50u) && at(7, 2) == 0xFF000000u);
    /* a quarter turn (900) about the top-left anchor: x goes down, y goes left */
    fresh();
    show(&b, 1, "quad.png", 1, 1, 1, 255, 8, 2, 100, 900, 0, 0);
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, 1, 1);
    CHECK(at(7, 2) == (0xFF000000u | 0x10u)); /* source 0,0 */
    CHECK(at(7, 5) == (0xFF000000u | 0x40u)); /* source 3,0 */
    CHECK(at(6, 2) == (0xFF000000u | 0x50u)); /* source 0,1 */
}

static void test_blends(void) {
    fresh();
    for (int i = 0; i < W * H; i++) frame[i] = 0xFF808080u;
    buf b = {0};
    show(&b, 1, "red.png", 1, 1, 1, 255, 0, 0, 100, 0, 0, 0);     /* normal */
    show(&b, 2, "red.png", 1, 1, 1, 128, 3, 0, 100, 0, 0, 0);     /* half opacity */
    show(&b, 3, "red.png", 1, 1, 1, 255, 6, 0, 100, 0, 0, 1);     /* add */
    show(&b, 4, "red.png", 1, 1, 1, 255, 9, 0, 100, 0, 0, 2);     /* subtract */
    show(&b, 5, "red.png", 1, 1, 1, 255, 12, 0, 100, 0, 0, 3);    /* multiply */
    show(&b, 6, "half.png", 1, 1, 1, 255, 0, 4, 100, 0, 0, 0);    /* the image's own alpha */
    show(&b, 7, "missing.png", 1, 1, 1, 255, 0, 8, 100, 0, 0, 0); /* nothing */
    run(&b);
    /* colour 50% on picture 1 */
    cmd(&b, (int32_t[]){opt(1, 0, 0xF, 0, 4), 1, 0, 0, 0, -1000000, -1000000, -1000000, -1000000, -1000000, -1000000, 0,
        0x2000000, 0, 0, 50, 100, 100}, 18, NULL);
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, -1000000, 1000000);
    CHECK(at(0, 0) == 0xFF800000u);   /* red at 50% red, green and blue 0 */
    CHECK(at(3, 0) == 0xFFC04040u);   /* half way between grey and red */
    CHECK(at(6, 0) == 0xFFFF8080u);   /* add, red clamps */
    CHECK(at(9, 0) == 0xFF000000u + 0x8080u); /* subtract: red goes to 0, others stay */
    CHECK(at(12, 0) == 0xFF800000u);  /* multiply: grey * red */
    CHECK(at(0, 4) == 0xFFC0C0C0u);   /* white at alpha 128 over grey */
    CHECK(at(0, 8) == 0xFF808080u);
}

static void test_window(void) {
    fresh();
    buf b = {0};
    /* a 12x6 window from the 6x6 base: corners 2x2, edges and middle stretched */
    cmd(&b, (int32_t[]){opt(0, 3, 0, 0, 0), 1, 0, 12, 6, 1, 255, 0, 0, 100, 0}, 11, "win.png");
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, 1, 1);
    CHECK((at(0, 0) & 0xFF) == 1 && (at(11, 0) & 0xFF) == 3 && (at(0, 5) & 0xFF) == 7 && (at(11, 5) & 0xFF) == 9);
    CHECK((at(5, 0) & 0xFF) == 2 && (at(5, 3) & 0xFF) == 5 && (at(0, 3) & 0xFF) == 4 && (at(11, 3) & 0xFF) == 6);
    CHECK(at(12, 0) == 0xFF000000u);
    /* smaller than its corners: still drawn, corners shrink */
    fresh();
    cmd(&b, (int32_t[]){opt(0, 3, 0, 0, 0), 1, 0, 3, 3, 1, 255, 0, 0, 100, 0}, 11, "win.png");
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, 1, 1);
    CHECK((at(0, 0) & 0xFF) == 1 && (at(2, 2) & 0xFF) == 9 && at(3, 0) == 0xFF000000u);
}

static void test_off_screen_and_odd(void) {
    fresh();
    buf b = {0};
    show(&b, 1, "quad.png", 1, 1, 1, 255, -100, -100, 100, 0, 0, 0);
    show(&b, 2, "quad.png", 1, 1, 1, 255, 0, 0, 0, 0, 0, 0);          /* zoom 0 */
    show(&b, 3, "quad.png", 9, 9, 99, 255, 0, 6, 100, 0, 0, 0);       /* split past the image, piece past the split */
    show(&b, 4, "quad.png", 1, 1, 1, 255, 14, 10, 100000, 0, 0, 0);   /* huge: clipped */
    cmd(&b, (int32_t[]){opt(0, 2, 0, 0, 0), 5, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, "text");   /* text: not yet */
    cmd(&b, (int32_t[]){opt(0, 0, 0, 0, 0), 6, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, "<SQUARE>");
    run(&b);
    wolf_render_pictures(vm, &SRC, frame, W, H, -1000000, 1000000);
    CHECK(at(0, 0) == 0xFF000000u);
    CHECK(at(0, 6) == (0xFF000000u | 0x10u)); /* the first 1x1 piece of a 4x2 split */
    CHECK(at(15, 11) == (0xFF000000u | 0x10u));
    wolf_render_pictures(vm, NULL, frame, W, H, 0, 1);
    wolf_vm_destroy(vm);
    vm = NULL;
}

int main(void) {
    images();
    test_place_and_pieces();
    test_zoom_flip_turn();
    test_blends();
    test_window();
    test_off_screen_and_odd();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("render: all checks passed");
    return 0;
}
