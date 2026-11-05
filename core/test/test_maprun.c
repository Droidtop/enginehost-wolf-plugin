/* Tests for page choice, the map runtime, and the TileSetData.dat and MapTree.dat loaders. Maps
 * are built in memory and files from docs/behaviour/formats.md and maps.md; no game data is
 * used. */
#include "../src/vm.h"
#include "../src/wolf_data.h"
#include "../src/wolf_map.h"
#include "../src/wolf_maprun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

static int log_count;
static char last_log[600];
static void on_log(void *ctx, int level, const char *m) {
    (void)ctx; (void)level;
    snprintf(last_log, sizeof last_log, "%s", m);
    log_count++;
}

#define VAR(x) (2000000 + (x))
#define SELF(x) (1100000 + (x))
#define SYS(x) (9000000 + (x))
#define ADD 0x100 /* 121's assignment "+=" (commands.md) */

/* ---- lists ---------------------------------------------------------------------------------- */

static const int32_t add_v0[] = {VAR(0), 1, 0, ADD}, add_v1[] = {VAR(1), 1, 0, ADD}, add_v2[] = {VAR(2), 1, 0, ADD};
static const int32_t add_v5[] = {VAR(5), 1, 0, ADD}, add_self0[] = {SELF(0), 1, 0, ADD}, wait2[] = {2};

/* var 0 += 1, wait 2 frames, var 1 += 1 */
static wolf_cmd waiting_cmds[] = {
    {121, 0, 4, 0, add_v0, NULL, NULL}, {180, 0, 1, 0, wait2, NULL, NULL}, {121, 0, 4, 0, add_v1, NULL, NULL}};
static wolf_cmd add_v2_cmds[] = {{121, 0, 4, 0, add_v2, NULL, NULL}};
static wolf_cmd add_v5_cmds[] = {{121, 0, 4, 0, add_v5, NULL, NULL}};
static wolf_cmd add_self0_cmds[] = {{121, 0, 4, 0, add_self0, NULL, NULL}};
/* a loop with nothing in it: the frame budget runs out */
static wolf_cmd forever_cmds[] = {{170, 0, 0, 0, NULL, NULL, NULL}, {498, 0, 0, 0, NULL, NULL, NULL}};

#define LIST(a) ((wolf_list){a, sizeof a / sizeof a[0], NULL})

static wolf_page_condition cond(uint8_t flags, uint8_t op, int32_t variable, int32_t value) {
    return (wolf_page_condition){flags, op, variable, value};
}

static wolf_map_page page(uint8_t trigger, wolf_list list) {
    wolf_map_page p;
    memset(&p, 0, sizeof p);
    p.trigger = trigger;
    p.list = list;
    for (int i = 0; i < 4; i++) p.cond[i] = cond(0, 0, 1000000, 0); /* unused, as the editor stores it */
    p.transfer = -1;
    return p;
}

static wolf_vm *make_vm(void) {
    wolf_config cfg = {64, 4, 4, 64, 4, 16, 1};
    wolf_host host = {NULL, on_log};
    return wolf_vm_create(&host, &cfg);
}

static wolf_map one_event(wolf_map_event *e) {
    wolf_map m;
    memset(&m, 0, sizeof m);
    m.nevents = 1;
    m.events = e;
    return m;
}

/* ---- page choice ---------------------------------------------------------------------------- */

