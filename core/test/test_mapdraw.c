/* Tests for the start position and map drawing (wolf_mapdraw.c), with synthetic maps, sheets and a
 * synthetic system database; no game data is used. */
#include "../src/vm.h"
#include "../src/wolf_data.h"
#include "../src/wolf_map.h"
#include "../src/wolf_mapdraw.h"

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

#define T 4 /* the tile size */
/* the base sheet: 8 chips across, 2 down; chip n is filled with colour 0xFF000000 | n */
static uint32_t base_px[8 * T * 2 * T];
/* an autotile sheet: 2 frames across, 5 rows down; (column c, row r) filled with 0xFF0000 | c << 8 | r */
static uint32_t auto_px[2 * T * 5 * T];
static const wolf_image base_img = {8 * T, 2 * T, base_px}, auto_img = {2 * T, 5 * T, auto_px};

static const wolf_image *image(void *ctx, const char *file) {
    (void)ctx;
    if (!strcmp(file, "MapChip/base.png")) return &base_img;
    if (!strcmp(file, "MapChip/water.png")) return &auto_img;
    return NULL;
}
static const wolf_render_source SRC = {NULL, image, NULL};

static void sheets(void) {
    for (uint32_t y = 0; y < 2 * T; y++)
        for (uint32_t x = 0; x < 8 * T; x++) base_px[y * 8 * T + x] = 0xFF000000u | ((y / T) * 8 + x / T);
    for (uint32_t y = 0; y < 5 * T; y++)
        for (uint32_t x = 0; x < 2 * T; x++) auto_px[y * 2 * T + x] = 0xFFFF0000u | (x / T) << 8 | (y / T);
}

static void on_log(void *ctx, int level, const char *m) { (void)ctx; (void)level; (void)m; }

/* A system database with type 0 (map files) and type 7 (the start: map 1 at 3, 2). */
static int make_sysdb(wolf_db *db) {
    buf p = {0}, d = {0};
    u32(&p, 8);
    for (int t = 0; t < 8; t++) {
        if (t == 0) {
            text(&p, "maps");
            u32(&p, 1); text(&p, "file");
            u32(&p, 2); text(&p, "title"); text(&p, "town");
            text(&p, "");
            u32(&p, 1); u8(&p, 1);
            u32(&p, 1); text(&p, "");
            u32(&p, 1); u32(&p, 0); u32(&p, 1); u32(&p, 0); u32(&p, 1); u32(&p, 0);
        } else if (t == 7) {
            text(&p, "start");
            u32(&p, 3); text(&p, "map"); text(&p, "x"); text(&p, "y");
            u32(&p, 1); text(&p, "first");
            text(&p, "");
            u32(&p, 3); u8(&p, 0); u8(&p, 0); u8(&p, 0);
            u32(&p, 3); text(&p, ""); text(&p, ""); text(&p, "");
            u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
            u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
            u32(&p, 3); u32(&p, 0); u32(&p, 0); u32(&p, 0);
        } else {
            text(&p, "");
            u32(&p, 0); u32(&p, 0); text(&p, "");
            u32(&p, 0); u32(&p, 0); u32(&p, 0); u32(&p, 0); u32(&p, 0);
        }
    }
    static const uint8_t head[] = {0x00, 0x57, 0x00, 0x00, 0x4F, 0x4C, 0x00, 0x46, 0x4D, 0x00};
    put(&d, head, sizeof head);
    u8(&d, 0xC1);
    u32(&d, 8);
    for (int t = 0; t < 8; t++) {
        u32(&d, 0xFFFFFFFEu); u32(&d, 0);
        if (t == 0) {
            u32(&d, 1); u32(&d, 2000);
            u32(&d, 2); text(&d, "MapData/Title.mps"); text(&d, "MapData/Town.mps");
        } else if (t == 7) {
            u32(&d, 3); u32(&d, 1000); u32(&d, 1001); u32(&d, 1002);
            u32(&d, 1); u32(&d, 1); u32(&d, 3); u32(&d, 2);
        } else {
            u32(&d, 0); u32(&d, 0);
        }
    }
    u8(&d, 0xC1);
    int r = wolf_db_load(p.p, p.n, d.p, d.n, db);
    free(p.p);
    free(d.p);
    return r;
}

