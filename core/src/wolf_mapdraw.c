/* See wolf_mapdraw.h. Tile pieces follow formats.md, "Drawing tiles" (wolf_tile_pieces). */
#include "wolf_mapdraw.h"

#include "vm_internal.h"

#include <math.h>
#include <string.h>

#define START_TYPE 7 /* system database: the start position (manual 03mapstartpos.html) */
#define MAPS_TYPE 0  /* system database: the map list, the .mps file first */

int wolf_start_position(const wolf_vm *vm, int32_t *map, int32_t *x, int32_t *y) {
    const struct wolf_db *db = vm->db[WOLF_DB_SYSTEM];
    if (!db || !wolf_db_number(db, START_TYPE, 0, 0, map)) return -1;
    if (!wolf_db_number(db, START_TYPE, 0, 1, x)) *x = 0;
    if (!wolf_db_number(db, START_TYPE, 0, 2, y)) *y = 0;
    return 0;
}

const char *wolf_map_file(const wolf_vm *vm, int32_t map) {
    const struct wolf_db *db = vm->db[WOLF_DB_SYSTEM];
    if (!db || map < 0) return NULL;
    const char *f = wolf_db_text(db, MAPS_TYPE, (uint32_t)map, 0);
    return f && *f ? f : NULL;
}

static double fit(double want, double map_px, double screen) {
    if (map_px <= screen) return -(screen - map_px) / 2; /* smaller than the screen: centred */
    if (want < 0) return 0;
    if (want > map_px - screen) return map_px - screen;
    return want;
}

void wolf_map_camera(const wolf_map *m, uint32_t tile, uint32_t sw, uint32_t sh, int32_t x, int32_t y,
                     double *cam_x, double *cam_y) {
    double t = tile;
    *cam_x = fit(x * t + t / 2 - sw / 2.0, (double)m->width * t, sw);
    *cam_y = fit(y * t + t / 2 - sh / 2.0, (double)m->height * t, sh);
}

void wolf_map_quads(const wolf_map *m, const wolf_tilesets *sets, uint32_t tile, const wolf_render_source *src,
                    double cam_x, double cam_y, uint32_t sw, uint32_t sh, uint64_t frame, wolf_quad_fn fn,
                    void *ctx) {
    if (!m || !m->tiles || !sets || !src || !src->image || !fn || !tile) return;
    if (m->tileset < 0 || (uint32_t)m->tileset >= sets->count) return;
    const wolf_tileset *ts = &sets->sets[m->tileset];
    const wolf_image *base = ts->base && *ts->base ? src->image(src->ctx, ts->base) : NULL;
    /* only the tiles on screen */
    int64_t x0 = (int64_t)(cam_x / tile) - 1, y0 = (int64_t)(cam_y / tile) - 1;
    int64_t x1 = (int64_t)((cam_x + sw) / tile) + 1, y1 = (int64_t)((cam_y + sh) / tile) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= (int64_t)m->width) x1 = (int64_t)m->width - 1;
    if (y1 >= (int64_t)m->height) y1 = (int64_t)m->height - 1;
    for (uint32_t layer = 0; layer < m->layers; layer++) {
        for (int64_t y = y0; y <= y1; y++) {
            for (int64_t x = x0; x <= x1; x++) {
                int32_t v = wolf_map_tile(m, layer, (int32_t)x, (int32_t)y);
                wolf_tile_piece pc[4];
                const wolf_image *im = base;
                const char *file = ts->base;
                uint32_t column = 0;
                if (v >= 100000) {
                    uint32_t slot = (uint32_t)(v / 100000 - 1);
                    if (slot >= ts->nautotiles || !ts->autotiles[slot] || !*ts->autotiles[slot]) continue;
                    file = ts->autotiles[slot];
                    im = src->image(src->ctx, file);
                    if (!im) continue;
                    uint32_t columns = im->width / tile ? im->width / tile : 1;
                    column = (uint32_t)(frame / wolf_autotile_step(file) % columns);
                }
                if (!im) continue;
                int n = wolf_tile_pieces(v, tile, column, pc);
                for (int k = 0; k < n; k++) {
                    wolf_quad q;
                    memset(&q, 0, sizeof q);
                    q.file = file;
                    q.image = im;
                    double X = (double)x * tile + pc[k].dx - cam_x, Y = (double)y * tile + pc[k].dy - cam_y;
                    q.x[0] = X; q.y[0] = Y;
                    q.x[1] = X + pc[k].w; q.y[1] = Y;
                    q.x[2] = X + pc[k].w; q.y[2] = Y + pc[k].h;
                    q.x[3] = X; q.y[3] = Y + pc[k].h;
                    q.u0 = pc[k].sx; q.v0 = pc[k].sy;
                    q.u1 = pc[k].sx + pc[k].w; q.v1 = pc[k].sy + pc[k].h;
                    if (q.u1 > im->width || q.v1 > im->height) continue; /* off the sheet */
                    q.opacity = 1;
                    q.r = q.g = q.b = 1;
                    q.blend = WOLF_BLEND_NORMAL;
                    q.number = -300000; /* the map's band, for the log */
                    fn(ctx, &q);
                }
            }
        }
    }
}

typedef struct {
    uint32_t *frame;
    uint32_t w, h;
} canvas;

/* A tile quad is an axis-aligned copy: straight through. */
static void copy_quad(void *ctx, const wolf_quad *q) {
    const canvas *cv = ctx;
    const wolf_image *im = q->image;
    int64_t x0 = (int64_t)floor(q->x[0]), y0 = (int64_t)floor(q->y[0]);
    int64_t w = (int64_t)(q->u1 - q->u0), h = (int64_t)(q->v1 - q->v0);
    for (int64_t dy = 0; dy < h; dy++) {
        int64_t Y = y0 + dy;
        if (Y < 0 || Y >= cv->h) continue;
        for (int64_t dx = 0; dx < w; dx++) {
            int64_t X = x0 + dx;
            if (X < 0 || X >= cv->w) continue;
            uint32_t s = im->argb[(size_t)(q->v0 + dy) * im->width + (size_t)(q->u0 + dx)];
            uint32_t a = s >> 24;
            if (!a) continue;
            uint32_t *d = &cv->frame[(size_t)Y * cv->w + (size_t)X];
            if (a == 255) { *d = s | 0xFF000000u; continue; }
            uint32_t out = 0xFF000000u;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t sc = (s >> sh) & 255, dc = (*d >> sh) & 255;
                out |= ((dc * (255 - a) + sc * a + 127) / 255) << sh;
            }
            *d = out;
        }
    }
}

void wolf_render_map(const wolf_map *m, const wolf_tilesets *sets, uint32_t tile, const wolf_render_source *src,
                     double cam_x, double cam_y, uint32_t *pixels, uint32_t sw, uint32_t sh, uint64_t frame) {
    if (!pixels) return;
    canvas cv = {pixels, sw, sh};
    wolf_map_quads(m, sets, tile, src, cam_x, cam_y, sw, sh, frame, copy_quad, &cv);
}
