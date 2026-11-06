/* Tests for pictures (150). Lists are built here from the layout in docs/behaviour/formats.md; no
 * game data is used. */
#include "../src/vm.h"
#include "../src/wolf_cmd.h"

#include <math.h>
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

static void cmd(buf *b, int32_t code, const int32_t *ints, int nint, const char *const *strs) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, 0);
    int ns = 0;
    while (strs && strs[ns]) ns++;
    u8(b, (uint32_t)ns);
    for (int i = 0; i < ns; i++) text(b, strs[i]);
    u8(b, 0);
    b->count++;
}

static int log_count;
static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; log_count++; }

static wolf_vm *vm;

/* Runs the commands in b at once, as one list. */
static void run(buf *b) {
    buf whole = {0};
    cmd(b, 0, NULL, 0, NULL);
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

#define SAME (-1000000)
#define V(x) (2000000 + (x))
#define S(x) (3000000 + (x))

/* The options word: operation, display type, blend, anchor, relative, zoom mode, range. */
static int32_t opt(int op, int type, int blend, int anchor, int relative, int zoom, int range) {
    return (int32_t)((uint32_t)op | (uint32_t)type << 4 | (uint32_t)blend << 8 | (uint32_t)anchor << 12 |
                     (relative ? 0x10000u : 0u) | (uint32_t)zoom << 20 | (range ? 0x1000000u : 0u));
}

static const wolf_picture *pic(int32_t number) {
    for (uint32_t i = 0; i < wolf_vm_picture_count(vm); i++) {
        const wolf_picture *p = wolf_vm_picture_at(vm, i);
        if (p->number == number) return p;
    }
    return NULL;
}

static int near(double a, double b) { return fabs(a - b) < 1e-6; }

static void fresh(void) {
    if (vm) wolf_vm_destroy(vm);
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {100, 0, 10, 10, 10, 0, 1, 0};
    vm = wolf_vm_create(&host, &cfg);
}

static void test_show_and_order(void) {
    fresh();
    buf b = {0};
    /* show 5: a file, split 4x2, piece 3, opacity 200, at 10,20, zoom 150, angle 900, add, centre */
    cmd(&b, 150, (int32_t[]){opt(0, 0, 1, 1, 0, 0, 0), 5, 0, 4, 2, 3, 200, 10, 20, 150, 900}, 11,
        (const char *[]){"Picture/a.png", NULL});
    /* show -3: a text with codes, and 100000: a window from a string variable */
    wolf_set(vm, NULL, V(1), 7);
    wolf_set_str(vm, NULL, S(2), "Picture/win.png");
    cmd(&b, 150, (int32_t[]){opt(0, 2, 0, 0, 0, 0, 0), -3, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11,
        (const char *[]){"HP \\v[1]\\c[2]", NULL});
    cmd(&b, 150, (int32_t[]){opt(0, 4, 0, 0, 0, 0, 0), 100000, 0, 200, 35, 1, 255, V(1), 8, 100, 0, S(2)}, 12, NULL);
    run(&b);
    CHECK(wolf_vm_picture_count(vm) == 3);
    CHECK(wolf_vm_picture_at(vm, 0)->number == -3 && wolf_vm_picture_at(vm, 2)->number == 100000);
    const wolf_picture *p = pic(5);
    CHECK(p && p->kind == WOLF_PIC_FILE && strcmp(p->content, "Picture/a.png") == 0);
    CHECK(p->div_x == 4 && p->div_y == 2 && p->pattern == 3 && p->blend == WOLF_BLEND_ADD && p->anchor == WOLF_ANCHOR_CENTER);
    CHECK(near(p->opacity, 200) && near(p->x, 10) && near(p->y, 20) && near(p->zoom_x, 150) && near(p->zoom_y, 150));
    CHECK(near(p->angle, 900) && near(p->r, 100));
    p = pic(-3);
    CHECK(p && p->kind == WOLF_PIC_TEXT && strcmp(p->content, "HP 7\\c[2]") == 0);
    p = pic(100000);
    CHECK(p && p->kind == WOLF_PIC_WINDOW && strcmp(p->content, "Picture/win.png") == 0 && p->div_x == 200 &&
          p->div_y == 35 && near(p->x, 7));
}

static void test_move_over_time(void) {
    fresh();
    buf b = {0};
    cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 0), 1, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, (const char *[]){"x.png", NULL});
    /* move over 4 frames to 40,80, opacity 55; zoom and angle "same" (zoom mode 4) */
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 0, 4, 0), 1, 4, 0, 0, SAME, 55, 40, 80, SAME, SAME}, 11, NULL);
    run(&b);
    const wolf_picture *p = pic(1);
    CHECK(near(p->x, 0) && near(p->opacity, 255)); /* nothing yet */
    wolf_vm_pictures_tick(vm);
    p = pic(1);
    CHECK(near(p->x, 10) && near(p->y, 20) && near(p->opacity, 255 - 50));
    wolf_vm_pictures_tick(vm);
    wolf_vm_pictures_tick(vm);
    wolf_vm_pictures_tick(vm);
    p = pic(1);
    CHECK(near(p->x, 40) && near(p->y, 80) && near(p->opacity, 55) && near(p->zoom_x, 100));
    wolf_vm_pictures_tick(vm);
    CHECK(near(pic(1)->x, 40)); /* it stays */

    /* relative: adds to where it is; SAME keeps; a move of a picture not shown does nothing */
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 1, 4, 0), 1, 0, 0, 0, SAME, SAME, -50, 0, SAME, SAME}, 11, NULL);
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 0, 4, 0), 2, 0, 0, 0, SAME, SAME, 1, 1, SAME, SAME}, 11, NULL);
    run(&b);
    p = pic(1);
    CHECK(near(p->x, -10) && near(p->y, 80) && near(p->opacity, 55) && p->blend == WOLF_BLEND_NORMAL);
    CHECK(pic(2) == NULL && wolf_vm_picture_count(vm) == 1);

    /* a new picture shown with a process time fades in */
    cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 0), 3, 2, 1, 1, 1, 200, 5, 6, 100, 0}, 11, (const char *[]){"y.png", NULL});
    run(&b);
    CHECK(near(pic(3)->opacity, 0) && near(pic(3)->x, 5));
    wolf_vm_pictures_tick(vm);
    CHECK(near(pic(3)->opacity, 100));
    wolf_vm_pictures_tick(vm);
    CHECK(near(pic(3)->opacity, 200));
}

