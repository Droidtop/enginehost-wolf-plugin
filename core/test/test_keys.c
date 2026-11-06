/* Tests for the player's keys and 123, key input (keys.c). */
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


static char last_log[600];
static int log_count;
static void on_log(void *ctx, int level, const char *m) {
    (void)ctx; (void)level;
    snprintf(last_log, sizeof last_log, "%s", m);
    log_count++;
}

#define V(x) (2000000 + (x))
#define SYS(x) (9000000 + (x))

static wolf_vm *make(int window) {
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {10, 0, 10, 20, 10, 0, 1, 0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    if (window) CHECK(wolf_vm_use_window(vm) == 0);
    return vm;
}

/* One 123 into V0 with these options, run once with these keys held. */
static int32_t held(uint32_t opt, uint32_t keys) {
    wolf_vm *vm = make(0);
    buf b = {0};
    cmd(&b, 123, (int32_t[]){V(0), (int32_t)opt}, 2, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread t;
    wolf_thread_start(&t, &l, -1, -1);
    wolf_set(vm, NULL, V(0), -5);
    wolf_vm_input(vm, keys);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_DONE);
    int32_t r = wolf_get(vm, NULL, V(0));
    wolf_list_free(&l);
    wolf_vm_destroy(vm);
    return r;
}

static void test_held(void) {
    /* 0x11: the four directions and confirm (the commonest form in the owner's games) */
    CHECK(held(0x11, 0) == 0);
    CHECK(held(0x11, WOLF_KEY_DOWN) == 2);
    CHECK(held(0x11, WOLF_KEY_LEFT) == 4);
    CHECK(held(0x11, WOLF_KEY_RIGHT) == 6);
    CHECK(held(0x11, WOLF_KEY_UP) == 8);
    CHECK(held(0x11, WOLF_KEY_CONFIRM | WOLF_KEY_DOWN) == 10);
    CHECK(held(0x11, WOLF_KEY_CANCEL) == 0); /* not asked about */
    CHECK(held(0x70, WOLF_KEY_CANCEL | WOLF_KEY_SUB) == 11);
    CHECK(held(0x40, WOLF_KEY_SUB) == 12);
    CHECK(held(0x10, WOLF_KEY_UP) == 0);
    /* 7 up and down only, 8 left and right only */
    CHECK(held(0x07, WOLF_KEY_LEFT) == 0 && held(0x07, WOLF_KEY_UP) == 8);
    CHECK(held(0x08, WOLF_KEY_UP) == 0 && held(0x08, WOLF_KEY_LEFT) == 4);
}

/* 0x91 twice: each waits for a new press; a held key does not answer the second */
static void test_wait(void) {
    wolf_vm *vm = make(0);
    buf b = {0};
    cmd(&b, 123, (int32_t[]){V(0), 0x91}, 2, NULL, 0);
    cmd(&b, 123, (int32_t[]){V(1), 0x91}, 2, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread t;
    wolf_thread_start(&t, &l, -1, -1);
    wolf_vm_input(vm, 0);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD);
    wolf_vm_input(vm, WOLF_KEY_CANCEL); /* not in the set */
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD);
    wolf_vm_input(vm, WOLF_KEY_CANCEL | WOLF_KEY_UP);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD); /* the first answered, the second waits */
    CHECK(wolf_get(vm, NULL, V(0)) == 8);
    wolf_vm_input(vm, WOLF_KEY_UP); /* still held */
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD);
    wolf_vm_input(vm, WOLF_KEY_UP | WOLF_KEY_RIGHT);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(1)) == 6);
    wolf_list_free(&l);
    wolf_vm_destroy(vm);
}

/* The confirm that closes a message does not also answer the 123 after it */
static void test_window_takes_press(void) {
    wolf_vm *vm = make(1);
    wolf_set(vm, NULL, SYS(9), 0); /* shown at once */
    buf b = {0};
    cmd(&b, 101, NULL, 0, (const char *[]){"Hi", NULL}, 0);
    cmd(&b, 123, (int32_t[]){V(0), 0x90}, 2, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread t;
    wolf_thread_start(&t, &l, -1, -1);
    wolf_vm_input(vm, 0);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD && wolf_vm_ui_busy(vm));
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(!wolf_vm_ui_busy(vm));
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD); /* now in the 123 */
    wolf_vm_input(vm, 0);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_YIELD);
    wolf_vm_input(vm, WOLF_KEY_CONFIRM);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(0)) == 10);
    wolf_list_free(&l);
    wolf_vm_destroy(vm);
}

/* Unknown option bits are logged once and read as the basic form; the key-code form gives 0 */
static void test_unknown(void) {
    wolf_vm *vm = make(0);
    buf b = {0};
    cmd(&b, 123, (int32_t[]){V(0), 0x20330}, 2, NULL, 0);
    cmd(&b, 123, (int32_t[]){V(1), 0x1180, 142}, 3, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread t;
    wolf_thread_start(&t, &l, -1, -1);
    wolf_set(vm, NULL, V(1), 7);
    log_count = 0;
    wolf_vm_input(vm, WOLF_KEY_CANCEL);
    CHECK(wolf_vm_run_frame(vm, &t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(0)) == 11 && wolf_get(vm, NULL, V(1)) == 0);
    CHECK(log_count == 2 && strstr(last_log, "key code") != NULL);
    wolf_list_free(&l);
    wolf_vm_destroy(vm);
}

int main(void) {
    test_held();
    test_wait();
    test_window_takes_press();
    test_unknown();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("keys: ok\n");
    return 0;
}
