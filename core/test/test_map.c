/* Tests for the map loader. Every map is built here from docs/behaviour/formats.md,
 * "Maps (.mps)"; no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t *p; size_t n, cap; } buf;
static void put(buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    if (n) memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void u8(buf *b, uint32_t v) { uint8_t x = (uint8_t)v; put(b, &x, 1); }
static void u32(buf *b, uint32_t v) { uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; put(b, x, 4); }
static void text(buf *b, const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; u32(b, n); put(b, s, n); }

static void lz4_literals(buf *b, const uint8_t *src, size_t n) {
    if (n < 15) {
        u8(b, (uint32_t)n << 4);
    } else {
        u8(b, 0xF0);
        size_t rest = n - 15;
        while (rest >= 255) { u8(b, 255); rest -= 255; }
        u8(b, (uint32_t)rest);
    }
    put(b, src, n);
}

static uint32_t rng = 4242;
static uint32_t next_rand(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

/* One command: ints, no strings, no route; extras adds the 3.5+ trailing byte list. */
static void cmd(buf *b, int32_t code, const int32_t *ints, int nint, int extras) {
    u8(b, (uint32_t)nint + 1);
    u32(b, (uint32_t)code);
    for (int i = 0; i < nint; i++) u32(b, (uint32_t)ints[i]);
    u8(b, 0);
    u8(b, 0);
    u8(b, 0);
    if (extras) { u8(b, 1); u8(b, 0xEE); }
}

/* A page whose list sets this event's self variable 3 to value. */
static void page(buf *b, int extras, uint8_t trigger, int32_t value, int32_t features) {
    u8(b, 0x79);
    u32(b, 0);
    text(b, "hero.png");
    u8(b, 4); u8(b, 1); u8(b, 255); u8(b, 0);
    u8(b, trigger);
    u8(b, 0x21); u8(b, 0x20); u8(b, 0x20); u8(b, 0x30);
    u32(b, 2000005); u32(b, 1000000); u32(b, 1000000); u32(b, 1000000);
    u32(b, 3); u32(b, 0); u32(b, 0); u32(b, (uint32_t)-2);
    u8(b, 3); u8(b, 4); u8(b, 2); u8(b, 1); u8(b, 0x05); u8(b, 0x02);
    u32(b, 2); /* route: step 7 with one argument, step 9 with none */
    u8(b, 7); u8(b, 1); u32(b, 12); u8(b, 1); u8(b, 0);
    u8(b, 9); u8(b, 0); u8(b, 1); u8(b, 0);
    u32(b, 2);
    int32_t set[4] = {1100003, value, 0, 0};
    cmd(b, 121, set, 4, extras);
    cmd(b, 0, NULL, 0, extras);
    u32(b, (uint32_t)features);
    u8(b, 1); u8(b, 2); u8(b, 3);
    if (features > 3) u8(b, 9);
    u8(b, 0x7A);
}

static void event(buf *b, int extras, int32_t id, const char *name, int pages) {
    u8(b, 0x6F);
    u8(b, 0x39); u8(b, 0x30); u8(b, 0); u8(b, 0);
    u32(b, (uint32_t)id);
    text(b, name);
    u32(b, (uint32_t)(id + 1)); u32(b, 2);
    u32(b, (uint32_t)pages);
    u32(b, 0);
    for (int i = 0; i < pages; i++) page(b, extras, (uint8_t)(i ? 2 : 1), 10 * id + i + 1, i ? 4 : 0);
    u8(b, 0x70);
}

/* The body after the version byte and the unknown byte. notiles: -1 in place of the tiles. */
static buf body(uint32_t version, int notiles) {
    buf b = {0};
    int layered = version >= 0x67;
    text(&b, "");
    u32(&b, 7);  /* tileset */
    u32(&b, 3);  /* width */
    u32(&b, 2);  /* height */
    u32(&b, 2);  /* events */
    uint32_t layers = 3;
    if (layered) { u32(&b, 0); layers = 2; u32(&b, layers); }
    if (notiles) {
        u32(&b, (uint32_t)-1);
    } else {
        for (uint32_t l = 0; l < layers; l++)
            for (uint32_t i = 0; i < 6; i++) u32(&b, l == 1 && i == 4 ? 300123 : 100 * l + i);
    }
    event(&b, layered, 0, "Door", 1);
    event(&b, layered, 5, "Guard", 2);
    u8(&b, 0x66);
    return b;
}

static buf map_file(uint32_t version, int utf8, int packed, const buf *bd) {
    buf f = {0};
    uint8_t magic[20] = {0};
    memcpy(magic + 10, "WOLFM", 6);
    if (utf8) magic[16] = 0x55;
    put(&f, magic, sizeof magic);
    u32(&f, version);
    u8(&f, 0);
    if (packed) {
        buf blk = {0};
        lz4_literals(&blk, bd->p, bd->n);
        u32(&f, (uint32_t)bd->n);
        u32(&f, (uint32_t)blk.n);
        put(&f, blk.p, blk.n);
        free(blk.p);
    } else {
        put(&f, bd->p, bd->n);
    }
    return f;
}