static void test_extended(void) {
    fresh();
    buf b = {0};
    /* 19 numbers: zoom apart (width 150 at [9], height 50 at [18]), colour set 20/40/60 */
    cmd(&b, 150, (int32_t[]){opt(0, 0, 3, 4, 0, 3, 0), 9, 0, 1, 1, 1, 255, 0, 0, 150, 0, 0, 0x2000000, 0, 0, 20, 40, 60, 50},
        19, (const char *[]){"z.png", NULL});
    run(&b);
    const wolf_picture *p = pic(9);
    CHECK(near(p->zoom_x, 150) && near(p->zoom_y, 50) && near(p->r, 20) && near(p->g, 40) && near(p->b, 60));
    CHECK(p->blend == WOLF_BLEND_MULTIPLY && p->anchor == WOLF_ANCHOR_BOTTOM_RIGHT);
    /* a colour word of 0x1000000 keeps the colour */
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 0, 4, 0), 9, 0, 0, 0, SAME, SAME, SAME, SAME, SAME, SAME, 0, 0x1000000, 0, 0,
        100, 100, 100}, 18, NULL);
    run(&b);
    CHECK(near(pic(9)->r, 20));

    /* a range: show 20..22 at once; a delay of 2 frames on a range move */
    cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 1), 20, 0, 1, 1, 1, 255, 1, 1, 100, 0, 0, 0x2000000, 0, 22, 100, 100, 100},
        18, (const char *[]){"r.png", NULL});
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 0, 4, 1), 20, 0, 0, 0, SAME, SAME, 70, SAME, SAME, SAME, 0, 0x1000000, 2, 22},
        15, NULL);
    run(&b);
    CHECK(pic(20) && pic(21) && pic(22) && !pic(23));
    CHECK(near(pic(21)->x, 1));
    wolf_vm_pictures_tick(vm);
    CHECK(near(pic(21)->x, 1));
    wolf_vm_pictures_tick(vm);
    CHECK(near(pic(20)->x, 70) && near(pic(22)->x, 70));

    /* a delay reset drops what is waiting, in its range only */
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 0, 4, 0), 20, 0, 0, 0, SAME, SAME, 5, SAME, SAME, SAME, 0, 0x1000000, 3}, 14, NULL);
    cmd(&b, 150, (int32_t[]){opt(1, 0, 0xF, 0, 0, 4, 0), 21, 0, 0, 0, SAME, SAME, 6, SAME, SAME, SAME, 0, 0x1000000, 3}, 14, NULL);
    cmd(&b, 150, (int32_t[]){opt(3, 0, 0, 0, 0, 0, 1), 20, 20}, 3, NULL);
    run(&b);
    for (int i = 0; i < 4; i++) wolf_vm_pictures_tick(vm);
    CHECK(near(pic(20)->x, 70) && near(pic(21)->x, 6));
}

