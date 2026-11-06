/* Tests for self-starting common events (wolf_commonrun.c). The common events are built here;
 * no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_cmd.h"
#include "../src/wolf_commonrun.h"
#include "../src/wolf_data.h"

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
static void cmd(buf *b, int32_t code, const int32_t *ints, int nint) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, 0); u8(b, 0); u8(b, 0);
    b->count++;
}

#define V(x) (2000000 + (x))
static const int32_t ADD = 0x100; /* 121's "+=" */

static void parse(buf *b, wolf_list *l) {
    cmd(b, 0, NULL, 0);
    buf whole = {0};
    u32(&whole, b->count);
    put(&whole, b->p, b->n);
    size_t used;
    CHECK(wolf_list_parse(whole.p, whole.n, &used, l) == WOLF_LIST_OK);
    free(whole.p);
    free(b->p);
    memset(b, 0, sizeof *b);
}

static int log_count;
static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; log_count++; }

/*
 * 0 called only: never starts
 * 1 automatic while V0 == 1: V1 += 1, waits a frame, then sets V0 = 0 (so it runs once)
 * 2 parallel while V2 >= 1: V3 += 1, waits 1 frame, V4 += 1 (cut off when V2 drops)
 * 3 always parallel: V5 += 1 each frame (a wait of 1 between)
 * 4 automatic while V6 == 1: V7 += 1 (blocked while 1 runs)
 * 5 parallel always, broken: a 213 to a label that... is fine; instead an endless loop with no wait
 */
int main(void) {
    wolf_list lists[6];
    buf b = {0};
    cmd(&b, 121, (int32_t[]){V(9), 1, 0, ADD}, 4);
    parse(&b, &lists[0]);
    cmd(&b, 121, (int32_t[]){V(1), 1, 0, ADD}, 4);
    cmd(&b, 180, (int32_t[]){1}, 1);
    cmd(&b, 121, (int32_t[]){V(0), 0, 0, 0}, 4);
    parse(&b, &lists[1]);
    cmd(&b, 121, (int32_t[]){V(3), 1, 0, ADD}, 4);
    cmd(&b, 180, (int32_t[]){1}, 1);
    cmd(&b, 121, (int32_t[]){V(4), 1, 0, ADD}, 4);
    parse(&b, &lists[2]);
    cmd(&b, 121, (int32_t[]){V(5), 1, 0, ADD}, 4);
    cmd(&b, 180, (int32_t[]){1}, 1);
    parse(&b, &lists[3]);
    cmd(&b, 121, (int32_t[]){V(7), 1, 0, ADD}, 4);
    parse(&b, &lists[4]);
    cmd(&b, 170, NULL, 0);
    cmd(&b, 498, NULL, 0);
    parse(&b, &lists[5]);

    wolf_common_event events[6];
    wolf_common table[6];
    memset(events, 0, sizeof events);
    const uint8_t runs[6] = {0, 1, 2, 3, 1, 3};
    const int32_t vars[6] = {0, V(0), V(2), 0, V(6), 0};
    const uint8_t ops[6] = {0, 2, 1, 0, 2, 0}; /* =, >= */
    for (int i = 0; i < 6; i++) {
        events[i].run = runs[i];
        events[i].condition_var = vars[i];
        events[i].condition_value = 1;
        events[i].condition_op = ops[i];
        events[i].return_self = -1;
        table[i].list = &lists[i];
        table[i].name = "";
        table[i].return_self = -1;
    }
    events[5].run = 0; /* the broken one only joins later */
    wolf_commons commons = {0};
    commons.count = 6;
    commons.events = events;
    commons.table = table;

    wolf_host host = {NULL, on_log};
    wolf_config cfg = {20, 0, 1, 1, 1, 0, 1, 0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    wolf_vm_set_common_events(vm, table, 6);
    wolf_commonrun r;
    CHECK(wolf_commonrun_init(&r, vm, &commons) == WOLF_COMMONRUN_OK && r.nslots == 4);

    /* nothing's condition holds but the always-parallel one */
    CHECK(wolf_commonrun_step(&r) == 1 && wolf_get(vm, NULL, V(5)) == 1 && !wolf_commonrun_blocking(&r));
    CHECK(wolf_commonrun_step(&r) == 0 && wolf_get(vm, NULL, V(5)) == 1); /* its wait */
    CHECK(wolf_commonrun_step(&r) == 1 && wolf_get(vm, NULL, V(5)) == 2); /* ended, started again */
    CHECK(wolf_get(vm, NULL, V(9)) == 0); /* called-only never runs */

    /* automatic 1 and 4 both hold: 1 runs, 4 waits for it */
    wolf_set(vm, NULL, V(0), 1);
    wolf_set(vm, NULL, V(6), 1);
    wolf_commonrun_step(&r);
    CHECK(wolf_get(vm, NULL, V(1)) == 1 && wolf_commonrun_blocking(&r) && wolf_commonrun_running(&r, 1));
    CHECK(wolf_get(vm, NULL, V(7)) == 0 && !wolf_commonrun_running(&r, 4));
    wolf_commonrun_step(&r); /* 1 finishes (V0 = 0), and 4 may start in the same frame once it has */
    CHECK(wolf_get(vm, NULL, V(0)) == 0 && !wolf_commonrun_running(&r, 1));
    wolf_commonrun_step(&r);
    CHECK(wolf_get(vm, NULL, V(1)) == 1 && wolf_get(vm, NULL, V(7)) >= 1); /* 1 did not run again; 4 ran */
    wolf_set(vm, NULL, V(6), 0);

    /* parallel 2: starts, and is cut off mid-run when its condition stops holding */
    wolf_set(vm, NULL, V(2), 1);
    wolf_commonrun_step(&r);
    CHECK(wolf_get(vm, NULL, V(3)) == 1 && wolf_commonrun_running(&r, 2));
    wolf_set(vm, NULL, V(2), 0);
    wolf_commonrun_step(&r);
    CHECK(!wolf_commonrun_running(&r, 2) && wolf_get(vm, NULL, V(4)) == 0);
    /* back on: from the top */
    wolf_set(vm, NULL, V(2), 5);
    wolf_commonrun_step(&r);
    CHECK(wolf_get(vm, NULL, V(3)) == 2);
    /* a map change stops every run */
    wolf_commonrun_map_changed(&r);
    CHECK(!wolf_commonrun_running(&r, 2) && !wolf_commonrun_running(&r, 3));
    wolf_commonrun_free(&r);

    /* a run that fails is not started again until the map changes */
    events[5].run = 3;
    CHECK(wolf_commonrun_init(&r, vm, &commons) == WOLF_COMMONRUN_OK && r.nslots == 5);
    log_count = 0;
    wolf_commonrun_step(&r);
    CHECK(!wolf_commonrun_running(&r, 5) && log_count >= 1);
    int before = log_count;
    wolf_commonrun_step(&r);
    CHECK(log_count == before); /* not tried again */
    wolf_commonrun_map_changed(&r);
    wolf_commonrun_step(&r);
    CHECK(log_count > before); /* tried again after the map change */
    wolf_commonrun_free(&r);

    /* no self-starting events at all */
    for (int i = 0; i < 6; i++) events[i].run = 0;
    CHECK(wolf_commonrun_init(&r, vm, &commons) == WOLF_COMMONRUN_OK && r.nslots == 0 && wolf_commonrun_step(&r) == 0);
    wolf_commonrun_free(&r);

    wolf_vm_destroy(vm);
    for (int i = 0; i < 6; i++) wolf_list_free(&lists[i]);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("commonrun: all checks passed");
    return 0;
}
