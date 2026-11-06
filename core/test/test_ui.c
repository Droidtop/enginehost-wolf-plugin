/* Tests for messages and choices (101, 102, 104, 105). Lists are built here from the layout in
 * docs/behaviour/formats.md; no game data is used. */
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

#define V(x) (2000000 + (x))
#define SYS(x) (9000000 + (x))

/* "V(x) = value" at indent */
static void set(buf *b, int32_t x, int32_t value, int indent) {
    cmd(b, 121, (int32_t[]){V(x), value, 0, 0}, 4, NULL, indent);
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

/* A recording UI. */
typedef struct {
    int messages, choices, closes;
    char last[256];
    char texts[15][64];
    uint32_t count;
    int cancel;
} rec;

static void on_message(void *ctx, const char *t) {
    rec *r = ctx;
    r->messages++;
    snprintf(r->last, sizeof r->last, "%s", t);
}
static void on_choices(void *ctx, const char *const *t, uint32_t count, int cancel) {
    rec *r = ctx;
    r->choices++;
    r->count = count;
    r->cancel = cancel;
    for (uint32_t i = 0; i < count && i < 15; i++) snprintf(r->texts[i], sizeof r->texts[i], "%s", t[i]);
}
static void on_close(void *ctx) { ((rec *)ctx)->closes++; }

static int log_count;
static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; log_count++; }

static wolf_vm *make_vm(rec *r) {
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {100, 0, 10, 100, 10, 0, 1, 0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    if (r) {
        wolf_ui ui = {r, on_message, on_choices, on_close};
        wolf_vm_set_ui(vm, &ui);
    }
    return vm;
}

static void test_message(void) {
    rec r = {0};
    wolf_vm *vm = make_vm(&r);
    wolf_set(vm, NULL, V(1), 42);
    buf b = {0};
    cmd(&b, 101, NULL, 0, (const char *[]){"@7\nHP \\v[1]\\!", NULL}, 0);
    set(&b, 2, 1, 0);
    wolf_list l = finish(&b);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(r.messages == 1 && strcmp(r.last, "HP 42\\!") == 0); /* codes replaced, "@7" line gone */
    CHECK(wolf_get(vm, NULL, SYS(0)) == 7);                     /* the face number */
    CHECK(wolf_vm_ui_busy(vm));
    /* it waits, frame after frame, without showing again */
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD && wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(r.messages == 1 && wolf_get(vm, NULL, V(2)) == 0);
    wolf_vm_message_closed(vm);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(2)) == 1 && !wolf_vm_ui_busy(vm));

    /* "@" not followed by digits and a line break stays text */
    buf c = {0};
    cmd(&c, 101, NULL, 0, (const char *[]){"@home is here", NULL}, 0);
    wolf_list lc = finish(&c);
    wolf_thread_start(t, &lc, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD && strcmp(r.last, "@home is here") == 0);
    wolf_vm_message_closed(vm);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, SYS(0)) == 7);

    wolf_list_free(&l);
    wolf_list_free(&lc);
    free(t);
    wolf_vm_destroy(vm);
}

static void test_headless(void) {
    wolf_vm *vm = make_vm(NULL);
    buf b = {0};
    cmd(&b, 101, NULL, 0, (const char *[]){"@3\nhello", NULL}, 0);
    /* choices: the first is empty, so the second is the first shown */
    cmd(&b, 102, (int32_t[]){2}, 1, (const char *[]){"", "B", NULL}, 0);
    cmd(&b, 401, NULL, 0, NULL, 0);
    set(&b, 1, 10, 1);
    cmd(&b, 401, NULL, 0, NULL, 0);
    set(&b, 1, 20, 1);
    cmd(&b, 499, NULL, 0, NULL, 0);
    wolf_list l = finish(&b);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    log_count = 0;
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, SYS(0)) == 3 && wolf_get(vm, NULL, V(1)) == 20);
    CHECK(log_count == 3); /* headless once, the message, the choice */
    wolf_list_free(&l);
    free(t);
    wolf_vm_destroy(vm);
}

/* 102 with three choices (cancel field `cancel`, extra bits), a 421 branch when separate, and
 * 402 branches for the extras; each body sets V1 to its number. */
static wolf_list choice_list(uint32_t cancel, uint32_t extra, const char *a, const char *b2, const char *c3) {
    buf b = {0};
    cmd(&b, 102, (int32_t[]){(int32_t)(3u | cancel << 4 | extra << 8)}, 1, (const char *[]){a, b2, c3, NULL}, 0);
    for (int i = 0; i < 3; i++) {
        cmd(&b, 401, NULL, 0, NULL, 0);
        set(&b, 1, 10 + i, 1);
    }
    for (int i = 0; i < 3; i++) {
        if (!(extra & (1u << i))) continue;
        cmd(&b, 402, NULL, 0, NULL, 0);
        set(&b, 1, 30 + i, 1);
    }
    if (cancel == 0) {
        cmd(&b, 421, NULL, 0, NULL, 0);
        set(&b, 1, 99, 1);
    }
    cmd(&b, 499, NULL, 0, NULL, 0);
    set(&b, 2, 1, 0); /* after the construct */
    return finish(&b);
}

/* Runs a choice list, answers with `answer` (or forces it with 104 when answer is -2), and returns
 * V1. */
