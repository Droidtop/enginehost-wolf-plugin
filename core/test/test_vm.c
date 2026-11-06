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

static const wolf_config small = {100, 0, 10, 0, 0, 0, 1, 0};

static void test_dispatch(void) {
    wolf_host host = {NULL, on_log};
    wolf_vm *vm = wolf_vm_create(&host, &small);
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
    wolf_thread_start(t, &l, -1, -1);
    log_count = 0;
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_vm_flag_stop_choices(vm) && !wolf_vm_flag_stop_message(vm));
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
    wolf_vm *vm2 = wolf_vm_create(&host, &small);
    wolf_thread_start(t, &le, -1, -1);
    CHECK(wolf_vm_run_frame(vm2, t) == WOLF_RUN_DONE && !wolf_vm_flag_stop_choices(vm2));
    wolf_vm_destroy(vm2);

    /* the per-frame budget: six calls of a million blanks is over 5,000,000 */
    buf m = {0};
    u32(&m, 1000000);
    for (int i = 0; i < 1000000; i++) cmd(&m, 0, NULL, 0, NULL, 0);
    wolf_list million;
    CHECK(wolf_list_parse(m.p, m.n, &used, &million) == WOLF_LIST_OK);
    wolf_thread_start(t, &million, -1, -1);
    for (int i = 0; i < 4; i++) CHECK(wolf_thread_call(t, &million, -1, 0) == WOLF_NEXT);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE); /* exactly 5,000,000 is allowed */
    wolf_thread_start(t, &million, -1, -1);
    for (int i = 0; i < 5; i++) CHECK(wolf_thread_call(t, &million, -1, 0) == WOLF_NEXT);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_E_LOOP);

    /* call depth is capped */
    wolf_thread_start(t, &l, -1, -1);
    int r = WOLF_NEXT;
    for (unsigned i = 0; i < WOLF_CALL_DEPTH_MAX && r == WOLF_NEXT; i++) r = wolf_thread_call(t, &l, -1, 0);
    CHECK(r == WOLF_RUN_E_DEPTH);

    wolf_list_free(&million);
    wolf_list_free(&le);
    wolf_list_free(&l);
    free(m.p); free(e.p); free(b.p); free(t);
    wolf_vm_destroy(vm);
}

/* ---- control flow ------------------------------------------------------------------------- */

/* A list written as {code, indent, nint, ints..., string or NULL}. */
typedef struct { int32_t code; int indent; int nint; int32_t ints[12]; const char *str; } step;

static wolf_list make(const step *s, int n) {
    buf b = {0};
    u32(&b, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        const char *strs[] = {s[i].str, NULL};
        cmd(&b, s[i].code, s[i].ints, s[i].nint, s[i].str ? strs : NULL, s[i].indent);
    }
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    free(b.p);
    return l;
}

#define V(x) (2000000 + (x))  /* normal variable x */
/* 121: variable = a + b, or += with ADD */
#define SET(var, a, ind) {121, ind, 4, {V(var), a, 0, 0x0000}, NULL}
#define ADD(var, a, ind) {121, ind, 4, {V(var), a, 0, 0x0100}, NULL}

static wolf_vm *vm_for(const wolf_host *h) { return wolf_vm_create(h, &small); }

static int run_all(wolf_vm *vm, const wolf_list *l, int frames) {
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, l, -1, -1);
    int r = WOLF_RUN_YIELD;
    for (int i = 0; i < frames && r == WOLF_RUN_YIELD; i++) r = wolf_vm_run_frame(vm, t);
    free(t);
    return r;
}