static void test_each_comparison(void) {
    wolf_vm *vm = make_vm();
    wolf_maprun r;
    CHECK(wolf_maprun_init(&r, vm) == WOLF_MAPRUN_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_set(vm, t, VAR(3), 5);
    /* op, value, expected with var 3 = 5 */
    static const struct { uint8_t op; int32_t value; int want; } cases[] = {
        {0, 4, 1}, {0, 5, 0}, {1, 5, 1}, {1, 6, 0}, {2, 5, 1}, {2, 4, 0}, {3, 5, 1}, {3, 4, 0},
        {4, 6, 1}, {4, 5, 0}, {5, 4, 1}, {5, 5, 0}, {6, 4, 1}, {6, 2, 0},
    };
    wolf_map_page pg = page(WOLF_PAGE_CONFIRM, LIST(add_v2_cmds));
    wolf_map_event e = {0, "", 0, 0, 1, &pg};
    wolf_map m = one_event(&e);
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pg.cond[1] = cond(0x01, cases[i].op, VAR(3), cases[i].value);
        CHECK(wolf_maprun_active_page(&r, 0) == (cases[i].want ? 0 : -1));
    }
    /* the other low bits do not turn a condition off, and an unused one is not read */
    pg.cond[1] = cond(0x0F, 2, VAR(3), 5);
    CHECK(wolf_maprun_active_page(&r, 0) == 0);
    pg.cond[1] = cond(0x0E, 2, VAR(3), 4);
    CHECK(wolf_maprun_active_page(&r, 0) == 0);
    /* an unknown comparison is false, and logged once */
    int before = log_count;
    pg.cond[1] = cond(0x01, 9, VAR(3), 5);
    CHECK(wolf_maprun_active_page(&r, 0) == -1);
    CHECK(wolf_maprun_active_page(&r, 0) == -1);
    CHECK(log_count == before + 1 && strstr(last_log, "comparison 9") != NULL);
    /* the value is a call value too: a variable compared with a variable */
    wolf_set(vm, t, VAR(4), 5);
    pg.cond[1] = cond(0x01, 2, VAR(3), VAR(4));
    CHECK(wolf_maprun_active_page(&r, 0) == 0);
    wolf_set(vm, t, VAR(4), 6);
    CHECK(wolf_maprun_active_page(&r, 0) == -1);
    /* every used condition must hold: all four */
    pg.cond[0] = cond(0x01, 2, VAR(3), 5);
    pg.cond[1] = cond(0x01, 4, VAR(3), 6);
    pg.cond[2] = cond(0x01, 5, VAR(3), 0);
    pg.cond[3] = cond(0x01, 1, VAR(3), 5);
    CHECK(wolf_maprun_active_page(&r, 0) == 0);
    pg.cond[3] = cond(0x01, 0, VAR(3), 5);
    CHECK(wolf_maprun_active_page(&r, 0) == -1);
    free(t);
    wolf_maprun_free(&r);
    wolf_vm_destroy(vm);
}

static void test_kinds_and_ties(void) {
    wolf_vm *vm = make_vm();
    wolf_maprun r;
    CHECK(wolf_maprun_init(&r, vm) == WOLF_MAPRUN_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    CHECK(wolf_maprun_active_page(&r, 0) == -1); /* no map */

    wolf_map_page a[3], b[2];
    a[0] = page(WOLF_PAGE_CONFIRM, LIST(add_v2_cmds)); /* no conditions: always qualifies */
    a[1] = page(WOLF_PAGE_CONFIRM, LIST(add_v2_cmds));
    a[1].cond[2] = cond(0x01, 2, SYS(7), 1); /* a system variable */
    a[2] = page(WOLF_PAGE_CONFIRM, LIST(add_v2_cmds));
    a[2].cond[0] = cond(0x01, 1, SELF(4), 2); /* this event's own self variable */
    b[0] = page(WOLF_PAGE_CONFIRM, LIST(add_v2_cmds));
    b[0].cond[0] = cond(0x01, 2, VAR(1), 1);
    b[1] = page(WOLF_PAGE_CONFIRM, LIST(add_v2_cmds));
    b[1].cond[0] = cond(0x01, 1, SELF(4), 2); /* the same slot, but event 9's */
    wolf_map_event ev[2] = {{2, "A", 0, 0, 3, a}, {9, "B", 1, 0, 2, b}};
    wolf_map m;
    memset(&m, 0, sizeof m);
    m.nevents = 2;
    m.events = ev;
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);

    CHECK(wolf_maprun_active_page(&r, 0) == 0);
    CHECK(wolf_maprun_active_page(&r, 1) == -1); /* none qualifies */
    CHECK(wolf_maprun_active_page(&r, 2) == -1); /* out of range */
    wolf_set(vm, t, SYS(7), 1);
    CHECK(wolf_maprun_active_page(&r, 0) == 1); /* a later page that qualifies wins */
    t->map_event = 2;
    wolf_set(vm, t, SELF(4), 2);
    CHECK(wolf_maprun_active_page(&r, 0) == 2); /* ties: the last qualifying page */
    CHECK(wolf_maprun_active_page(&r, 1) == -1); /* event 9's slot is still 0 */
    wolf_set(vm, t, VAR(1), 1);
    CHECK(wolf_maprun_active_page(&r, 1) == 0);
    t->map_event = 9;
    wolf_set(vm, t, SELF(4), 3);
    CHECK(wolf_maprun_active_page(&r, 1) == 1);
    wolf_set(vm, t, SYS(7), 0);
    CHECK(wolf_maprun_active_page(&r, 0) == 2); /* page 2 does not depend on page 1 */
    t->map_event = 2;
    wolf_set(vm, t, SELF(4), 1);
    CHECK(wolf_maprun_active_page(&r, 0) == 0);
    free(t);
    wolf_maprun_free(&r);
    wolf_vm_destroy(vm);
}

