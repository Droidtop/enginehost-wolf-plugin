/*
 * Common events that start by themselves: automatic, parallel and always-parallel (the editor's
 * manual, help/02commonev.html). One run slot per such event, stepped once a frame before the
 * map's events. docs/behaviour/maps.md, "Common events that start by themselves".
 */
#ifndef ENGINEHOST_WOLF_COMMONRUN_H
#define ENGINEHOST_WOLF_COMMONRUN_H

#include "vm.h"
#include "wolf_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t event;      /* its place in CommonEvent.dat */
    uint8_t run;         /* 1 automatic, 2 parallel, 3 always parallel */
    wolf_thread *thread; /* NULL when it is not running */
    uint8_t halted;      /* a run failed: not started again until the next map */
} wolf_commonrun_slot;

typedef struct {
    wolf_vm *vm;
    const wolf_commons *commons;
    uint32_t nslots;
    wolf_commonrun_slot *slots;
} wolf_commonrun;

enum { WOLF_COMMONRUN_OK = 0, WOLF_COMMONRUN_E_NOMEM = -1 };

/* commons must outlive r, and be the table the VM was given (wolf_vm_set_common_events). */
int wolf_commonrun_init(wolf_commonrun *r, wolf_vm *vm, const wolf_commons *commons);
void wolf_commonrun_free(wolf_commonrun *r);
/* One frame: starts, stops and steps every self-starting common event, in file order. Returns
 * how many are running afterwards. */
uint32_t wolf_commonrun_step(wolf_commonrun *r);
/* A map change: every run stops; each starts again from the top when its condition holds. Runs
 * that failed may start again. */
void wolf_commonrun_map_changed(wolf_commonrun *r);
/* True while an automatic common event is running: nothing else starts (map pages wait). */
int wolf_commonrun_blocking(const wolf_commonrun *r);
int wolf_commonrun_running(const wolf_commonrun *r, uint32_t event);

#ifdef __cplusplus
}
#endif

#endif
