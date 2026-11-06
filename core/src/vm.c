/* The dispatcher: the handler table, the per-frame loop and the VM's storage. */
#include "vm_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Caps on what a game's configuration may ask for, so a bad database cannot exhaust memory. */
#define VARIABLES_CAP 1000000u
#define MAP_EVENTS_CAP 100000u
#define COMMON_EVENTS_CAP 100000u

void wolf_vm_log(wolf_vm *vm, int level, const char *fmt, ...) {
    if (!vm->host.log) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    vm->host.log(vm->host.ctx, level, msg);
}

/* ---- simple handlers --------------------------------------------------------------------- */

static int h_nothing(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) { /* 0, 99, 103 */
    (void)vm; (void)t; (void)c;
    return WOLF_NEXT;
}

/* 106: the runtime can raise a modal "Message" box here. The core never blocks: it logs. */
static int h_debug_text(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)t;
    wolf_vm_log(vm, WOLF_LOG_INFO, "debug text: %s", c->nstr ? c->strs[0].text : "");
    return WOLF_NEXT;
}

static wolf_handler table[CODE_MAX + 1];

static void init_table(void) {
    static int done;
    if (done) return;
    table[0] = h_nothing;
    table[99] = h_nothing;
    table[103] = h_nothing;
    table[101] = wolf_h_message;
    table[102] = wolf_h_choices;
    table[104] = wolf_h_stop_choices;
    table[105] = wolf_h_stop_message;
    table[150] = wolf_h_picture;
    table[106] = h_debug_text;
    table[111] = wolf_h_if_number;
    table[112] = wolf_h_if_string;
    table[121] = wolf_h_set_variable;
    table[170] = wolf_h_loop;
    table[171] = wolf_h_loop_break;
    table[172] = wolf_h_stop_event;
    table[176] = wolf_h_loop_continue;
    table[179] = wolf_h_loop_times;
    table[180] = wolf_h_wait;
    table[210] = wolf_h_call;
    table[212] = wolf_h_label;
    table[213] = wolf_h_jump;
    table[122] = wolf_h_set_string;
    table[250] = wolf_h_database;
    table[300] = wolf_h_call_by_name;
    table[401] = wolf_h_case;
    table[402] = wolf_h_case;
    table[420] = wolf_h_else;
    table[421] = wolf_h_case;
    table[498] = wolf_h_loop_end;
    table[499] = wolf_h_branch_end;
    done = 1;
}

int wolf_vm_has_handler(int32_t code) {
    init_table();
    return code >= 0 && code <= CODE_MAX && table[code] != NULL;
}

/* ---- the VM -------------------------------------------------------------------------------- */

static void free_strings(char **s, size_t n) {
    if (!s) return;
    for (size_t i = 0; i < n; i++) free(s[i]);
    free(s);
}

wolf_vm *wolf_vm_create(const wolf_host *host, const wolf_config *config) {
    init_table();
    wolf_vm *vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;
    if (host) vm->host = *host;
    if (config) vm->cfg = *config;
    wolf_config *c = &vm->cfg;
    if (c->normal > VARIABLES_CAP) c->normal = VARIABLES_CAP;
    if (c->spare > 100000u) c->spare = 100000u;
    if (c->strings > VARIABLES_CAP) c->strings = VARIABLES_CAP;
    if (c->system > 100000u) c->system = 100000u;
    if (c->system_strings > 100000u) c->system_strings = 100000u;
    if (c->map_events > MAP_EVENTS_CAP) c->map_events = MAP_EVENTS_CAP;
    vm->rng = c->seed;
    vm->normal = calloc(c->normal + 1u, sizeof(int32_t));
    vm->spare = calloc((size_t)c->spare * 9u + 1u, sizeof(int32_t));
    vm->system = calloc(c->system + 1u, sizeof(int32_t));
    vm->strings = calloc(c->strings + 1u, sizeof(char *));
    vm->system_strings = calloc(c->system_strings + 1u, sizeof(char *));
    vm->map_self = calloc((size_t)c->map_events * MAP_SELF_COUNT + 1u, sizeof(int32_t));
    if (!vm->normal || !vm->spare || !vm->system || !vm->strings || !vm->system_strings || !vm->map_self) {
        wolf_vm_destroy(vm);
        return NULL;
    }
    return vm;
}

