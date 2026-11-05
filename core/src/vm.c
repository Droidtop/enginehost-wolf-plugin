#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_MAX 1000

struct wolf_vm {
    wolf_host host;
    int flag_interrupt;
    int flag_skip_wait;
    uint8_t unhandled[CODE_MAX + 1];
    int unhandled_other; /* a code outside 0..CODE_MAX was met */
};

void wolf_vm_log(wolf_vm *vm, int level, const char *fmt, ...) {
    if (!vm->host.log) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    vm->host.log(vm->host.ctx, level, msg);
}

/* ---- handlers ------------------------------------------------------------------------------ */

/* 0: the blank command (and the end of every list in the editor). */
static int h_blank(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)t; (void)c;
    return WOLF_NEXT;
}

/* 103: a comment. */
static int h_comment(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)t; (void)c;
    return WOLF_NEXT;
}

/* 104 and 105 set a flag other handlers read (local lane, #858). What reads them is not built yet. */
static int h_flag_interrupt(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)t; (void)c;
    vm->flag_interrupt = 1;
    return WOLF_NEXT;
}
static int h_flag_skip_wait(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)t; (void)c;
    vm->flag_skip_wait = 1;
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
    table[0] = h_blank;
    table[103] = h_comment;
    table[104] = h_flag_interrupt;
    table[105] = h_flag_skip_wait;
    table[106] = h_debug_text;
    done = 1;
}

int wolf_vm_has_handler(int32_t code) {
    init_table();
    return code >= 0 && code <= CODE_MAX && table[code] != NULL;
}

/* ---- the loop ------------------------------------------------------------------------------ */

wolf_vm *wolf_vm_create(const wolf_host *host) {
    init_table();
    wolf_vm *vm = calloc(1, sizeof *vm);
    if (vm && host) vm->host = *host;
    return vm;
}

void wolf_vm_destroy(wolf_vm *vm) { free(vm); }

void wolf_thread_start(wolf_thread *t, const wolf_list *list) {
    t->depth = 0;
    t->wait = 0;
    if (list) {
        t->stack[0].list = list;
        t->stack[0].pc = 0;
        t->depth = 1;
    }
}

int wolf_thread_call(wolf_thread *t, const wolf_list *list) {
    if (t->depth >= WOLF_CALL_DEPTH_MAX) return WOLF_RUN_E_DEPTH;
    t->stack[t->depth].list = list;
    t->stack[t->depth].pc = 0;
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
        if (call->pc >= call->list->count) {
            t->depth--; /* the list ran out: return to the caller */
            continue;
        }
        const wolf_cmd *c = &call->list->cmds[call->pc];
        if (c->code == -1) { /* end of list */
            t->depth--;
            continue;
        }
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

int wolf_vm_flag_interrupt(const wolf_vm *vm) { return vm->flag_interrupt; }
int wolf_vm_flag_skip_wait(const wolf_vm *vm) { return vm->flag_skip_wait; }
