/* Tests for command 250 and the database call values. The databases are built here from the
 * layouts in docs/behaviour/formats.md; no game data is used. */
#include "../src/vm.h"
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
}

static char last_log[600];
static int log_count;
static void on_log(void *ctx, int level, const char *m) {
    (void)ctx; (void)level;
    snprintf(last_log, sizeof last_log, "%s", m);
    log_count++;
}

/* One database: type 0 "Items" with fields Price (number), Name (text), Stock (number) and rows
 * Potion, Ether; type 1 "Spare", one number field "N", one row. The numbers start at
 * base, base+1, ... so the three databases hold different values. */
static int make_db(wolf_db *db, int32_t base) {
    buf p = {0};
    u32(&p, 2);
    text(&p, "Items");
    u32(&p, 3); text(&p, "Price"); text(&p, "Name"); text(&p, "Stock");
    u32(&p, 2); text(&p, "Potion"); text(&p, "Ether");
    text(&p, "");
    u32(&p, 3); u8(&p, 0); u8(&p, 0); u8(&p, 0);
    u32(&p, 3); text(&p, ""); text(&p, ""); text(&p, "");
    u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
    text(&p, "Spare");
    u32(&p, 1); text(&p, "N");
    u32(&p, 1); text(&p, "Only");
    text(&p, "");
    u32(&p, 1); u8(&p, 0);
    u32(&p, 1); text(&p, "");
    u32(&p, 1); u32(&p, 0);
    u32(&p, 1); u32(&p, 0);
    u32(&p, 1); u32(&p, 0);

    static const uint8_t head[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};
    buf d = {0};
    put(&d, head, sizeof head);
    u8(&d, 0xC1);
    u32(&d, 2);
    u32(&d, 0xFFFFFFFEu); u32(&d, 0);
    u32(&d, 3); u32(&d, 1000); u32(&d, 2000); u32(&d, 1001);
    u32(&d, 2);
    u32(&d, (uint32_t)base); u32(&d, (uint32_t)(base + 1)); text(&d, "Potion");
    u32(&d, (uint32_t)(base + 2)); u32(&d, (uint32_t)(base + 3)); text(&d, "Ether");
    u32(&d, 0xFFFFFFFEu); u32(&d, 0);
    u32(&d, 1); u32(&d, 1000);
    u32(&d, 1); u32(&d, (uint32_t)(base + 100));
    u8(&d, 0xC1);
    int r = wolf_db_load(p.p, p.n, d.p, d.n, db);
    free(p.p);
    free(d.p);
    return r;
}

static const wolf_config CFG = {100, 10, 20, 20, 20, 4, 1};

typedef struct {
    wolf_vm *vm;
    wolf_db user, changeable, system;
} world;

static void world_open(world *w) {
    wolf_host host = {NULL, on_log};
    CHECK(make_db(&w->user, 10) == WOLF_READ_OK);
    CHECK(make_db(&w->changeable, 20) == WOLF_READ_OK);
    CHECK(make_db(&w->system, 30) == WOLF_READ_OK);
    w->vm = wolf_vm_create(&host, &CFG);
    wolf_vm_set_databases(w->vm, &w->user, &w->changeable, &w->system);
}

static void world_close(world *w) {
    wolf_vm_destroy(w->vm);
    wolf_db_free(&w->user);
    wolf_db_free(&w->changeable);
    wolf_db_free(&w->system);
}

