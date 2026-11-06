/*
 * Which map a game starts on, and drawing a map's tiles as quads for either renderer.
 * docs/behaviour/maps.md, "Starting a game" and "Drawing the map".
 */
#ifndef ENGINEHOST_WOLF_MAPDRAW_H
#define ENGINEHOST_WOLF_MAPDRAW_H

#include <stdint.h>

#include "vm.h"
#include "wolf_data.h"
#include "wolf_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The start position: the system database's type 7, row 0 ("初期位置": map, X, Y). 0, or -1 when
 * the game has none. */
int wolf_start_position(const wolf_vm *vm, int32_t *map, int32_t *x, int32_t *y);
/* A map's file, relative to Data/ (the system database's type 0, the map's row, its first
 * field), or NULL. */
const char *wolf_map_file(const wolf_vm *vm, int32_t map);

/* Where the screen's top left sits on the map, in pixels, centred on tile (x, y) and kept inside
 * the map [M]; a map smaller than the screen is centred. */
void wolf_map_camera(const wolf_map *m, uint32_t tile, uint32_t screen_w, uint32_t screen_h, int32_t x, int32_t y,
                     double *cam_x, double *cam_y);

/* The map's tiles as quads, layer by layer, for the screen at the camera. tile is the game's tile
 * size (Game.dat setting 0); frame drives the autotiles' animation. Tiles whose sheet the source
 * has no image for are skipped. */
void wolf_map_quads(const wolf_map *m, const wolf_tilesets *sets, uint32_t tile, const wolf_render_source *src,
                    double cam_x, double cam_y, uint32_t screen_w, uint32_t screen_h, uint64_t frame,
                    wolf_quad_fn fn, void *ctx);
/* The same, in software onto frame. */
void wolf_render_map(const wolf_map *m, const wolf_tilesets *sets, uint32_t tile, const wolf_render_source *src,
                     double cam_x, double cam_y, uint32_t *pixels, uint32_t screen_w, uint32_t screen_h,
                     uint64_t frame);

#ifdef __cplusplus
}
#endif

#endif