/* ---- the runtime ---------------------------------------------------------------------------- */

static void test_runs(void) {
    wolf_vm *vm = make_vm();
    wolf_maprun r;
    CHECK(wolf_maprun_init(&r, vm) == WOLF_MAPRUN_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    CHECK(wolf_maprun_step(&r) == 0); /* no map: nothing */

    wolf_map_page auto_p = page(WOLF_PAGE_AUTO, LIST(waiting_cmds));
    wolf_map_page par_p = page(WOLF_PAGE_PARALLEL, LIST(add_v2_cmds));
    wolf_map_page confirm_p = page(WOLF_PAGE_CONFIRM, LIST(add_v5_cmds));
    wolf_map_page touch_p = page(WOLF_PAGE_HERO_TOUCH, LIST(add_v5_cmds));
    wolf_map_page gated = page(WOLF_PAGE_PARALLEL, LIST(add_v5_cmds));
    gated.cond[0] = cond(0x01, 2, VAR(9), 1);
    /* event 7: page 1 counts its own self variable 0; page 2 takes over once it reaches 3 */
    wolf_map_page own[2] = {page(WOLF_PAGE_PARALLEL, LIST(add_self0_cmds)), page(WOLF_PAGE_PARALLEL, LIST(add_v5_cmds))};
    own[1].cond[0] = cond(0x01, 1, SELF(0), 3);
    wolf_map_event ev[6] = {
        {0, "auto", 0, 0, 1, &auto_p},   {3, "parallel", 0, 0, 1, &par_p}, {4, "confirm", 0, 0, 1, &confirm_p},
        {5, "touch", 0, 0, 1, &touch_p}, {6, "gated", 0, 0, 1, &gated},    {7, "own", 0, 0, 2, own},
    };
    wolf_map m;
    memset(&m, 0, sizeof m);
    m.nevents = 6;
    m.events = ev;
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);
    CHECK(!wolf_maprun_blocking(&r));

    /* frame 1: the automatic page starts and waits; the parallel one runs to its end */
    CHECK(wolf_maprun_step(&r) == 1);
    CHECK(wolf_get(vm, t, VAR(0)) == 1 && wolf_get(vm, t, VAR(1)) == 0 && wolf_get(vm, t, VAR(2)) == 1);
    CHECK(wolf_maprun_running(&r, 0) && !wolf_maprun_running(&r, 1) && wolf_maprun_blocking(&r));
    CHECK(wolf_get(vm, t, VAR(5)) == 0); /* confirm, touch and the gated page did not run */
    /* frame 2: still waiting; the parallel page starts again */
    CHECK(wolf_maprun_step(&r) == 1);
    CHECK(wolf_get(vm, t, VAR(0)) == 1 && wolf_get(vm, t, VAR(1)) == 0 && wolf_get(vm, t, VAR(2)) == 2);
    /* frame 3: the wait is over and the automatic run ends */
    CHECK(wolf_maprun_step(&r) == 0);
    CHECK(wolf_get(vm, t, VAR(0)) == 1 && wolf_get(vm, t, VAR(1)) == 1 && wolf_get(vm, t, VAR(2)) == 3);
    CHECK(!wolf_maprun_blocking(&r));
    /* event 7 has counted to 3 and shows its second page from now on */
    t->map_event = 7;
    CHECK(wolf_get(vm, t, SELF(0)) == 3 && wolf_maprun_active_page(&r, 5) == 1);
    /* frame 4: its condition still holds, so the automatic page starts again; event 7 runs page 2;
     * the gated page opens */
    wolf_set(vm, t, VAR(9), 1);
    CHECK(wolf_maprun_step(&r) == 1);
    CHECK(wolf_get(vm, t, VAR(0)) == 2 && wolf_get(vm, t, SELF(0)) == 3 && wolf_get(vm, t, VAR(5)) == 2);
    CHECK(wolf_maprun_blocking(&r));

    /* leaving the map drops every run; variables stay */
    CHECK(wolf_maprun_enter(&r, NULL) == WOLF_MAPRUN_OK);
    CHECK(!wolf_maprun_running(&r, 0) && !wolf_maprun_blocking(&r) && wolf_maprun_step(&r) == 0);
    CHECK(wolf_get(vm, t, VAR(0)) == 2);
    /* another map: none of the first map's runs carry over, even under the same event index */
    wolf_map_page other_p = page(WOLF_PAGE_PARALLEL, LIST(add_v5_cmds));
    wolf_map_event other = {0, "other", 0, 0, 1, &other_p};
    wolf_map m2 = one_event(&other);
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);
    CHECK(wolf_maprun_step(&r) == 1 && wolf_maprun_running(&r, 0)); /* auto page waiting again */
    CHECK(wolf_get(vm, t, VAR(0)) == 3);
    int32_t v5 = wolf_get(vm, t, VAR(5));
    CHECK(wolf_maprun_enter(&r, &m2) == WOLF_MAPRUN_OK);
    CHECK(!wolf_maprun_running(&r, 0) && !wolf_maprun_blocking(&r));
    CHECK(wolf_maprun_step(&r) == 0); /* index 0 is now the other map's parallel event */
    CHECK(wolf_get(vm, t, VAR(5)) == v5 + 1 && wolf_get(vm, t, VAR(1)) == 1); /* the auto run never resumed */
    /* back again: the automatic page starts from its first command */
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);
    CHECK(wolf_maprun_step(&r) == 1 && wolf_get(vm, t, VAR(0)) == 4);

    /* a page that stops showing: its run is not stepped while another page is active */
    wolf_set(vm, t, VAR(9), 0);
    auto_p.cond[0] = cond(0x01, 2, VAR(9), 1);
    CHECK(wolf_maprun_active_page(&r, 0) == -1);
    CHECK(wolf_maprun_step(&r) == 1 && wolf_maprun_running(&r, 0));
    CHECK(wolf_maprun_step(&r) == 1 && wolf_get(vm, t, VAR(1)) == 1);
    wolf_set(vm, t, VAR(9), 1);
    CHECK(wolf_maprun_step(&r) == 1); /* the wait's second frame */
    CHECK(wolf_maprun_step(&r) == 0 && wolf_get(vm, t, VAR(1)) == 2);

    free(t);
    wolf_maprun_free(&r);
    wolf_vm_destroy(vm);
}

