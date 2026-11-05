/*
 * Wolf RPG Editor maps (.mps): the header, the tile layers and the map events with their pages.
 * Layout: docs/behaviour/formats.md, "Maps (.mps)". Every count, size and length is checked
 * against the input and a cap; the map owns copies of everything it keeps. Pro-protected maps
 * are refused.
 */
#ifndef ENGINEHOST_WOLF_MAP_H
#define ENGINEHOST_WOLF_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "vm.h"
#include "wolf_cmd.h"
#include "wolf_read.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caps on what a map may ask for. */
#define WOLF_MAP_SIDE_MAX 100000u   /* width or height, in tiles */
#define WOLF_MAP_LAYERS_MAX 64u
#define WOLF_MAP_EVENTS_MAX 100000u
#define WOLF_MAP_PAGES_MAX 10000u
#define WOLF_MAP_ROUTE_MAX 100000u  /* steps in one page's move route */

/* A layer value from here on is an autotile: value / base is its slot plus one, and
 * value % base the four corners' shapes as decimal digits. Below it, a chip of the base
 * sheet, 8 to a row. */
#define WOLF_MAP_AUTOTILE_BASE 100000

enum {
    WOLF_PAGE_CONFIRM = 0,      /* run when the hero checks the event */
    WOLF_PAGE_AUTO = 1,         /* run on its own, alone */
    WOLF_PAGE_PARALLEL = 2,     /* run on its own, beside everything else */
    WOLF_PAGE_HERO_TOUCH = 3,
    WOLF_PAGE_EVENT_TOUCH = 4,
};

typedef struct {
    uint8_t flags;    /* bit 0: the condition is used; other low bits Unknown */
    uint8_t op;       /* the comparison, the stored byte's high 4 bits */
    int32_t variable; /* a variable reference */
    int32_t value;
} wolf_page_condition;

typedef struct {
    char *graphic;
    uint8_t direction, frame, opacity, blend;
    uint8_t trigger;                  /* WOLF_PAGE_* (other values kept as stored) */
    wolf_page_condition cond[4];
    uint8_t anim_speed, move_speed, move_freq;
    uint8_t move_type;                /* 0 none, 1 custom route, 2 random, 3 toward the hero */
    uint8_t options;                  /* formats.md lists the bits */
    uint8_t route_options;
    uint32_t route_steps;
    uint32_t route_len;
    uint8_t *route;                   /* the steps as stored (route_len bytes), owned */
    wolf_list list;
    int32_t features;
    uint8_t shadow, range_x, range_y;
    int32_t transfer;                 /* the stored byte, or -1 when the page has none */
} wolf_map_page;

typedef struct {
    int32_t id;
    char *name;
    int32_t x, y;
    uint32_t npages;
    wolf_map_page *pages;
} wolf_map_event;

typedef struct {
    int utf8;
    uint32_t version;
    int packed;
    int32_t tileset;
    uint32_t width, height, layers;
    int32_t *tiles;   /* layers x height x width, or NULL when the map stores none */
    uint32_t nevents;
    wolf_map_event *events;
} wolf_map;

int wolf_map_load(const uint8_t *p, size_t n, wolf_map *out);
void wolf_map_free(wolf_map *m);

/* A layer's value at (x, y), or 0 outside the map or when it stores no tiles. */
int32_t wolf_map_tile(const wolf_map *m, uint32_t layer, int32_t x, int32_t y);

/* The event with this id, or NULL. */
const wolf_map_event *wolf_map_find(const wolf_map *m, int32_t id);

/* Widens cfg->map_events so every event's self variables have a slot (never narrows it). */
void wolf_map_config(const wolf_map *m, wolf_config *cfg);

/* Starts t on one page of one event (by their places in the map), with the event's self
 * variables. 0, or -1 when there is no such event or page, or its id has no self slot in a VM
 * made with cfg (call wolf_map_config first). Which page an event shows is decided by the
 * pages' conditions; that choice is not made here. */
int wolf_map_start_page(wolf_thread *t, const wolf_map *m, const wolf_config *cfg, uint32_t event_index,
                        uint32_t page_index);

#ifdef __cplusplus
}
#endif

#endif
