/* See wolf_commonrun.h; the rules and their sources are in docs/behaviour/maps.md. */
#include "wolf_commonrun.h"

#include "vm_internal.h"

#include <stdlib.h>
#include <string.h>

enum { RUN_CALLED = 0, RUN_AUTOMATIC = 1, RUN_PARALLEL = 2, RUN_ALWAYS = 3 };

int wolf_commonrun_init(wolf_commonrun *r, wolf_vm *vm, const wolf_commons *commons) {
    memset(r, 0, sizeof *r);
    r->vm = vm;
    r->commons = commons;
    uint32_t n = 0;
    for (uint32_t i = 0; i < commons->count; i++)
        if (commons->events[i].run >= RUN_AUTOMATIC && commons->events[i].run <= RUN_ALWAYS) n++;
    if (!n) return WOLF_COMMONRUN_OK;
    r->slots = calloc(n, sizeof *r->slots);
    if (!r->slots) return WOLF_COMMONRUN_E_NOMEM;
    for (uint32_t i = 0; i < commons->count; i++) {
        uint8_t run = commons->events[i].run;
        if (run < RUN_AUTOMATIC || run > RUN_ALWAYS) continue;
        r->slots[r->nslots].event = i;
        r->slots[r->nslots].run = run;
        r->nslots++;
    }
    return WOLF_COMMONRUN_OK;
}

static void stop(wolf_commonrun_slot *s) {
    free(s->thread);
    s->thread = NULL;
}

void wolf_commonrun_free(wolf_commonrun *r) {
    for (uint32_t i = 0; i < r->nslots; i++) stop(&r->slots[i]);
    free(r->slots);
    memset(r, 0, sizeof *r);
}

/* The event's condition: its variable compared with its value, by the comparison numbering 111
 * uses [M]. Always-parallel events have none. */
static int holds(wolf_commonrun *r, const wolf_commonrun_slot *s) {
    if (s->run == RUN_ALWAYS) return 1;
    const wolf_common_event *e = &r->commons->events[s->event];
    return wolf_compare(r->vm, wolf_get(r->vm, NULL, e->condition_var), e->condition_value, e->condition_op);
}

int wolf_commonrun_blocking(const wolf_commonrun *r) {
    for (uint32_t i = 0; i < r->nslots; i++)
        if (r->slots[i].run == RUN_AUTOMATIC && r->slots[i].thread) return 1;
    return 0;
}

int wolf_commonrun_running(const wolf_commonrun *r, uint32_t event) {
    for (uint32_t i = 0; i < r->nslots; i++)
        if (r->slots[i].event == event) return r->slots[i].thread != NULL;
    return 0;
}

void wolf_commonrun_map_changed(wolf_commonrun *r) {
    for (uint32_t i = 0; i < r->nslots; i++) {
        stop(&r->slots[i]);
        r->slots[i].halted = 0;
    }
}

uint32_t wolf_commonrun_step(wolf_commonrun *r) {
    uint32_t running = 0;
    for (uint32_t i = 0; i < r->nslots; i++) {
        wolf_commonrun_slot *s = &r->slots[i];
        int ok = holds(r, s);
        if (s->thread && !ok && s->run != RUN_AUTOMATIC) stop(s); /* a parallel run is cut off (manual) */
        if (!s->thread) {
            /* nothing starts while another automatic event runs (manual) [M: parallel ones still do] */
            if (!ok || s->halted || (s->run == RUN_AUTOMATIC && wolf_commonrun_blocking(r))) continue;
            s->thread = calloc(1, sizeof *s->thread);
            if (!s->thread) continue;
            wolf_thread_start(s->thread, r->commons->table[s->event].list, -1, (int32_t)s->event);
        }
        int res = wolf_vm_run_frame(r->vm, s->thread);
        if (res == WOLF_RUN_YIELD) { running++; continue; }
        if (res != WOLF_RUN_DONE) {
            wolf_vm_log(r->vm, WOLF_LOG_ERROR, "common event %u stopped (%d); it is not started again on this map",
                        s->event, res);
            s->halted = 1;
        }
        stop(s); /* ended: it starts again next frame while its condition holds */
    }
    return running;
}
