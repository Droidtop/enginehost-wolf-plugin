/* Shared between the interpreter's source files; not part of the core's interface. */
#ifndef ENGINEHOST_WOLF_VM_INTERNAL_H
#define ENGINEHOST_WOLF_VM_INTERNAL_H

#include "vm.h"

#define CODE_MAX 1000
#define CSELF_COUNT 100
#define CSELF_STR_FIRST 5
#define CSELF_STR_LAST 9
#define MAP_SELF_COUNT 10

/* Things logged once each, so a game that hits them every frame does not flood the log. */
enum {
    ONCE_DB, ONCE_CHARA, ONCE_RANGE_1500000, ONCE_MAP_EVENT_CALL, ONCE_NAME_ESCAPE,
    ONCE_STRING_OP, ONCE_BAD_ARGS, ONCE_LABEL, ONCE_COUNT
};

struct wolf_vm {
    wolf_host host;
    wolf_config cfg;
    int flag_stop_choices;
    int flag_stop_message;
    uint8_t unhandled[CODE_MAX + 1];
    int unhandled_other;
    uint8_t once[ONCE_COUNT];

    int32_t *normal;
    int32_t *spare; /* 9 blocks of cfg.spare */
    int32_t *system;
    char **strings;
    char **system_strings;
    int32_t *map_self; /* cfg.map_events x MAP_SELF_COUNT */

    const wolf_common *common;
    uint32_t ncommon;
    int32_t *cself;     /* ncommon x CSELF_COUNT */
    char **cself_str;   /* ncommon x 5 (self variables 5-9) */

    uint32_t rng;
};

void wolf_once(wolf_vm *vm, int which, const char *fmt, ...);
uint32_t wolf_random(wolf_vm *vm, uint32_t bound); /* 0..bound inclusive */

/* An operand: a literal from -999,999 to 999,999 (and, by our reading, anything below), else the
 * variable that call value names. */
int32_t wolf_value(wolf_vm *vm, const wolf_thread *t, int32_t v);
/* Whether a call value is one the "fast" common-event arithmetic covers: a literal, a self
 * variable or a normal variable (manual, 計算式の説明). */
int wolf_is_plain(int32_t v);

/* Handlers, by file. */
int wolf_h_set_variable(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);   /* 121 */
int wolf_h_if_number(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);      /* 111 */
int wolf_h_if_string(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);      /* 112 */
int wolf_h_case(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 401, 402, 421 */
int wolf_h_else(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 420 */
int wolf_h_branch_end(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 499 */
int wolf_h_loop(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 170 */
int wolf_h_loop_times(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 179 */
int wolf_h_loop_end(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);       /* 498 */
int wolf_h_loop_break(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 171 */
int wolf_h_loop_continue(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);  /* 176 */
int wolf_h_stop_event(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);     /* 172 */
int wolf_h_wait(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 180 */
int wolf_h_label(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);          /* 212 */
int wolf_h_jump(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 213 */
int wolf_h_call(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);           /* 210 */
int wolf_h_call_by_name(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c);   /* 300 */

/* Called by the loop when a list ends, to hand back a common event's result. */
void wolf_return(wolf_vm *vm, wolf_thread *t);

#endif
