/* Tests for the built-in message window and choice list (msgwin.c), with a fake font. */
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
static void cmd(buf *b, int32_t code, const int32_t *ints, int nint, const char *const *strs, int indent) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, (uint32_t)indent);
    int ns = 0;
    while (strs && strs[ns]) ns++;
    u8(b, (uint32_t)ns);
    for (int i = 0; i < ns; i++) text(b, strs[i]);
    u8(b, 0);
    b->count++;
}
static wolf_list finish(buf *b) {
    buf whole = {0};
    cmd(b, 0, NULL, 0, NULL, 0);
    u32(&whole, b->count);
    put(&whole, b->p, b->n);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(whole.p, whole.n, &used, &l) == WOLF_LIST_OK);
    free(whole.p);
    free(b->p);
    memset(b, 0, sizeof *b);
    return l;
}

/* a fake font: every glyph a 4x8 white block, advance 5 */
static uint32_t white[32];
static const wolf_image block = {4, 8, white};
static wolf_glyph g = {"g", &block, 0, 6, 5, 6};
static const wolf_glyph *glyph(void *ctx, uint32_t font, uint32_t size, uint32_t cp) {
    (void)ctx; (void)font; (void)size; (void)cp;
    return &g;
}
static const wolf_image *no_image(void *ctx, const char *file) { (void)ctx; (void)file; return NULL; }
static const wolf_render_source SRC = {NULL, no_image, glyph};

static int nquads;
static double first_x, first_y;
static void count(void *ctx, const wolf_quad *q) {
    (void)ctx;
    if (nquads == 0) { first_x = q->x[0]; first_y = q->y[0]; }
    nquads++;
}
static int quads(wolf_vm *vm) {
    nquads = 0;
    wolf_window_quads(vm, &SRC, 640, 480, count, NULL);
    return nquads;
}

static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; }

#define V(x) (2000000 + (x))
#define SYS(x) (9000000 + (x))

static wolf_vm *make(void) {
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {10, 0, 10, 20, 10, 0, 1, 0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    CHECK(wolf_vm_use_window(vm) == 0);
    return vm;
}

static void test_message(void) {
    wolf_vm *vm = make();
    wolf_set(vm, NULL, SYS(1), 30);
    wolf_set(vm, NULL, SYS(2), 40);
    wolf_set(vm, NULL, SYS(9), 120); /* two characters a frame */
    buf b = {0};
    cmd(&b, 101, NULL, 0, (const char *[]){"ABCDEF", NULL}, 0);
    cmd(&b, 121, (int32_t[]){V(1), 1, 0, 0}, 4, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(wolf_get(vm, NULL, SYS(12)) == 1);
    CHECK(quads(vm) == 0); /* nothing typed yet */
    wolf_vm_input(vm, 0);
    CHECK(quads(vm) == 2 && first_x == 30 && first_y == 40);
    wolf_vm_input(vm, 0);
    CHECK(quads(vm) == 4);
    /* confirm while typing shows the rest, and does not close */
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(quads(vm) == 6 && wolf_vm_ui_busy(vm));
    /* holding is not pressing again */
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(wolf_vm_ui_busy(vm));
    wolf_vm_input(vm, 0);
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(!wolf_vm_ui_busy(vm) && wolf_get(vm, NULL, SYS(12)) == 0 && quads(vm) == 0);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(1)) == 1);

    /* speed 0 shows it all at once; \^ closes it without a key once shown */
    wolf_set(vm, NULL, SYS(9), 0);
    buf c = {0};
    cmd(&c, 101, NULL, 0, (const char *[]){"AB\\^", NULL}, 0);
    wolf_list lc = finish(&c);
    wolf_thread_start(t, &lc, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    wolf_vm_input(vm, 0);
    CHECK(quads(vm) == 2);
    wolf_vm_input(vm, 0);
    CHECK(!wolf_vm_ui_busy(vm) && wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    wolf_list_free(&l);
    wolf_list_free(&lc);
    free(t);
    wolf_vm_destroy(vm);
}

/* 102 with three choices (the second empty), cancel separate; bodies set V2 */
static void test_choices(void) {
    wolf_vm *vm = make();
    buf b = {0};
    cmd(&b, 102, (int32_t[]){3}, 1, (const char *[]){"Yes", "", "No", NULL}, 0);
    for (int i = 0; i < 3; i++) {
        cmd(&b, 401, NULL, 0, NULL, 0);
        cmd(&b, 121, (int32_t[]){V(2), 10 + i, 0, 0}, 4, NULL, 1);
    }
    cmd(&b, 421, NULL, 0, NULL, 0);
    cmd(&b, 121, (int32_t[]){V(2), 99, 0, 0}, 4, NULL, 1);
    cmd(&b, 499, NULL, 0, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread *t = calloc(1, sizeof *t);

    /* down skips the empty choice; confirm picks "No" */
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(quads(vm) == 3 + 2); /* "Yes" and "No", no window or cursor images in this game */
    wolf_vm_input(vm, WOLF_KEY_DOWN);
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(2)) == 12);
    /* up from the first wraps to the last; cancel takes the cancel branch */
    wolf_vm_input(vm, 0);
    wolf_thread_start(t, &l, -1, -1);
    wolf_vm_run_frame(vm, t);
    wolf_vm_input(vm, WOLF_KEY_UP);
    wolf_vm_input(vm, WOLF_KEY_CANCEL);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(2)) == 99);
    /* system variable 7 sets where the cursor starts */
    wolf_vm_input(vm, 0);
    wolf_set(vm, NULL, SYS(7), 2);
    wolf_thread_start(t, &l, -1, -1);
    wolf_vm_run_frame(vm, t);
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(2)) == 12);
    wolf_list_free(&l);
    free(t);
    wolf_vm_destroy(vm);
}

int main(void) {
    for (int i = 0; i < 32; i++) white[i] = 0xFFFFFFFFu;
    test_message();
    test_choices();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("msgwin: all checks passed");
    return 0;
}