static void test_start(void) {
    wolf_db sys;
    CHECK(make_sysdb(&sys) == WOLF_READ_OK);
    wolf_host host = {NULL, on_log};
    wolf_config cfg = {10, 0, 10, 10, 10, 0, 1, 0};
    wolf_vm *vm = wolf_vm_create(&host, &cfg);
    int32_t map = -1, x = -1, y = -1;
    CHECK(wolf_start_position(vm, &map, &x, &y) == -1); /* no system database yet */
    wolf_vm_set_databases(vm, NULL, NULL, &sys);
    CHECK(wolf_start_position(vm, &map, &x, &y) == 0 && map == 1 && x == 3 && y == 2);
    CHECK(!strcmp(wolf_map_file(vm, map), "MapData/Town.mps") && !strcmp(wolf_map_file(vm, 0), "MapData/Title.mps"));
    CHECK(wolf_map_file(vm, 2) == NULL && wolf_map_file(vm, -1) == NULL);
    wolf_vm_destroy(vm);
    wolf_db_free(&sys);
}

static wolf_map make_map(uint32_t w, uint32_t h, int32_t *tiles) {
    wolf_map m;
    memset(&m, 0, sizeof m);
    m.tileset = 0;
    m.width = w;
    m.height = h;
    m.layers = 1;
    m.tiles = tiles;
    return m;
}

static char *autos[] = {"MapChip/water.png"};
static wolf_tileset set0 = {"field", "MapChip/base.png", 1, autos, 0, NULL, NULL};
static wolf_tilesets sets = {0, 0, 1, &set0};

static void test_camera(void) {
    int32_t tiles[100] = {0};
    wolf_map m = make_map(10, 10, tiles);
    double cx, cy;
    wolf_map_camera(&m, T, 16, 16, 5, 5, &cx, &cy);
    CHECK(cx == 14 && cy == 14);
    wolf_map_camera(&m, T, 16, 16, 0, 9, &cx, &cy); /* kept inside the map */
    CHECK(cx == 0 && cy == 24);
    wolf_map_camera(&m, T, 16, 16, -1, 0, &cx, &cy); /* off the map to the left: clamped */
    CHECK(cx == 0);
    wolf_map small = make_map(2, 2, tiles);
    wolf_map_camera(&small, T, 16, 16, 0, 0, &cx, &cy); /* smaller than the screen: centred */
    CHECK(cx == -4 && cy == -4);
}

static void test_draw(void) {
    /* a 3x2 map: base chips 9 and 1, an autotile with corners 4 0 3 1, nothing (-1) */
    int32_t tiles[6] = {9, 1, 104031, -1, 0, 0};
    wolf_map m = make_map(3, 2, tiles);
    static uint32_t px[12 * 8];
    for (int i = 0; i < 12 * 8; i++) px[i] = 0xFF000000u;
    wolf_render_map(&m, &sets, T, &SRC, 0, 0, px, 12, 8, 0);
    CHECK(px[0] == (0xFF000000u | 9) && px[4] == (0xFF000000u | 1));
    /* the autotile at (2,0): quarters from rows 4, 0, 3, 1 of column 0 */
    CHECK(px[0 * 12 + 8] == 0xFFFF0004u && px[0 * 12 + 10] == 0xFFFF0000u);
    CHECK(px[2 * 12 + 8] == 0xFFFF0003u && px[3 * 12 + 11] == 0xFFFF0001u);
    /* animation: every 20 frames the next column */
    wolf_render_map(&m, &sets, T, &SRC, 0, 0, px, 12, 8, 20);
    CHECK(px[0 * 12 + 8] == 0xFFFF0104u);
    wolf_render_map(&m, &sets, T, &SRC, 0, 0, px, 12, 8, 40);
    CHECK(px[0 * 12 + 8] == 0xFFFF0004u);
    /* the camera moves the map: one tile left */
    for (int i = 0; i < 12 * 8; i++) px[i] = 0xFF000000u;
    wolf_render_map(&m, &sets, T, &SRC, T, 0, px, 12, 8, 0);
    CHECK(px[0] == (0xFF000000u | 1) && px[8] == 0xFF000000u);
    /* a missing sheet, a bad tileset or no tiles draws nothing */
    wolf_tileset none = {"none", "MapChip/missing.png", 0, NULL, 0, NULL, NULL};
    wolf_tilesets nosets = {0, 0, 1, &none};
    for (int i = 0; i < 12 * 8; i++) px[i] = 0xFF000000u;
    wolf_render_map(&m, &nosets, T, &SRC, 0, 0, px, 12, 8, 0);
    m.tileset = 5;
    wolf_render_map(&m, &sets, T, &SRC, 0, 0, px, 12, 8, 0);
    m.tileset = 0;
    m.tiles = NULL;
    wolf_render_map(&m, &sets, T, &SRC, 0, 0, px, 12, 8, 0);
    CHECK(px[0] == 0xFF000000u && px[8] == 0xFF000000u);
}

int main(void) {
    sheets();
    test_start();
    test_camera();
    test_draw();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("mapdraw: all checks passed");
    return 0;
}