static void test_erase(void) {
    fresh();
    buf b = {0};
    for (int n = 1; n <= 4; n++)
        cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 0), n, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, (const char *[]){"e.png", NULL});
    cmd(&b, 150, (int32_t[]){opt(2, 0, 0, 0, 0, 0, 0), 1, 0}, 3, NULL);             /* at once */
    cmd(&b, 150, (int32_t[]){opt(2, 0, 0, 0, 0, 0, 0), 2, 2}, 3, NULL);             /* over 2 frames */
    cmd(&b, 150, (int32_t[]){opt(2, 0, 0, 0, 0, 0, 0), 3, 0, 1}, 4, NULL);          /* after 1 frame */
    cmd(&b, 150, (int32_t[]){opt(2, 0, 0, 0, 0, 0, 1), 4, 0, 0, 0, 9}, 6, NULL);    /* a range */
    cmd(&b, 150, (int32_t[]){opt(2, 0, 0, 0, 0, 0, 0), 50, 0}, 3, NULL);            /* not shown: nothing */
    run(&b);
    CHECK(!pic(1) && pic(2) && pic(3) && !pic(4));
    wolf_vm_pictures_tick(vm);
    CHECK(near(pic(2)->opacity, 127.5) && !pic(3));
    wolf_vm_pictures_tick(vm);
    CHECK(!pic(2) && wolf_vm_picture_count(vm) == 0);
    /* showing again over a fading picture keeps it */
    cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 0), 7, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, (const char *[]){"f.png", NULL});
    cmd(&b, 150, (int32_t[]){opt(2, 0, 0, 0, 0, 0, 0), 7, 5}, 3, NULL);
    cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 0), 7, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, (const char *[]){"g.png", NULL});
    run(&b);
    for (int i = 0; i < 6; i++) wolf_vm_pictures_tick(vm);
    CHECK(pic(7) && strcmp(pic(7)->content, "g.png") == 0 && near(pic(7)->opacity, 255));
}

static void test_refusals(void) {
    fresh();
    buf b = {0};
    log_count = 0;
    cmd(&b, 150, (int32_t[]){opt(0, 0, 0, 0, 0, 0, 0)}, 1, NULL);                    /* too short */
    cmd(&b, 150, (int32_t[]){opt(7, 0, 0, 0, 0, 0, 0), 1}, 2, NULL);                 /* unknown op */
    cmd(&b, 150, (int32_t[]){(int32_t)(0x4000000u), 1, 0, 1, 1, 1, 255, 0, 0, 100, 0}, 11, (const char *[]){"t.png", NULL});
    /* a huge range is cut */
    cmd(&b, 150, (int32_t[]){opt(0, 0, 9, 9, 0, 0, 1), 0, 0, 1, 1, 1, 255, 0, 0, 100, 0, 0, 0x2000000, 0, 999999,
        100, 100, 100}, 18, (const char *[]){"many.png", NULL});
    run(&b);
    CHECK(pic(1) && pic(1)->blend == 0); /* free transform: drawn as a rectangle */
    CHECK(wolf_vm_picture_count(vm) == 10000);
    CHECK(pic(0)->blend == WOLF_BLEND_NORMAL && pic(0)->anchor == WOLF_ANCHOR_TOP_LEFT); /* unknown blend and anchor */
    CHECK(log_count >= 4);
    wolf_vm_destroy(vm);
    vm = NULL;
}

int main(void) {
    test_show_and_order();
    test_move_over_time();
    test_extended();
    test_erase();
    test_refusals();
    if (vm) wolf_vm_destroy(vm);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("picture: all checks passed");
    return 0;
}
