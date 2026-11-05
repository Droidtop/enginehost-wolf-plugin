/*
 * The map runtime. Rules and sources: docs/behaviour/maps.md.
 */
#include "wolf_maprun.h"

#include "vm_internal.h"

#include <stdlib.h>

static void drop(wolf_maprun_slot *s) {
    free(s->thread);
    s->thread = NULL;
}

int wolf_maprun_init(wolf_maprun *r, wolf_vm *vm) {
    r->vm = vm;
    r->map = NULL;
    r->nslots = 0;
    r->slots = NULL;
    r->stepping = 0;
    r->probe = calloc(1, sizeof *r->probe);
    return r->probe ? WOLF_MAPRUN_OK : WOLF_MAPRUN_E_NOMEM;
}

void wolf_maprun_free(wolf_maprun *r) {
    for (uint32_t i = 0; i < r->nslots; i++) drop(&r->slots[i]);
    free(r->slots);
    free(r->probe);
    r->slots = NULL;
    r->probe = NULL;
    r->nslots = 0;
    r->map = NULL;
}

int wolf_maprun_enter(wolf_maprun *r, const wolf_map *map) {
    if (r->stepping) return WOLF_MAPRUN_E_BUSY;
    for (uint32_t i = 0; i < r->nslots; i++) drop(&r->slots[i]);
    free(r->slots);
    r->slots = NULL;
    r->nslots = 0;
    r->map = NULL;
    if (!map) return WOLF_MAPRUN_OK;
    if (map->nevents) {
        r->slots = calloc(map->nevents, sizeof *r->slots);
        if (!r->slots) return WOLF_MAPRUN_E_NOMEM;
    }
    r->nslots = map->nevents;
    r->map = map;
    return WOLF_MAPRUN_OK;
}

static int page_holds(wolf_vm *vm, const wolf_thread *ctx, const wolf_map_page *pg) {
    for (int i = 0; i < 4; i++) {
        const wolf_page_condition *c = &pg->cond[i];
        if (!(c->flags & 0x01)) continue;
        int32_t left = wolf_value(vm, ctx, c->variable);
        int32_t right = wolf_value(vm, ctx, c->value);
        if (!wolf_compare(vm, left, right, c->op)) return 0;
    }
    return 1;
}

int32_t wolf_maprun_active_page(wolf_maprun *r, uint32_t event_index) {
    if (!r->map || event_index >= r->map->nevents) return -1;
    const wolf_map_event *e = &r->map->events[event_index];
    r->probe->depth = 0; /* no list: self variables 1,100,000+X mean this event's */
    r->probe->map_event = e->id;
    int32_t chosen = -1;
    for (uint32_t p = 0; p < e->npages; p++)
        if (page_holds(r->vm, r->probe, &e->pages[p])) chosen = (int32_t)p;
    return chosen;
}

static int runs_by_itself(uint8_t trigger) { return trigger == WOLF_PAGE_AUTO || trigger == WOLF_PAGE_PARALLEL; }

/* Runs one frame of an event's run; a run that ends or fails is dropped. */
static void advance(wolf_maprun *r, uint32_t i) {
    wolf_maprun_slot *s = &r->slots[i];
    int res = wolf_vm_run_frame(r->vm, s->thread);
    if (res == WOLF_RUN_YIELD) return;
    if (res != WOLF_RUN_DONE) {
        wolf_vm_log(r->vm, WOLF_LOG_ERROR, "map event %d (page %d) stopped with error %d; it is not run again on this map",
                    (int)r->map->events[i].id, (int)s->page + 1, res);
        s->halted = 1;
    }
    drop(s);
}

uint32_t wolf_maprun_step(wolf_maprun *r) {
    if (!r->map) return 0;
    r->stepping = 1;
    uint32_t left = 0;
    for (uint32_t i = 0; i < r->nslots; i++) {
        wolf_maprun_slot *s = &r->slots[i];
        int32_t page = wolf_maprun_active_page(r, i);
        if (page >= 0 && runs_by_itself(r->map->events[i].pages[page].trigger)) {
            if (!s->thread && !s->halted) {
                s->thread = calloc(1, sizeof *s->thread);
                if (!s->thread) {
                    wolf_vm_log(r->vm, WOLF_LOG_ERROR, "out of memory starting map event %d", (int)r->map->events[i].id);
                } else if (wolf_map_start_page(s->thread, r->map, &r->vm->cfg, i, (uint32_t)page) != 0) {
                    wolf_vm_log(r->vm, WOLF_LOG_ERROR, "map event %d has no self-variable slots; it is not run",
                                (int)r->map->events[i].id);
                    s->halted = 1;
                    drop(s);
                } else {
                    s->page = page;
                    s->trigger = r->map->events[i].pages[page].trigger;
                }
            }
            if (s->thread) advance(r, i);
        }
        if (s->thread) left++;
    }
    r->stepping = 0;
    return left;
}

int wolf_maprun_running(const wolf_maprun *r, uint32_t event_index) {
    return event_index < r->nslots && r->slots[event_index].thread != NULL;
}

int wolf_maprun_blocking(const wolf_maprun *r) {
    for (uint32_t i = 0; i < r->nslots; i++)
        if (r->slots[i].thread && r->slots[i].trigger == WOLF_PAGE_AUTO) return 1;
    return 0;
}