static void test_failures(void) {
    wolf_vm *vm = make_vm();
    wolf_maprun r;
    CHECK(wolf_maprun_init(&r, vm) == WOLF_MAPRUN_OK);
    wolf_thread *t = calloc(1, sizeof *t);
    wolf_map_page loop_p = page(WOLF_PAGE_PARALLEL, LIST(forever_cmds));
    wolf_map_page far_p = page(WOLF_PAGE_AUTO, LIST(add_v5_cmds));
    wolf_map_page ok_p = page(WOLF_PAGE_PARALLEL, LIST(add_v2_cmds));
    wolf_map_event ev[3] = {{1, "loop", 0, 0, 1, &loop_p}, {16, "no slot", 0, 0, 1, &far_p}, {2, "ok", 0, 0, 1, &ok_p}};
    wolf_map m;
    memset(&m, 0, sizeof m);
    m.nevents = 3;
    m.events = ev;
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);
    int before = log_count;
    CHECK(wolf_maprun_step(&r) == 0);
    CHECK(log_count >= before + 3); /* the budget, the stopped run, the missing slot */
    CHECK(strstr(last_log, "no self-variable slots") != NULL);
    CHECK(wolf_get(vm, t, VAR(2)) == 1 && wolf_get(vm, t, VAR(5)) == 0);
    /* neither is tried again on this map; the others carry on */
    before = log_count;
    CHECK(wolf_maprun_step(&r) == 0);
    CHECK(log_count == before && wolf_get(vm, t, VAR(2)) == 2);
    CHECK(!wolf_maprun_blocking(&r));
    /* entering the map again gives the failed run another go */
    CHECK(wolf_maprun_enter(&r, &m) == WOLF_MAPRUN_OK);
    CHECK(wolf_maprun_step(&r) == 0 && log_count > before);
    /* an empty map */
    wolf_map empty;
    memset(&empty, 0, sizeof empty);
    CHECK(wolf_maprun_enter(&r, &empty) == WOLF_MAPRUN_OK && wolf_maprun_step(&r) == 0);
    CHECK(wolf_maprun_active_page(&r, 0) == -1 && !wolf_maprun_running(&r, 0));
    free(t);
    wolf_maprun_free(&r);
    wolf_vm_destroy(vm);
}

