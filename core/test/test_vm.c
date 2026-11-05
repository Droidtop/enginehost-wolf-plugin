/* Tests for the command-list parser and the dispatcher. Lists are built here from the layout in
 * docs/behaviour/formats.md; no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_cmd.h"

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

/* One command: code, ints, strings (NULL-terminated array), indent. */
static void cmd(buf *b, int32_t code, const int32_t *ints, int nint, const char *const *strs, int indent) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, (uint32_t)indent);
    int ns = 0;
    while (strs && strs[ns]) ns++;
    u8(b, (uint32_t)ns);
    for (int i = 0; i < ns; i++) {
        uint32_t len = (uint32_t)strlen(strs[i]) + 1;
        u32(b, len);
        put(b, strs[i], len);
    }
    u8(b, 0);
}

static char last_log[600];
static int log_count;
static void on_log(void *ctx, int level, const char *m) {
    (void)ctx; (void)level;
    snprintf(last_log, sizeof last_log, "%s", m);
    log_count++;
}

static void test_parse(void) {
    buf b = {0};
    u32(&b, 4);
    int32_t args[] = {7, -2, 1000000};
    const char *strs[] = {"hello", "", NULL};
    cmd(&b, 103, args, 3, strs, 2);
    cmd(&b, 0, NULL, 0, NULL, 0);
    /* a move route after a command */
    u8(&b, 1); u32(&b, 201); u8(&b, 0); u8(&b, 0); u8(&b, 1);
    for (int i = 0; i < 5; i++) u8(&b, 0);
    u8(&b, 3); u32(&b, 2);
    u8(&b, 1); u8(&b, 1); u32(&b, 9); u8(&b, 1); u8(&b, 0);
    u8(&b, 2); u8(&b, 0); u8(&b, 1); u8(&b, 0);
    cmd(&b, 0, NULL, 0, NULL, 0);
    u8(&b, 0xEE); /* trailing data is left for the caller */

    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    CHECK(used == b.n - 1);
    CHECK(l.count == 4);
    CHECK(l.cmds[0].code == 103 && l.cmds[0].indent == 2 && l.cmds[0].nint == 3);
    CHECK(l.cmds[0].ints[1] == -2 && l.cmds[0].ints[2] == 1000000);
    CHECK(l.cmds[0].nstr == 2 && strcmp(l.cmds[0].strs[0].text, "hello") == 0 && l.cmds[0].strs[1].len == 0);
    CHECK(l.cmds[2].code == 201 && l.cmds[2].route && l.cmds[2].route->step_count == 2);
    CHECK(l.cmds[3].route == NULL);
    wolf_list_free(&l);

    /* every truncation fails cleanly */
    for (size_t n = 0; n < used; n++) {
        wolf_list t;
        size_t u;
        CHECK(wolf_list_parse(b.p, n, &u, &t) != WOLF_LIST_OK);
    }
    /* a count the input cannot hold, a bad tail, an argc of zero */
    uint8_t big[] = {0xFF, 0xFF, 0x0F, 0x00, 1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(wolf_list_parse(big, sizeof big, &used, &l) != WOLF_LIST_OK);
    buf bad = {0};
    u32(&bad, 1); u8(&bad, 1); u32(&bad, 0); u8(&bad, 0); u8(&bad, 0); u8(&bad, 7);
    CHECK(wolf_list_parse(bad.p, bad.n, &used, &l) == WOLF_LIST_E_FORMAT);
    bad.p[4] = 0;
    CHECK(wolf_list_parse(bad.p, bad.n, &used, &l) == WOLF_LIST_E_FORMAT);
    free(bad.p);

    /* random mutations never crash (run under ASan) */
    uint32_t rng = 7;
    for (int round = 0; round < 20000; round++) {
        buf m = {0};
        put(&m, b.p, b.n);
        for (int e = 0; e < 3; e++) {
            rng = rng * 1103515245u + 12345u;
            m.p[(rng >> 8) % m.n] ^= (uint8_t)(rng >> 3);
        }
        wolf_list t;
        size_t u;
        if (wolf_list_parse(m.p, m.n, &u, &t) == WOLF_LIST_OK) {
            CHECK(u <= m.n);
            wolf_list_free(&t);
        }
        free(m.p);
    }
    free(b.p);
}

static void test_dispatch(void) {
    wolf_host host = {NULL, on_log};
    wolf_vm *vm = wolf_vm_create(&host);
    buf b = {0};
    u32(&b, 6);
    const char *msg[] = {"hi", NULL};
    cmd(&b, 103, NULL, 0, NULL, 0);
    cmd(&b, 106, NULL, 0, msg, 0);
    cmd(&b, 104, NULL, 0, NULL, 0);
    cmd(&b, 999, NULL, 0, NULL, 0);
    cmd(&b, 999, NULL, 0, NULL, 0);
    cmd(&b, 0, NULL, 0, NULL, 0);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l);
    log_count = 0;
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_vm_flag_interrupt(vm) && !wolf_vm_flag_skip_wait(vm));
    CHECK(wolf_vm_unhandled_seen(vm, 999));
    CHECK(log_count == 2); /* the debug text, then 999 once */
    CHECK(wolf_vm_has_handler(0) && !wolf_vm_has_handler(999) && !wolf_vm_has_handler(-5));

    /* the end-of-list command stops a list early */
    buf e = {0};
    u32(&e, 2);
    cmd(&e, -1, NULL, 0, NULL, 0);
    cmd(&e, 104, NULL, 0, NULL, 0);
    wolf_list le;
    CHECK(wolf_list_parse(e.p, e.n, &used, &le) == WOLF_LIST_OK);
    wolf_vm *vm2 = wolf_vm_create(&host);
    wolf_thread_start(t, &le);
    CHECK(wolf_vm_run_frame(vm2, t) == WOLF_RUN_DONE && !wolf_vm_flag_interrupt(vm2));
    wolf_vm_destroy(vm2);

    /* the per-frame budget: six calls of a million blanks is over 5,000,000 */
    buf m = {0};
    u32(&m, 1000000);
    for (int i = 0; i < 1000000; i++) cmd(&m, 0, NULL, 0, NULL, 0);
    wolf_list million;
    CHECK(wolf_list_parse(m.p, m.n, &used, &million) == WOLF_LIST_OK);
    wolf_thread_start(t, &million);
    for (int i = 0; i < 4; i++) CHECK(wolf_thread_call(t, &million) == WOLF_NEXT);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE); /* exactly 5,000,000 is allowed */
    wolf_thread_start(t, &million);
    for (int i = 0; i < 5; i++) CHECK(wolf_thread_call(t, &million) == WOLF_NEXT);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_E_LOOP);

    /* call depth is capped */
    wolf_thread_start(t, &l);
    int r = WOLF_NEXT;
    for (unsigned i = 0; i < WOLF_CALL_DEPTH_MAX && r == WOLF_NEXT; i++) r = wolf_thread_call(t, &l);
    CHECK(r == WOLF_RUN_E_DEPTH);

    wolf_list_free(&million);
    wolf_list_free(&le);
    wolf_list_free(&l);
    free(m.p); free(e.p); free(b.p); free(t);
    wolf_vm_destroy(vm);
}

int main(void) {
    test_parse();
    test_dispatch();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    fprintf(stderr, "all VM checks passed\n");
    return 0;
}