static int load(const uint8_t *p, size_t n) {
    wolf_map m;
    int r = wolf_map_load(p, n, &m);
    if (r == WOLF_READ_OK) wolf_map_free(&m);
    else CHECK(m.events == NULL && m.tiles == NULL);
    return r;
}

static void check_map(const wolf_map *m, uint32_t version, int notiles) {
    int layered = version >= 0x67;
    CHECK(m->version == version && m->tileset == 7 && m->width == 3 && m->height == 2);
    CHECK(m->layers == (layered ? 2u : 3u));
    if (notiles) {
        CHECK(m->tiles == NULL && wolf_map_tile(m, 0, 0, 0) == 0);
    } else {
        CHECK(m->tiles != NULL);
        CHECK(wolf_map_tile(m, 0, 2, 1) == 5);
        CHECK(wolf_map_tile(m, 1, 1, 1) == 300123); /* an autotile: slot 2, corners 0123 */
        CHECK(wolf_map_tile(m, 1, 0, 0) == 100);
        CHECK(wolf_map_tile(m, 1, 3, 0) == 0 && wolf_map_tile(m, 0, -1, 0) == 0);
        CHECK(wolf_map_tile(m, m->layers, 0, 0) == 0);
    }
    CHECK(m->nevents == 2);
    if (m->nevents != 2) return;
    const wolf_map_event *e = &m->events[1];
    CHECK(e->id == 5 && strcmp(e->name, "Guard") == 0 && e->x == 6 && e->y == 2 && e->npages == 2);
    CHECK(wolf_map_find(m, 5) == e && wolf_map_find(m, 1) == NULL);
    CHECK(m->events[0].npages == 1 && m->events[0].pages[0].trigger == WOLF_PAGE_AUTO);
    if (e->npages != 2) return;
    const wolf_map_page *pg = &e->pages[1];
    CHECK(strcmp(pg->graphic, "hero.png") == 0 && pg->direction == 4 && pg->frame == 1);
    CHECK(pg->opacity == 255 && pg->blend == 0 && pg->trigger == WOLF_PAGE_PARALLEL);
    CHECK(pg->cond[0].flags == 1 && pg->cond[0].op == 2 && pg->cond[0].variable == 2000005 && pg->cond[0].value == 3);
    CHECK(pg->cond[1].flags == 0 && pg->cond[3].op == 3 && pg->cond[3].value == -2);
    CHECK(pg->anim_speed == 3 && pg->move_speed == 4 && pg->move_freq == 2 && pg->move_type == 1);
    CHECK(pg->options == 0x05 && pg->route_options == 0x02);
    CHECK(pg->route_steps == 2 && pg->route_len == 12 && pg->route[0] == 7 && pg->route[8] == 9);
    CHECK(pg->list.count == 2 && pg->list.cmds[0].code == 121 && pg->list.cmds[0].ints[1] == 52);
    CHECK(pg->features == 4 && pg->shadow == 1 && pg->range_x == 2 && pg->range_y == 3 && pg->transfer == 9);
    CHECK(e->pages[0].features == 0 && e->pages[0].transfer == -1);
}

/* A loaded page runs on a thread with its event's self variables. */
static void check_run(const wolf_map *m) {
    wolf_config cfg = {16, 16, 16, 16, 16, 0, 1};
    wolf_thread *t = calloc(1, sizeof *t);
    CHECK(wolf_map_start_page(t, m, &cfg, 1, 0) == -1); /* no self slots yet */
    wolf_map_config(m, &cfg);
    CHECK(cfg.map_events == 6);
    wolf_host host = {0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    CHECK(vm != NULL);
    if (!vm) { free(t); return; }
    CHECK(wolf_map_start_page(t, m, &cfg, 1, 1) == 0);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_map_start_page(t, m, &cfg, 0, 0) == 0);
    CHECK(wolf_vm_run_frame(vm, t) == WOLF_RUN_DONE);
    CHECK(wolf_get(vm, t, 1100003) == 1); /* event 0's own slot */
    t->map_event = 5;
    CHECK(wolf_get(vm, t, 1100003) == 52);
    t->map_event = 4;
    CHECK(wolf_get(vm, t, 1100003) == 0);
    CHECK(wolf_map_start_page(t, m, &cfg, 2, 0) == -1);
    CHECK(wolf_map_start_page(t, m, &cfg, 1, 2) == -1);
    wolf_config small = cfg;
    small.map_events = 5;
    CHECK(wolf_map_start_page(t, m, &small, 1, 0) == -1); /* id 5 has no slot */
    free(t);
    wolf_vm_destroy(vm);
}