/* ---- loaders -------------------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void u8(buf *b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u32(buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void text(buf *b, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; u32(b, n); put(b, s, n); }

static uint32_t rng = 777;
static uint32_t next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

static buf tileset_file(int utf8, int autotiles) {
    buf b = {0};
    static const uint8_t magic[10] = {0, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};
    put(&b, magic, sizeof magic);
    if (utf8) b.p[6] = 0x55;
    u8(&b, 0x02); /* version */
    u32(&b, 2);
    /* tileset 0: three autotiles, four chips */
    text(&b, "Field");
    text(&b, "MapChip/field.png");
    for (int i = 0; i < autotiles; i++) text(&b, i == 1 ? "" : "MapChip/water.png");
    u8(&b, 0xFF);
    u32(&b, 4);
    u8(&b, 0); u8(&b, 3); u8(&b, 0); u8(&b, 255);
    u8(&b, 0xFF);
    u32(&b, 4);
    u32(&b, 0); u32(&b, 0x0F); u32(&b, 0x110); u32(&b, 0x80000201);
    /* tileset 1: nothing */
    text(&b, "");
    text(&b, "");
    u8(&b, 0xFF);
    u32(&b, 0);
    u8(&b, 0xFF);
    u32(&b, 0);
    u8(&b, 0xCF);
    return b;
}

static int load_tilesets(const uint8_t *p, size_t n) {
    wolf_tilesets t;
    int r = wolf_tilesets_load(p, n, &t);
    if (r == WOLF_READ_OK) wolf_tilesets_free(&t);
    else CHECK(t.sets == NULL);
    return r;
}

static void abuse(const buf *f, int (*load)(const uint8_t *, size_t), int fuzz) {
    for (size_t k = 0; k < f->n; k++) CHECK(load(f->p, k) != WOLF_READ_OK);
    uint8_t *m = malloc(f->n + 1);
    memcpy(m, f->p, f->n);
    m[f->n] = 0;
    CHECK(load(m, f->n + 1) != WOLF_READ_OK); /* a byte too many */
    m[1] = 0x50;
    CHECK(load(m, f->n) == WOLF_READ_E_PROTECTED);
    for (int i = 0; i < fuzz; i++) {
        memcpy(m, f->p, f->n);
        int edits = 1 + (int)(next_rand() % 4);
        for (int e = 0; e < edits; e++) m[next_rand() % f->n] = (uint8_t)next_rand();
        load(m, f->n);
    }
    free(m);
}