static void test_number_condition(void) {
    wolf_host host = {NULL, on_log};
    step s[] = {
        {111, 0, 4, {0x11, V(0), 5, 2}, NULL}, /* one case, with else: v0 == 5 */
        {401, 0, 0, {0}, NULL},
        SET(1, 1, 1),
        {420, 0, 0, {0}, NULL},
        SET(1, 2, 1),
        {499, 0, 0, {0}, NULL},
        SET(2, 7, 0),
    };
    wolf_list l = make(s, 7);
    for (int v0 = 4; v0 <= 5; v0++) {
        wolf_vm *vm = vm_for(&host);
        wolf_set(vm, NULL, V(0), v0);
        CHECK(run_all(vm, &l, 1) == WOLF_RUN_DONE);
        CHECK(wolf_get(vm, NULL, V(1)) == (v0 == 5 ? 1 : 2));
        CHECK(wolf_get(vm, NULL, V(2)) == 7);
        wolf_vm_destroy(vm);
    }
    wolf_list_free(&l);

    /* two cases, no else: the second runs; with neither true, nothing runs */
    step two[] = {
        {111, 0, 7, {2, V(0), 1, 2, V(0), 2, 2}, NULL},
        {401, 0, 0, {0}, NULL},
        SET(1, 10, 1),
        {401, 0, 0, {0}, NULL},
        SET(1, 20, 1),
        /* a branch nested in the second body */
        {111, 1, 4, {1, V(0), 2, 1}, NULL}, /* v0 >= 2 */
        {401, 1, 0, {0}, NULL},
        SET(3, 30, 2),
        {499, 1, 0, {0}, NULL},
        {499, 0, 0, {0}, NULL},
        SET(2, 7, 0),
    };
    wolf_list l2 = make(two, 11);
    int expect1[] = {0, 10, 20, 0};
    for (int v0 = 0; v0 <= 3; v0++) {
        wolf_vm *vm = vm_for(&host);
        wolf_set(vm, NULL, V(0), v0);
        CHECK(run_all(vm, &l2, 1) == WOLF_RUN_DONE);
        CHECK(wolf_get(vm, NULL, V(1)) == expect1[v0]);
        CHECK(wolf_get(vm, NULL, V(3)) == (v0 == 2 ? 30 : 0));
        CHECK(wolf_get(vm, NULL, V(2)) == 7);
        wolf_vm_destroy(vm);
    }
    wolf_list_free(&l2);

    /* every comparison */
    int32_t want[7][3] = { /* left 1 against right 0, 1, 2 */
        {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}, {0, 1, 0}};
    for (int op = 0; op < 7; op++) {
        for (int right = 0; right < 3; right++) {
            step c[] = {
                {111, 0, 4, {1, V(0), right, op}, NULL},
                {401, 0, 0, {0}, NULL},
                SET(1, 1, 1),
                {499, 0, 0, {0}, NULL},
            };
            wolf_list lc = make(c, 4);
            wolf_vm *vm = vm_for(&host);
            wolf_set(vm, NULL, V(0), 1);
            run_all(vm, &lc, 1);
            CHECK(wolf_get(vm, NULL, V(1)) == want[op][right]);
            wolf_vm_destroy(vm);
            wolf_list_free(&lc);
        }
    }
}

static void test_string_condition(void) {
    wolf_host host = {NULL, on_log};
    for (int op = 0; op < 4; op++) {
        step s[] = {
            {112, 0, 2, {1, (int32_t)(((uint32_t)op << 28) | 3000000u)}, "ab"},
            {401, 0, 0, {0}, NULL},
            SET(1, 1, 1),
            {499, 0, 0, {0}, NULL},
        };
        wolf_list l = make(s, 4);
        wolf_vm *vm = vm_for(&host);
        wolf_set_str(vm, NULL, 3000000, "abc");
        run_all(vm, &l, 1);
        int expect[] = {0, 1, 1, 1}; /* "abc" vs "ab": ==, !=, includes, starts with */
        CHECK(wolf_get(vm, NULL, V(1)) == expect[op]);
        wolf_vm_destroy(vm);
        wolf_list_free(&l);
    }
    /* compared with another string variable */
    step v[] = {
        {112, 0, 3, {1, (int32_t)((1u << 24) | 3000000u), 3000001}, NULL},
        {401, 0, 0, {0}, NULL},
        SET(1, 1, 1),
        {499, 0, 0, {0}, NULL},
    };
    wolf_list lv = make(v, 4);
    wolf_vm *vm = vm_for(&host);
    wolf_set_str(vm, NULL, 3000000, "same");
    wolf_set_str(vm, NULL, 3000001, "same");
    run_all(vm, &lv, 1);
    CHECK(wolf_get(vm, NULL, V(1)) == 1);
    wolf_vm_destroy(vm);
    wolf_list_free(&lv);
}

static void test_loops(void) {
    wolf_host host = {NULL, on_log};
    /* counted: 3 runs; 0 runs skips the body */
    for (int n = 0; n <= 3; n += 3) {
        step s[] = {{179, 0, 1, {n}, NULL}, ADD(0, 1, 1), {498, 0, 0, {0}, NULL}, SET(1, 1, 0)};
        wolf_list l = make(s, 4);
        wolf_vm *vm = vm_for(&host);
        CHECK(run_all(vm, &l, 1) == WOLF_RUN_DONE);
        CHECK(wolf_get(vm, NULL, V(0)) == n && wolf_get(vm, NULL, V(1)) == 1);
        wolf_vm_destroy(vm);
        wolf_list_free(&l);
    }
    /* endless, left by a break inside a branch */
    step b[] = {
        {170, 0, 0, {0}, NULL},
        ADD(0, 1, 1),
        {111, 1, 4, {1, V(0), 4, 2}, NULL},
        {401, 1, 0, {0}, NULL},
        {171, 2, 0, {0}, NULL},
        {499, 1, 0, {0}, NULL},
        {498, 0, 0, {0}, NULL},
        SET(1, 1, 0),
    };
    wolf_list lb = make(b, 8);
    wolf_vm *vm = vm_for(&host);
    CHECK(run_all(vm, &lb, 1) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(0)) == 4 && wolf_get(vm, NULL, V(1)) == 1);
    wolf_vm_destroy(vm);
    wolf_list_free(&lb);
    /* continue in a counted loop still uses up a run */
    step c[] = {
        {179, 0, 1, {5}, NULL},
        ADD(0, 1, 1),
        {111, 1, 4, {1, V(0), 3, 4}, NULL}, /* v0 < 3 */
        {401, 1, 0, {0}, NULL},
        {176, 2, 0, {0}, NULL},
        {499, 1, 0, {0}, NULL},
        ADD(1, 1, 1),
        {498, 0, 0, {0}, NULL},
    };
    wolf_list lc = make(c, 8);
    vm = vm_for(&host);
    CHECK(run_all(vm, &lc, 1) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(0)) == 5 && wolf_get(vm, NULL, V(1)) == 3);
    wolf_vm_destroy(vm);
    wolf_list_free(&lc);
    /* nested counted loops: 3 x 2 */
    step n[] = {
        {179, 0, 1, {3}, NULL},
        {179, 1, 1, {2}, NULL},
        ADD(0, 1, 2),
        {498, 1, 0, {0}, NULL},
        {498, 0, 0, {0}, NULL},
    };
    wolf_list ln = make(n, 5);
    vm = vm_for(&host);
    CHECK(run_all(vm, &ln, 1) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(0)) == 6);
    wolf_vm_destroy(vm);
    wolf_list_free(&ln);
    /* an endless loop with no wait is stopped by the frame budget */
    step e[] = {{170, 0, 0, {0}, NULL}, {0, 1, 0, {0}, NULL}, {498, 0, 0, {0}, NULL}};
    wolf_list le = make(e, 3);
    vm = vm_for(&host);
    CHECK(run_all(vm, &le, 1) == WOLF_RUN_E_LOOP);
    wolf_vm_destroy(vm);
    wolf_list_free(&le);
}