void wolf_vm_set_common_events(wolf_vm *vm, const wolf_common *events, uint32_t count) {
    if (count > COMMON_EVENTS_CAP) count = COMMON_EVENTS_CAP;
    free(vm->cself);
    free_strings(vm->cself_str, (size_t)vm->ncommon * 5u);
    vm->common = events;
    vm->ncommon = 0;
    vm->cself = calloc((size_t)count * CSELF_COUNT + 1u, sizeof(int32_t));
    vm->cself_str = calloc((size_t)count * 5u + 1u, sizeof(char *));
    if (vm->cself && vm->cself_str) vm->ncommon = count;
}

void wolf_vm_set_databases(wolf_vm *vm, struct wolf_db *user, struct wolf_db *changeable,
                           struct wolf_db *system) {
    vm->db[WOLF_DB_USER] = user;
    vm->db[WOLF_DB_CHANGEABLE] = changeable;
    vm->db[WOLF_DB_SYSTEM] = system;
}

void wolf_vm_destroy(wolf_vm *vm) {
    if (!vm) return;
    free(vm->normal);
    free(vm->spare);
    free(vm->system);
    free_strings(vm->strings, vm->cfg.strings);
    free_strings(vm->system_strings, vm->cfg.system_strings);
    free(vm->map_self);
    free(vm->cself);
    free_strings(vm->cself_str, (size_t)vm->ncommon * 5u);
    wolf_pictures_free(vm);
    free(vm);
}

void wolf_thread_start(wolf_thread *t, const wolf_list *list, int32_t map_event, int32_t common) {
    t->depth = 0;
    t->wait = 0;
    t->map_event = map_event;
    if (list) wolf_thread_call(t, list, common, 0);
}

int wolf_thread_call(wolf_thread *t, const wolf_list *list, int32_t common, int32_t ret_target) {
    if (t->depth >= WOLF_CALL_DEPTH_MAX) return WOLF_RUN_E_DEPTH;
    wolf_call *c = &t->stack[t->depth];
    c->list = list;
    c->pc = 0;
    c->common = common;
    c->ret_target = ret_target;
    c->nloops = 0;
    t->depth++;
    return WOLF_NEXT;
}

static void note_unhandled(wolf_vm *vm, int32_t code) {
    if (code >= 0 && code <= CODE_MAX) {
        if (vm->unhandled[code]) return;
        vm->unhandled[code] = 1;
    } else {
        if (vm->unhandled_other) return;
        vm->unhandled_other = 1;
    }
    wolf_vm_log(vm, WOLF_LOG_WARN, "command %d is not implemented yet; skipped", (int)code);
}

int wolf_vm_unhandled_seen(const wolf_vm *vm, int32_t code) {
    if (code >= 0 && code <= CODE_MAX) return vm->unhandled[code];
    return vm->unhandled_other;
}

int wolf_vm_run_frame(wolf_vm *vm, wolf_thread *t) {
    if (t->wait > 0) {
        t->wait--;
        return WOLF_RUN_YIELD;
    }
    uint32_t budget = WOLF_COMMANDS_PER_FRAME;
    while (t->depth > 0) {
        wolf_call *call = &t->stack[t->depth - 1];
        if (call->pc >= call->list->count || call->list->cmds[call->pc].code == -1) {
            wolf_return(vm, t); /* the list ended: hand back a result, return to the caller */
            continue;
        }
        const wolf_cmd *c = &call->list->cmds[call->pc];
        if (budget-- == 0) {
            wolf_vm_log(vm, WOLF_LOG_ERROR, "over %u commands in one frame: stopped as an infinite loop",
                        WOLF_COMMANDS_PER_FRAME);
            return WOLF_RUN_E_LOOP;
        }
        call->pc++; /* handlers see pc on the next command; a jump sets it, a retry steps back */
        wolf_handler h = c->code >= 0 && c->code <= CODE_MAX ? table[c->code] : NULL;
        if (!h) {
            note_unhandled(vm, c->code);
            continue;
        }
        int r = h(vm, t, c);
        if (r == WOLF_YIELD) return WOLF_RUN_YIELD;
        if (r < 0) return r;
    }
    return WOLF_RUN_DONE;
}

int wolf_vm_flag_stop_choices(const wolf_vm *vm) { return vm->flag_stop_choices; }
int wolf_vm_flag_stop_message(const wolf_vm *vm) { return vm->flag_stop_message; }