static void test_tilesets(void) {
    for (int utf8 = 0; utf8 < 2; utf8++) {
        buf f = tileset_file(utf8, 3);
        wolf_tilesets t;
        CHECK(wolf_tilesets_load(f.p, f.n, &t) == WOLF_READ_OK);
        CHECK(t.utf8 == utf8 && t.version == 2 && t.count == 2);
        if (t.count == 2) {
            const wolf_tileset *s = &t.sets[0];
            CHECK(strcmp(s->name, "Field") == 0 && strcmp(s->base, "MapChip/field.png") == 0);
            CHECK(s->nautotiles == 3 && strcmp(s->autotiles[0], "MapChip/water.png") == 0 && s->autotiles[1][0] == 0);
            CHECK(s->nchips == 4 && s->tags[1] == 3 && s->tags[3] == 255);
            CHECK(s->flags[1] == (WOLF_CHIP_NOT_DOWN | WOLF_CHIP_NOT_LEFT | WOLF_CHIP_NOT_RIGHT | WOLF_CHIP_NOT_UP));
            CHECK(s->flags[2] == (WOLF_CHIP_HIDES | WOLF_CHIP_ABOVE) && s->flags[3] == 0x80000201u);
            CHECK(t.sets[1].nautotiles == 0 && t.sets[1].nchips == 0 && t.sets[1].name[0] == 0);
        }
        wolf_tilesets_free(&t);
        CHECK(t.sets == NULL && t.count == 0);
        abuse(&f, load_tilesets, 20000);
        free(f.p);
    }
    /* the tag and flag counts must agree */
    buf f = tileset_file(0, 1);
    size_t at = 10 + 1 + 4 + (4 + 6) + (4 + 18) + (4 + 18) + 1 + 4 + 4 + 1; /* tileset 0's flag count */
    CHECK(f.p[at] == 4 && f.p[at - 1] == 0xFF);
    f.p[at] = 3;
    CHECK(load_tilesets(f.p, f.n) == WOLF_READ_E_FORMAT);
    free(f.p);
    /* the autotile list is capped */
    buf big = tileset_file(0, 257);
    CHECK(load_tilesets(big.p, big.n) == WOLF_READ_E_FORMAT);
    free(big.p);
    buf most = tileset_file(0, 256);
    CHECK(load_tilesets(most.p, most.n) == WOLF_READ_OK);
    free(most.p);
    /* a scrambled (non-zero indicator) file is refused */
    buf s = tileset_file(0, 1);
    s.p[0] = 7;
    CHECK(load_tilesets(s.p, s.n) == WOLF_READ_E_PROTECTED);
    free(s.p);
    /* a count far beyond the bytes */
    buf c = tileset_file(0, 1);
    c.p[11] = 0xFF; c.p[12] = 0xFF;
    CHECK(load_tilesets(c.p, c.n) == WOLF_READ_E_FORMAT);
    free(c.p);
}

static buf tree_file(uint32_t n) {
    buf b = {0};
    for (int i = 0; i < 10; i++) u8(&b, (uint32_t)i * 3); /* the head: meaning Unknown, not checked */
    u8(&b, 0x66);
    u32(&b, n);
    for (uint32_t i = 0; i < n; i++) {
        u32(&b, i == 0 ? (uint32_t)-1 : i / 2);
        u32(&b, i + 1);
    }
    u8(&b, 0xBC);
    return b;
}

static int load_tree(const uint8_t *p, size_t n) {
    wolf_map_tree t;
    int r = wolf_map_tree_load(p, n, &t);
    if (r == WOLF_READ_OK) wolf_map_tree_free(&t);
    else CHECK(t.entries == NULL);
    return r;
}

static void test_map_tree(void) {
    buf f = tree_file(5);
    wolf_map_tree t;
    CHECK(wolf_map_tree_load(f.p, f.n, &t) == WOLF_READ_OK);
    CHECK(t.version == 0x66 && t.count == 5);
    if (t.count == 5) {
        CHECK(t.entries[0].parent == -1 && t.entries[0].map == 1);
        CHECK(t.entries[3].parent == 1 && t.entries[3].map == 4 && t.entries[4].parent == 2);
    }
    wolf_map_tree_free(&t);
    abuse(&f, load_tree, 5000);
    free(f.p);
    buf e = tree_file(0);
    CHECK(load_tree(e.p, e.n) == WOLF_READ_OK);
    e.p[e.n - 1] = 0xBD;
    CHECK(load_tree(e.p, e.n) == WOLF_READ_E_FORMAT);
    e.p[e.n - 1] = 0xBC;
    e.p[11] = 0xFF; e.p[12] = 0xFF; e.p[13] = 0xFF; e.p[14] = 0x7F; /* a count far beyond the bytes */
    CHECK(load_tree(e.p, e.n) == WOLF_READ_E_FORMAT);
    free(e.p);
}

int main(void) {
    test_each_comparison();
    test_kinds_and_ties();
    test_runs();
    test_failures();
    test_tilesets();
    test_map_tree();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("maprun: all checks passed\n");
    return 0;
}