static void abuse(const buf *f, int fuzz) {
    for (size_t k = 0; k < f->n; k++) CHECK(load(f->p, k) != WOLF_READ_OK);
    uint8_t *m = malloc(f->n + 1);
    memcpy(m, f->p, f->n);
    m[f->n] = 0x66;
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

static void test_version(uint32_t version, int utf8, int packed, int notiles) {
    buf bd = body(version, notiles);
    buf f = map_file(version, utf8, packed, &bd);
    wolf_map m;
    CHECK(wolf_map_load(f.p, f.n, &m) == WOLF_READ_OK);
    CHECK(m.utf8 == utf8 && m.packed == packed);
    check_map(&m, version, notiles);
    check_run(&m);
    wolf_map_free(&m);
    abuse(&f, 20000);
    if (packed) {
        for (size_t k = 0; k < bd.n; k++) { /* a short body, packed correctly */
            buf t = map_file(version, utf8, 1, &(buf){bd.p, k, k});
            if (t.n) t.p[t.n - 1] = 0x66; /* past the raw last-byte check */
            CHECK(load(t.p, t.n) != WOLF_READ_OK);
            free(t.p);
        }
        uint8_t *mb = malloc(bd.n);
        for (int i = 0; i < 5000; i++) {
            memcpy(mb, bd.p, bd.n);
            mb[next_rand() % bd.n] = (uint8_t)next_rand();
            buf t = map_file(version, utf8, 1, &(buf){mb, bd.n, bd.n});
            load(t.p, t.n);
            free(t.p);
        }
        free(mb);
    }
    free(f.p);
    free(bd.p);
}

static void test_refusals(void) {
    buf bd = body(0x66, 0);
    buf f = map_file(0x66, 0, 0, &bd);
    uint8_t *m = malloc(f.n);
    /* each byte of the magic */
    for (size_t i = 0; i < 20; i++) {
        memcpy(m, f.p, f.n);
        m[i] ^= 0x01;
        CHECK(load(m, f.n) != WOLF_READ_OK);
    }
    /* a last byte that is not the terminator */
    memcpy(m, f.p, f.n);
    m[f.n - 1] = 0x65;
    CHECK(load(m, f.n) == WOLF_READ_E_FORMAT);
    free(m);
    free(f.p);
    free(bd.p);

    /* a size beyond the caps is refused before allocating */
    buf b = {0};
    uint8_t magic[20] = {0};
    memcpy(magic + 10, "WOLFM", 6);
    put(&b, magic, 20);
    u32(&b, 0x66); u8(&b, 0);
    text(&b, ""); u32(&b, 0); u32(&b, 200000); u32(&b, 200000); u32(&b, 0);
    u8(&b, 0x66);
    CHECK(load(b.p, b.n) == WOLF_READ_E_TOO_BIG);
    /* negative sizes */
    b.n = 20 + 5 + 5 + 4;
    u32(&b, (uint32_t)-3); u32(&b, 1); u32(&b, 0); u8(&b, 0x66);
    CHECK(load(b.p, b.n) == WOLF_READ_E_FORMAT);
    /* tiles that do not fit the file */
    b.n = 20 + 5 + 5 + 4;
    u32(&b, 50); u32(&b, 50); u32(&b, 0); u8(&b, 0x66);
    CHECK(load(b.p, b.n) == WOLF_READ_E_FORMAT);
    /* an event count beyond what the bytes can hold */
    b.n = 20 + 5 + 5 + 4;
    u32(&b, 0); u32(&b, 0); u32(&b, 1000); u8(&b, 0x66);
    CHECK(load(b.p, b.n) == WOLF_READ_E_FORMAT);
    /* an empty map: no tiles, no events */
    b.n = 20 + 5 + 5 + 4;
    u32(&b, 0); u32(&b, 0); u32(&b, 0); u8(&b, 0x66);
    wolf_map mp;
    CHECK(wolf_map_load(b.p, b.n, &mp) == WOLF_READ_OK && mp.nevents == 0 && mp.width == 0);
    wolf_map_free(&mp);
    /* a packed envelope asking past the unpacked cap */
    b.n = 20;
    u32(&b, 0x67); u8(&b, 0);
    u32(&b, 0xFFFFFFFFu); u32(&b, 1); u8(&b, 0x66);
    CHECK(load(b.p, b.n) == WOLF_READ_E_TOO_BIG);
    free(b.p);
}

int main(void) {
    test_version(0x65, 0, 0, 0); /* 2.x, plain */
    test_version(0x66, 1, 0, 0); /* 3.x UTF-8, plain */
    test_version(0x66, 1, 0, 1); /* 3.x UTF-8 with no tiles */
    test_version(0x66, 1, 1, 0); /* 3.x packed */
    test_version(0x67, 1, 1, 0); /* 3.5: layers, command tails, packed */
    test_version(0x67, 1, 0, 1); /* 3.5 plain, no tiles */
    test_refusals();
    if (failures) { fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    fprintf(stderr, "all map checks passed\n");
    return 0;
}