static void test_labels_waits_stop(void) {
    wolf_host host = {NULL, on_log};
    step s[] = {{213, 0, 0, {0}, "L"}, SET(0, 99, 0), {212, 0, 0, {0}, "L"}, SET(1, 1, 0), {213, 0, 0, {0}, "none"}};
    wolf_list l = make(s, 5);
    wolf_vm *vm = vm_for(&host);
    log_count = 0;
    CHECK(run_all(vm, &l, 1) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(0)) == 0 && wolf_get(vm, NULL, V(1)) == 1);
    CHECK(log_count == 1); /* the missing label */
    wolf_vm_destroy(vm);
    wolf_list_free(&l);

    /* wait 2: two frames pass, the third goes on; wait 0 does not yield */
    step w[] = {{180, 0, 1, {0}, NULL}, {180, 0, 1, {2}, NULL}, SET(0, 1, 0)};
    wolf_list lw = make(w, 3);
    vm = vm_for(&host);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &lw, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(wolf_get(vm, NULL, V(0)) == 0);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(0)) == 1);
    free(t);
    wolf_vm_destroy(vm);
    wolf_list_free(&lw);
}

static void test_calls(void) {
    wolf_host host = {NULL, on_log};
    /* common event 0 "Adder": self 1 = argument (self 0) + 1, then stop; self 1 is its result */
    step ce[] = {
        {121, 0, 4, {1600001, 1600000, 1, 0}, NULL},
        {172, 0, 0, {0}, NULL},
        {121, 0, 4, {1600001, 0, 0, 0}, NULL}, /* never reached */
    };
    wolf_list lce = make(ce, 3);
    wolf_common commons[] = {{&lce, "Adder", 1}};
    /* 210 by id with one number argument and a result target; 300 by name; the caller goes on */
    step s[] = {
        {210, 0, 4, {500000, 0x01000001, 7, V(5)}, NULL},
        SET(6, 1, 0),
        {300, 0, 4, {0, 0x01000001, 40, V(7)}, "Adder"},
        {210, 0, 2, {500009, 0}, NULL},      /* no such common event: logged, ignored */
        {210, 0, 2, {12, 0}, NULL},          /* a map event's page: not built, logged */
    };
    wolf_list l = make(s, 5);
    wolf_config cfg = small;
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    wolf_vm_set_common_events(vm, commons, 1);
    log_count = 0;
    CHECK(run_all(vm, &l, 1) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(5)) == 8);
    CHECK(wolf_get(vm, NULL, V(6)) == 1);
    CHECK(wolf_get(vm, NULL, V(7)) == 41);
    CHECK(log_count == 2);
    wolf_vm_destroy(vm);
    wolf_list_free(&l);

    /* a common event that calls itself stops at the depth cap */
    step self[] = {{210, 0, 2, {500000, 0}, NULL}};
    wolf_list ls = make(self, 1);
    wolf_common rec[] = {{&ls, "Self", -1}};
    vm = wolf_vm_create(&host, &cfg);
    wolf_vm_set_common_events(vm, rec, 1);
    CHECK(run_all(vm, &ls, 1) == WOLF_RUN_E_DEPTH);
    wolf_vm_destroy(vm);
    wolf_list_free(&ls);
    wolf_list_free(&lce);
}

int main(void) {
    test_parse();
    test_dispatch();
    test_number_condition();
    test_string_condition();
    test_loops();
    test_labels_waits_stop();
    test_calls();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    fprintf(stderr, "all VM checks passed\n");
    return 0;
}