/* Runs one list of commands to its end. */
static void run(world *w, const buf *body, uint32_t count) {
    buf b = {0};
    u32(&b, count + 1);
    put(&b, body->p, body->n);
    cmd(&b, 0, NULL, 0, NULL);
    wolf_list l;
    size_t used;
    CHECK(wolf_list_parse(b.p, b.n, &used, &l) == WOLF_LIST_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_thread_start(t, &l, -1, -1);
    CHECK(wolf_vm_run_frame(w->vm, t) == WOLF_RUN_DONE);
    free(t);
    wolf_list_free(&l);
    free(b.p);
}

/* 250's packed word: assignment, options, database (0 changeable, 1 system, 2 user), operation
 * (0 write, 1 read), name boxes. */
static int32_t word(int how, int options, int which, int op, int names) {
    return (int32_t)((uint32_t)options | (uint32_t)how << 4 | (uint32_t)which << 8 | (uint32_t)op << 12 |
                     (uint32_t)names << 16);
}

#define V(x) (2000000 + (x))
#define S(x) (3000000 + (x))
#define DB_USER(t, d, f) (1000000000 + (t) * 1000000 + (d) * 100 + (f))
#define DB_CHANGE(t, d, f) (1100000000 + (t) * 1000000 + (d) * 100 + (f))
#define DB_SYSTEM(t, d, f) (1300000000 + (t) * 1000000 + (d) * 100 + (f))

static void test_call_values(void) {
    world w;
    world_open(&w);
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 0, 0)) == 10);
    CHECK(wolf_get(w.vm, NULL, DB_CHANGE(0, 1, 2)) == 23);
    CHECK(wolf_get(w.vm, NULL, DB_SYSTEM(1, 0, 0)) == 130);
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_CHANGE(0, 1, 1)), "Ether") == 0);
    /* writes land in that database only */
    wolf_set(w.vm, NULL, DB_CHANGE(0, 0, 0), 777);
    CHECK(wolf_get(w.vm, NULL, DB_CHANGE(0, 0, 0)) == 777 && wolf_get(w.vm, NULL, DB_USER(0, 0, 0)) == 10);
    wolf_set_str(w.vm, NULL, DB_SYSTEM(0, 0, 1), "Elixir");
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_SYSTEM(0, 0, 1)), "Elixir") == 0);
    /* a value from the database used as an operand */
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 1, 2)) == 13);
    /* the wrong kind, a missing row, field or type, and the 1,200,000,000s read as 0 / "" */
    log_count = 0;
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 0, 1)) == 0);
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_USER(0, 0, 0)), "") == 0);
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 2, 0)) == 0);
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 0, 3)) == 0);
    CHECK(wolf_get(w.vm, NULL, DB_USER(5, 0, 0)) == 0);
    CHECK(wolf_get(w.vm, NULL, 1200000000) == 0);
    CHECK(wolf_get(w.vm, NULL, 2147483647) == 0);
    wolf_set(w.vm, NULL, DB_USER(0, 0, 1), 5); /* ignored: a text field */
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_USER(0, 0, 1)), "Potion") == 0);
    CHECK(log_count == 1); /* logged once */
    /* with no databases set, everything reads 0 */
    wolf_vm_set_databases(w.vm, NULL, NULL, NULL);
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 0, 0)) == 0);
    world_close(&w);
}

static void test_read_write(void) {
    world w;
    world_open(&w);
    buf b = {0};
    uint32_t n = 0;
    /* read: V0 = user Items Ether Price; V1 += changeable Items Potion Stock */
    int32_t r0[] = {0, 1, 0, word(0, 0, 2, 1, 0), V(0)};
    cmd(&b, 250, r0, 5, NULL); n++;
    int32_t r1[] = {0, 0, 2, word(1, 0, 0, 1, 0), V(1)};
    cmd(&b, 250, r1, 5, NULL); n++;
    cmd(&b, 250, r1, 5, NULL); n++;
    /* write: system Spare Only N -= 30 (from a variable holding 30) */
    int32_t w2[] = {1, 0, 0, word(2, 0, 1, 0, 0), V(2)};
    cmd(&b, 121, (int32_t[]){V(2), 30, 0, 0}, 4, NULL); n++;
    cmd(&b, 250, w2, 5, NULL); n++;
    /* the type, data and field may be call values */
    cmd(&b, 121, (int32_t[]){V(3), 1, 0, 0}, 4, NULL); n++;
    int32_t r3[] = {0, V(3), 2, word(0, 0, 0, 1, 0), V(4)};
    cmd(&b, 250, r3, 5, NULL); n++;
    /* text: read into a string variable, append, write a literal, write from a variable */
    int32_t t0[] = {0, 0, 1, word(0, 0, 2, 1, 0), S(0)};
    cmd(&b, 250, t0, 5, NULL); n++;
    int32_t t1[] = {0, 1, 1, word(1, 0, 2, 1, 0), S(0)};
    cmd(&b, 250, t1, 5, NULL); n++;
    const char *lit[] = {"Hi-Potion", "", "", "", NULL};
    int32_t t2[] = {0, 0, 1, word(0, 2, 0, 0, 0)};
    cmd(&b, 250, t2, 4, lit); n++;
    int32_t t3[] = {0, 1, 1, word(1, 0, 0, 0, 0), S(0)};
    cmd(&b, 250, t3, 5, NULL); n++;
    run(&w, &b, n);

    CHECK(wolf_get(w.vm, NULL, V(0)) == 12);
    CHECK(wolf_get(w.vm, NULL, V(1)) == 21 * 2); /* Potion's Stock, added twice */
    CHECK(wolf_get(w.vm, NULL, DB_SYSTEM(1, 0, 0)) == 130 - 30);
    CHECK(wolf_get(w.vm, NULL, V(4)) == 23);
    CHECK(strcmp(wolf_get_str(w.vm, NULL, S(0)), "PotionEther") == 0);
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_CHANGE(0, 0, 1)), "Hi-Potion") == 0);
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_CHANGE(0, 1, 1)), "EtherPotionEther") == 0);
    /* the user and system copies are untouched by changeable writes */
    CHECK(strcmp(wolf_get_str(w.vm, NULL, DB_USER(0, 0, 1)), "Potion") == 0);
    free(b.p);
    world_close(&w);
}

