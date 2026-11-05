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

typedef struct {
    const wolf_list *list;
    uint32_t pc;
} wolf_call;

/* One running event: a stack of command lists (a common-event call pushes one). */
typedef struct {
    wolf_call stack[WOLF_CALL_DEPTH_MAX];
    uint32_t depth;
    uint32_t wait; /* frames still to wait before running */
} wolf_thread;

typedef struct wolf_vm wolf_vm;
typedef int (*wolf_handler)(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);

wolf_vm *wolf_vm_create(const wolf_host *host);
void wolf_vm_destroy(wolf_vm *vm);

/* Starts a thread on a list. */
void wolf_thread_start(wolf_thread *t, const wolf_list *list);
/* Pushes a call; returns WOLF_RUN_E_DEPTH when too deep. */
int wolf_thread_call(wolf_thread *t, const wolf_list *list);

/* Runs one frame of a thread. */
int wolf_vm_run_frame(wolf_vm *vm, wolf_thread *t);

/* The flags commands 104 and 105 set (local lane's static pass; names are ours). */
int wolf_vm_flag_interrupt(const wolf_vm *vm);
int wolf_vm_flag_skip_wait(const wolf_vm *vm);

/* Diagnostics: whether a code has a handler, and the codes met without one. */
int wolf_vm_has_handler(int32_t code);
int wolf_vm_unhandled_seen(const wolf_vm *vm, int32_t code);

/* For handlers in other files. */
void wolf_vm_log(wolf_vm *vm, int level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