static int32_t choose(uint32_t cancel, uint32_t extra, int32_t answer, rec *r) {
    wolf_vm *vm = make_vm(r);
    wolf_list l = choice_list(cancel, extra, "A", "B", "C");
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_YIELD);
    if (answer == -2) {
        buf f = {0};
        cmd(&f, 104, NULL, 0, NULL, 0);
        wolf_list lf = finish(&f);
        wolf_thread *u = calloc(1, sizeof *u);
        wolf_thread_start(u, &lf, -1, -1);
        CHECK(wolf_vm_run_frame(vm, u) == WOLF_RUN_DONE);
        free(u);
        wolf_list_free(&lf);
    } else {
        wolf_vm_choice_made(vm, answer);
    }
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, NULL, V(2)) == 1); /* the construct always ends */
    int32_t v = wolf_get(vm, NULL, V(1));
    wolf_list_free(&l);
    free(t);
    wolf_vm_destroy(vm);
    return v;
}

static void test_choices(void) {
    rec r = {0};
    CHECK(choose(0, 0, 0, &r) == 10);
    CHECK(r.choices == 1 && r.count == 3 && r.cancel == 1 && strcmp(r.texts[2], "C") == 0);
    CHECK(choose(0, 0, 2, &r) == 12);
    CHECK(choose(0, 0, -1, &r) == 99);     /* a separate cancel branch */
    CHECK(choose(3, 0, -1, &r) == 11);     /* cancel acts as choice 1 */
    CHECK(choose(1, 0, 1, &r) == 11 && r.cancel == 0); /* cancel disabled */
    CHECK(choose(1, 0, -1, &r) == 0);      /* a UI that cancels anyway: past the end */
    CHECK(choose(0, 0, 7, &r) == 0);       /* an answer out of range: past the end */
    CHECK(choose(9, 0, -1, &r) == 0);      /* cancel naming no choice is taken as disabled */
    /* 104 forces the list shut: the forced branch is the 402 after left/right ones */
    int closes = r.closes;
    CHECK(choose(0, 0x4, -2, &r) == 32 && r.closes == closes + 1);
    CHECK(choose(0, 0x5, -2, &r) == 32);
    CHECK(choose(0, 0x7, -2, &r) == 32);
    CHECK(choose(0, 0, -2, &r) == 0);      /* no forced branch: past the end */

    /* every choice empty (after codes): the whole command is skipped */
    wolf_vm *vm = make_vm(&r);
    wolf_list l = choice_list(0, 0, "", "\\s[5]", "");
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    int before = r.choices;
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(r.choices == before && wolf_get(vm, NULL, V(1)) == 0 && wolf_get(vm, NULL, V(2)) == 1);
    wolf_list_free(&l);
    free(t);
    wolf_vm_destroy(vm);
}

static void test_one_window(void) {
    rec r = {0};
    wolf_vm *vm = make_vm(&r);
    buf a = {0}, b = {0}, s = {0};
    cmd(&a, 101, NULL, 0, (const char *[]){"first", NULL}, 0);
    set(&a, 1, 1, 0);
    cmd(&b, 101, NULL, 0, (const char *[]){"second", NULL}, 0);
    set(&b, 2, 1, 0);
    cmd(&s, 105, NULL, 0, NULL, 0);
    wolf_list la = finish(&a), lb = finish(&b), ls = finish(&s);
    wolf_thread *ta = calloc(1, sizeof *ta), *tb = calloc(1, sizeof *tb), *ts = calloc(1, sizeof *ts);
    wolf_thread_start(ta, &la, -1, -1);
    wolf_thread_start(tb, &lb, -1, -1);
    CHECK(wolf_vm_run_frame(vm, ta) == WOLF_RUN_YIELD);
    CHECK(wolf_vm_run_frame(vm, tb) == WOLF_RUN_YIELD);
    CHECK(r.messages == 1 && strcmp(r.last, "first") == 0); /* the second waits its turn */
    /* 105 from a parallel thread closes the first */
    wolf_thread_start(ts, &ls, -1, -1);
    CHECK(wolf_vm_run_frame(vm, ts) == WOLF_RUN_DONE && r.closes == 1);
    CHECK(wolf_vm_flag_stop_message(vm));
    CHECK(wolf_vm_run_frame(vm, tb) == WOLF_RUN_YIELD && r.messages == 1); /* still the first's */
    CHECK(wolf_vm_run_frame(vm, ta) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(1)) == 1);
    CHECK(wolf_vm_run_frame(vm, tb) == WOLF_RUN_YIELD && r.messages == 2 && strcmp(r.last, "second") == 0);
    wolf_vm_message_closed(vm);
    CHECK(wolf_vm_run_frame(vm, tb) == WOLF_RUN_DONE && wolf_get(vm, NULL, V(2)) == 1);
    /* answers with nothing up are ignored */
    wolf_vm_message_closed(vm);
    wolf_vm_choice_made(vm, 1);
    CHECK(!wolf_vm_ui_busy(vm));
    wolf_list_free(&la);
    wolf_list_free(&lb);
    wolf_list_free(&ls);
    free(ta);
    free(tb);
    free(ts);
    wolf_vm_destroy(vm);
}

int main(void) {
    test_message();
    test_headless();
    test_choices();
    test_one_window();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("ui: all checks passed");
    return 0;
}