static void test_overflow_and_names(void) {
    world w;
    world_open(&w);
    buf b = {0};
    uint32_t n = 0;
    /* field 102 of data 0 is field 2 of data 1; data 10000 of type 0 is data 0 of type 1 */
    int32_t o1[] = {0, 0, 102, word(0, 0, 2, 1, 0), V(0)};
    cmd(&b, 250, o1, 5, NULL); n++;
    int32_t o2[] = {0, 10000, 0, word(0, 0, 2, 1, 0), V(1)};
    cmd(&b, 250, o2, 5, NULL); n++;
    /* name boxes: the names decide when their bits are set */
    const char *names[] = {"", "Items", "Ether", "Stock", NULL};
    int32_t n1[] = {1, 0, 0, word(0, 0, 2, 1, 7), V(2)};
    cmd(&b, 250, n1, 5, names); n++;
    /* a set bit with an unknown name keeps the stored number */
    const char *unknown[] = {"", "", "Nobody", "", NULL};
    int32_t n2[] = {0, 0, 0, word(0, 0, 2, 1, 2), V(3)};
    cmd(&b, 250, n2, 5, unknown); n++;
    /* the strings without their bits are only labels */
    int32_t n3[] = {0, 0, 0, word(0, 0, 2, 1, 0), V(4)};
    cmd(&b, 250, n3, 5, names); n++;
    run(&w, &b, n);
    CHECK(wolf_get(w.vm, NULL, V(0)) == 13);
    CHECK(wolf_get(w.vm, NULL, V(1)) == 110);
    CHECK(wolf_get(w.vm, NULL, V(2)) == 13);
    CHECK(wolf_get(w.vm, NULL, V(3)) == 10);
    CHECK(wolf_get(w.vm, NULL, V(4)) == 10);
    free(b.p);
    world_close(&w);
}

static void test_refusals(void) {
    world w;
    world_open(&w);
    buf b = {0};
    uint32_t n = 0;
    int32_t bad[][5] = {
        {0, 0, 0, word(0, 0, 3, 1, 0), V(0)},  /* no database 3 */
        {0, 0, 0, word(0, 0, 2, 5, 0), V(0)},  /* an operation not built */
        {0, 5, 0, word(0, 0, 2, 1, 0), V(0)},  /* no such data */
        {0, -1, 0, word(0, 0, 2, 1, 0), V(0)}, /* negative */
        {0, 0, 0, word(9, 0, 2, 1, 0), V(0)},  /* assignment 9 (atan): not for 250 */
        {0, 0, 0, word(0, 2, 2, 0, 0), V(0)},  /* a literal text into a number field */
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) { cmd(&b, 250, bad[i], 5, NULL); n++; }
    cmd(&b, 250, (int32_t[]){0, 0, 0}, 3, NULL); n++;                         /* too short */
    cmd(&b, 250, (int32_t[]){0, 0, 0, word(0, 0, 2, 1, 0)}, 4, NULL); n++;     /* no target */
    cmd(&b, 121, (int32_t[]){V(0), 4, 0, 0}, 4, NULL); n++;
    run(&w, &b, n);
    CHECK(wolf_get(w.vm, NULL, V(0)) == 4); /* nothing above wrote, and the list ran on */
    CHECK(wolf_get(w.vm, NULL, DB_USER(0, 0, 0)) == 10);
    /* a database the game did not load */
    wolf_vm_set_databases(w.vm, &w.user, NULL, &w.system);
    buf c = {0};
    cmd(&c, 250, (int32_t[]){0, 0, 0, word(0, 0, 0, 1, 0), V(5)}, 5, NULL);
    run(&w, &c, 1);
    CHECK(wolf_get(w.vm, NULL, V(5)) == 0);
    free(b.p);
    free(c.p);
    world_close(&w);
}

int main(void) {
    test_call_values();
    test_read_write();
    test_overflow_and_names();
    test_refusals();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("db: all checks passed");
    return 0;
}
