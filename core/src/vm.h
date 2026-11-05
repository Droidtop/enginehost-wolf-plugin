/*
 * The Wolf event interpreter's dispatcher. Structure from the local lane's static pass on
 * Droidtop/tracker#858 (one handler per command code, a program counter per command list,
 * handlers that carry on or yield until the next frame, 5,000,000 commands per frame); each
 * command's behaviour is in docs/behaviour/commands.md with its source.
 *
 * Built to the coordinator's recommendation while the owner was away (2026-11-05); the owner
 * to confirm the shape.
 */
#ifndef ENGINEHOST_WOLF_VM_H
#define ENGINEHOST_WOLF_VM_H

#include <stdint.h>

#include "wolf_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOLF_COMMANDS_PER_FRAME 5000000u
#define WOLF_CALL_DEPTH_MAX 256u

enum wolf_log_level { WOLF_LOG_DEBUG, WOLF_LOG_INFO, WOLF_LOG_WARN, WOLF_LOG_ERROR };

/* What the interpreter needs from the platform. Everything it does outside its own state goes
 * through here, so the core never touches files, the network or the screen directly. */
typedef struct {
    void *ctx;
    void (*log)(void *ctx, int level, const char *message);
} wolf_host;

/* A handler's answer. */
enum {
    WOLF_NEXT = 1,  /* carry on this frame */
    WOLF_YIELD = 0, /* stop until the next frame */
    /* negative: an error that stops the thread */
};

/* What a frame ended with. */
enum {
    WOLF_RUN_YIELD = 1,      /* the thread waits for the next frame */
    WOLF_RUN_DONE = 0,       /* the thread's lists all ended */
    WOLF_RUN_E_LOOP = -1,    /* over the per-frame command budget: the runtime's "infinite loop" */
    WOLF_RUN_E_DEPTH = -2,   /* calls nested past WOLF_CALL_DEPTH_MAX */
    WOLF_RUN_E_COMMAND = -3, /* a handler refused its command */
};

#define WOLF_LOOPS_MAX 32u

typedef struct {
    uint32_t opener; /* index of the 170 or 179 */
    int32_t left;    /* runs left for 179; -1 for 170 */
} wolf_loop;

/* One command list being run: an event page or a common event. */
typedef struct {
    const wolf_list *list;
    uint32_t pc;
    int32_t common;     /* the common event this list is, or -1 */
    int32_t ret_target; /* call value the result goes to, or 0 for none */
    wolf_loop loops[WOLF_LOOPS_MAX]; /* counted loops' runs left, by opener */
    uint32_t nloops;
} wolf_call;

/* One running event: a stack of command lists (a common-event call pushes one). */
typedef struct {
    wolf_call stack[WOLF_CALL_DEPTH_MAX];
    uint32_t depth;
    uint32_t wait;     /* frames still to wait before running */
    int32_t map_event; /* the map event whose self variables 1,100,000+X means, or -1 */
} wolf_thread;

/* A common event, from CommonEvent.dat. */
typedef struct {
    const wolf_list *list;
    const char *name;
    int32_t return_self; /* the self variable (0-99) returned to the caller, or -1 */
} wolf_common;

/* How much of each kind of variable the game has (from its system database). */
typedef struct {
    uint32_t normal;       /* 2,000,000 + X */
    uint32_t spare;        /* each of spare variables 1-9: 2,000,000 + 100,000*Y + X */
    uint32_t strings;      /* 3,000,000 + X */
    uint32_t system;       /* 9,000,000 + X */
    uint32_t system_strings; /* 9,900,000 + X */
    uint32_t map_events;   /* map events whose self variables are kept */
    uint32_t seed;         /* random seed (saves carry it) */
} wolf_config;

typedef struct wolf_vm wolf_vm;
typedef int (*wolf_handler)(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);

wolf_vm *wolf_vm_create(const wolf_host *host, const wolf_config *config);
/* The game's common events; the table and its lists must outlive the VM's use of them. */
void wolf_vm_set_common_events(wolf_vm *vm, const wolf_common *events, uint32_t count);
void wolf_vm_destroy(wolf_vm *vm);

/* Starts a thread on a map event's page (map_event >= 0) or a common event (common >= 0). */
void wolf_thread_start(wolf_thread *t, const wolf_list *list, int32_t map_event, int32_t common);
/* Pushes a call; returns WOLF_RUN_E_DEPTH when too deep. */
int wolf_thread_call(wolf_thread *t, const wolf_list *list, int32_t common, int32_t ret_target);

/* Variables, by call value (docs/behaviour/commands.md, "Call values"). Reads of anything
 * outside the game's variables give 0 (or ""); such writes are ignored. */
int32_t wolf_get(wolf_vm *vm, const wolf_thread *t, int32_t ref);
void wolf_set(wolf_vm *vm, const wolf_thread *t, int32_t ref, int32_t value);
const char *wolf_get_str(wolf_vm *vm, const wolf_thread *t, int32_t ref);
void wolf_set_str(wolf_vm *vm, const wolf_thread *t, int32_t ref, const char *value);

/* Runs one frame of a thread. */
int wolf_vm_run_frame(wolf_vm *vm, wolf_thread *t);

/* The flags 104 (force-stop choices) and 105 (force-stop message) set. */
int wolf_vm_flag_stop_choices(const wolf_vm *vm);
int wolf_vm_flag_stop_message(const wolf_vm *vm);

/* Diagnostics: whether a code has a handler, and the codes met without one. */
int wolf_vm_has_handler(int32_t code);
int wolf_vm_unhandled_seen(const wolf_vm *vm, int32_t code);

/* For handlers in other files. */
void wolf_vm_log(wolf_vm *vm, int level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
