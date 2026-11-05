/*
 * The map runtime: the map the game is on, which page each of its events shows, and the runs of
 * the automatic and parallel pages, stepped once a frame. Behaviour is docs/behaviour/maps.md,
 * with its source for each rule.
 */
#ifndef ENGINEHOST_WOLF_MAPRUN_H
#define ENGINEHOST_WOLF_MAPRUN_H

#include "vm.h"
#include "wolf_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One event's run, if it has one. */
typedef struct {
    wolf_thread *thread; /* NULL when the event has no run */
    int32_t page;        /* the page the run started on */
    uint8_t trigger;     /* that page's trigger */
    uint8_t halted;      /* a run of this event failed: not started again on this map */
} wolf_maprun_slot;

typedef struct {
    wolf_vm *vm;
    const wolf_map *map; /* NULL before the first map, or after leaving one */
    uint32_t nslots;     /* the map's event count */
    wolf_maprun_slot *slots;
    wolf_thread *probe;  /* holds an event's context while its page conditions are read */
    int stepping;
} wolf_maprun;

enum { WOLF_MAPRUN_OK = 0, WOLF_MAPRUN_E_NOMEM = -1, WOLF_MAPRUN_E_BUSY = -2 };

/* The VM must outlive the runtime. */
int wolf_maprun_init(wolf_maprun *r, wolf_vm *vm);
void wolf_maprun_free(wolf_maprun *r);

/* Makes map the current map (NULL: none). Every run of the map before is dropped. The map must
 * stay loaded while it is current, and the VM's self-variable slots should cover its event ids
 * (wolf_map_config); an event without a slot is logged and never run. Refused with
 * WOLF_MAPRUN_E_BUSY from inside wolf_maprun_step. */
int wolf_maprun_enter(wolf_maprun *r, const wolf_map *map);

/* The page an event of the current map shows: the last one whose used conditions all hold,
 * read with that event's self variables; -1 when none does or the index is out of range. */
int32_t wolf_maprun_active_page(wolf_maprun *r, uint32_t event_index);

/* One frame: for each event in file order, starts or steps the run of its active page when that
 * page is automatic or parallel. Answers how many runs are left. */
uint32_t wolf_maprun_step(wolf_maprun *r);

/* Whether an event has a run, and whether any run of an automatic page is going (which holds
 * the hero and other non-parallel events still, docs/behaviour/maps.md). */
int wolf_maprun_running(const wolf_maprun *r, uint32_t event_index);
int wolf_maprun_blocking(const wolf_maprun *r);

#ifdef __cplusplus
}
#endif

#endif
