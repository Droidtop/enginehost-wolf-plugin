/*
 * Control flow: conditions and their branches, loops, labels, waits and common-event calls.
 * Behaviour is described in docs/behaviour/commands.md with its source for each command.
 *
 * A command list is flat: what an editor shows nested is a run of commands whose indent byte is
 * one deeper than the command that opens it. A construct (a condition, a loop) and the markers
 * that close it share the opener's indent, so every jump below finds its target by indent.
 */
#include "vm_internal.h"

#include <string.h>

/* ---- moving through a list ------------------------------------------------------------------ */

typedef int (*cmd_test)(const wolf_cmd *c);

/*
 * Moves pc forward to just past the first command at `indent` that `want` accepts. A command
 * shallower than `indent` means the construct has ended without one: pc stops on it and 0 comes
 * back. Running off the end of the list also answers 0 (a malformed construct ends the list
 * rather than looping).
 */
static int seek(wolf_call *call, uint8_t indent, cmd_test want) {
    const wolf_list *l = call->list;
    while (call->pc < l->count) {
        const wolf_cmd *c = &l->cmds[call->pc];
        if (c->indent < indent) return 0;
        call->pc++;
        if (c->indent == indent && want(c)) return 1;
    }
    return 0;
}

static int is_branch_end(const wolf_cmd *c) { return c->code == 499; }
static int is_loop_end(const wolf_cmd *c) { return c->code == 498; }
static int is_case_or_end(const wolf_cmd *c) {
    return c->code == 401 || c->code == 402 || c->code == 420 || c->code == 421 || c->code == 499;
}
static int opens_loop(int32_t code) { return code == 170 || code == 179; }

static wolf_call *top(wolf_thread *t) { return &t->stack[t->depth - 1]; }

/* ---- conditions ------------------------------------------------------------------------------ */

/*
 * After a condition command: walks its case markers in order and stops in the body of the first
 * case whose test passes, else in the else body if there is one, else past the branch end.
 * Tests run lazily, in marker order, so a test after the chosen case is never evaluated.
 */
typedef int (*case_test)(wolf_vm *vm, const wolf_thread *t, const wolf_cmd *c, uint32_t which);

static void choose_case(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c, case_test test) {
    wolf_call *call = top(t);
    for (uint32_t which = 0;; which++) {
        if (!seek(call, c->indent, is_case_or_end)) return;
        int32_t code = call->list->cmds[call->pc - 1].code;
        if (code == 499 || code == 420 || code == 421) return; /* nothing matched / the else body */
        if (test(vm, t, c, which)) return;
    }
}

static int number_case(wolf_vm *vm, const wolf_thread *t, const wolf_cmd *c, uint32_t which) {
    uint32_t cases = (uint32_t)c->ints[0] & 0x0F;
    uint32_t base = 1 + 3 * which;
    if (which >= cases || base + 2 >= c->nint) return 0;
    int32_t left = wolf_value(vm, t, c->ints[base]);
    int32_t right = wolf_value(vm, t, c->ints[base + 1]);
    switch (c->ints[base + 2] & 0xFF) {
    case 0: return left > right;
    case 1: return left >= right;
    case 2: return left == right;
    case 3: return left <= right;
    case 4: return left < right;
    case 5: return left != right;
    case 6: return (left & right) != 0;
    default:
        wolf_once(vm, ONCE_BAD_ARGS, "a command 111 uses comparison %d, which is not known; false",
                  (int)(c->ints[base + 2] & 0xFF));
        return 0;
    }
}

int wolf_h_if_number(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 1) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 111 has no arguments; its branch is skipped");
        seek(top(t), c->indent, is_branch_end);
        return WOLF_NEXT;
    }
    choose_case(vm, t, c, number_case);
    return WOLF_NEXT;
}

static int string_case(wolf_vm *vm, const wolf_thread *t, const wolf_cmd *c, uint32_t which) {
    uint32_t cases = (uint32_t)c->ints[0] & 0x0F;
    if (which >= cases || 1 + which >= c->nint) return 0;
    uint32_t word = (uint32_t)c->ints[1 + which];
    uint32_t flags = word >> 24;
    int32_t ref = (int32_t)(word & 0xFFFFFF);
    const char *left = wolf_get_str(vm, t, ref);
    const char *right;
    if (flags & 1) {
        /* the compared-with string is a variable too: its call values follow the case words, one
         * for each case that has this flag, in case order */
        uint32_t at = 1 + cases;
        for (uint32_t k = 0; k < which; k++)
            if (1 + k < c->nint && ((uint32_t)c->ints[1 + k] >> 24) & 1) at++;
        if (at >= c->nint) return 0;
        right = wolf_get_str(vm, t, c->ints[at]);
    } else {
        right = which < c->nstr ? c->strs[which].text : "";
    }
    switch (flags >> 4) {
    case 0: return strcmp(left, right) == 0;
    case 1: return strcmp(left, right) != 0;
    case 2: return strstr(left, right) != NULL;
    case 3: return strncmp(left, right, strlen(right)) == 0;
    default:
        wolf_once(vm, ONCE_BAD_ARGS, "a command 112 uses comparison %u, which is not known; false", flags >> 4);
        return 0;
    }
}

int wolf_h_if_string(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 1) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 112 has no arguments; its branch is skipped");
        seek(top(t), c->indent, is_branch_end);
        return WOLF_NEXT;
    }
    choose_case(vm, t, c, string_case);
    return WOLF_NEXT;
}

/* A case or else marker reached by running: the chosen body has ended, so the rest of the
 * construct is skipped. */
int wolf_h_case(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm;
    seek(top(t), c->indent, is_branch_end);
    return WOLF_NEXT;
}

int wolf_h_else(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) { return wolf_h_case(vm, t, c); }

int wolf_h_branch_end(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)t; (void)c;
    return WOLF_NEXT;
}

/* ---- loops ----------------------------------------------------------------------------------- */

/* The record of a counted loop's runs left, by the index of its opener. */
static wolf_loop *loop_record(wolf_call *call, uint32_t opener, int make) {
    for (uint32_t i = 0; i < call->nloops; i++)
        if (call->loops[i].opener == opener) return &call->loops[i];
    if (!make) return NULL;
    if (call->nloops == WOLF_LOOPS_MAX) { /* drop the oldest: left by a jump out of its loop */
        memmove(call->loops, call->loops + 1, sizeof call->loops[0] * (WOLF_LOOPS_MAX - 1));
        call->nloops--;
    }
    wolf_loop *r = &call->loops[call->nloops++];
    r->opener = opener;
    r->left = 1;
    return r;
}

static void loop_forget(wolf_call *call, uint32_t opener) {
    for (uint32_t i = 0; i < call->nloops; i++) {
        if (call->loops[i].opener != opener) continue;
        call->loops[i] = call->loops[--call->nloops];
        return;
    }
}

/* The opener of the loop a command at `at` sits in, matched as brackets (a loop end on the way
 * back closes a loop nested before it), or -1. */
static int64_t enclosing_loop(const wolf_call *call, uint32_t at) {
    int depth = 0;
    for (int64_t i = (int64_t)at - 1; i >= 0; i--) {
        int32_t code = call->list->cmds[i].code;
        if (code == 498) depth++;
        else if (opens_loop(code)) {
            if (depth == 0) return i;
            depth--;
        }
    }
    return -1;
}

/* The opener a loop end at `at` closes: the nearest loop opener before it at its indent. */
static int64_t opener_of_end(const wolf_call *call, uint32_t at) {
    uint8_t indent = call->list->cmds[at].indent;
    for (int64_t i = (int64_t)at - 1; i >= 0; i--) {
        const wolf_cmd *c = &call->list->cmds[i];
        if (c->indent == indent && opens_loop(c->code)) return i;
    }
    return -1;
}

/*
 * One run of the loop at `opener` has ended. An endless loop always starts again. A counted loop
 * takes one off its runs left (one, for a loop entered by a jump past its opener) and starts
 * again while any remain; when none do, pc goes past its loop end (already there when the end
 * itself was reached).
 */
static void next_run(wolf_call *call, uint32_t opener, int at_end) {
    const wolf_cmd *o = &call->list->cmds[opener];
    if (o->code == 179) {
        wolf_loop *r = loop_record(call, opener, 0);
        int32_t left = (r ? r->left : 1) - 1;
        if (left <= 0) {
            loop_forget(call, opener);
            if (!at_end) {
                call->pc = opener + 1;
                seek(call, o->indent, is_loop_end);
            }
            return;
        }
        r = loop_record(call, opener, 1);
        r->left = left;
    }
    call->pc = opener + 1;
}

int wolf_h_loop(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)t; (void)c;
    return WOLF_NEXT;
}

int wolf_h_loop_times(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    wolf_call *call = top(t);
    uint32_t opener = call->pc - 1;
    int32_t runs = c->nint >= 1 ? wolf_value(vm, t, c->ints[0]) : 0;
    if (runs <= 0) {
        loop_forget(call, opener);
        seek(call, c->indent, is_loop_end);
        return WOLF_NEXT;
    }
    loop_record(call, opener, 1)->left = runs;
    return WOLF_NEXT;
}

int wolf_h_loop_end(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)c;
    wolf_call *call = top(t);
    int64_t opener = opener_of_end(call, call->pc - 1);
    if (opener >= 0) next_run(call, (uint32_t)opener, 1);
    return WOLF_NEXT;
}

int wolf_h_loop_break(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)c;
    wolf_call *call = top(t);
    int64_t opener = enclosing_loop(call, call->pc - 1);
    if (opener < 0) return WOLF_NEXT;
    loop_forget(call, (uint32_t)opener);
    call->pc = (uint32_t)opener + 1;
    seek(call, call->list->cmds[opener].indent, is_loop_end);
    return WOLF_NEXT;
}

int wolf_h_loop_continue(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)c;
    wolf_call *call = top(t);
    int64_t opener = enclosing_loop(call, call->pc - 1);
    if (opener >= 0) next_run(call, (uint32_t)opener, 0);
    return WOLF_NEXT;
}

/* ---- ending, waiting, labels ------------------------------------------------------------------ */

/* 172: the rest of this list is dropped; a called common event returns to its caller. */
int wolf_h_stop_event(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)c;
    wolf_call *call = top(t);
    call->pc = call->list->count;
    return WOLF_NEXT;
}

/* 180: the thread sleeps for that many frames; 0 does not yield at all. */
int wolf_h_wait(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    int32_t frames = c->nint >= 1 ? wolf_value(vm, t, c->ints[0]) : 0;
    if (frames <= 0) return WOLF_NEXT;
    t->wait = (uint32_t)frames - 1;
    return WOLF_YIELD;
}

int wolf_h_label(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)vm; (void)t; (void)c;
    return WOLF_NEXT;
}

/* 213: on to just past the first label of that name in this list. */
int wolf_h_jump(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    wolf_call *call = top(t);
    const char *name = c->nstr ? c->strs[0].text : "";
    for (uint32_t i = 0; i < call->list->count; i++) {
        const wolf_cmd *l = &call->list->cmds[i];
        if (l->code == 212 && l->nstr && strcmp(l->strs[0].text, name) == 0) {
            call->pc = i + 1;
            return WOLF_NEXT;
        }
    }
    wolf_once(vm, ONCE_LABEL, "a jump to label \"%s\" has no such label in its list; ignored", name);
    return WOLF_NEXT;
}

/* ---- common-event calls ---------------------------------------------------------------------- */

#define COMMON_ID_FIRST 500000
#define COMMON_ID_LAST 599999

/* The self variable a common event's k-th number argument lands in: 0-4, then 10 on, as 5-9
 * hold strings. */
static int32_t argument_slot(uint32_t k) { return k < 5 ? (int32_t)k : (int32_t)k + 5; }

static int call_common(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c, int32_t common) {
    if (common < 0 || (uint32_t)common >= vm->ncommon || !vm->common[common].list) {
        wolf_once(vm, ONCE_BAD_ARGS, "a call to common event %d, which does not exist; ignored", (int)common);
        return WOLF_NEXT;
    }
    uint32_t status = c->nint >= 2 ? (uint32_t)c->ints[1] : 0;
    uint32_t numbers = status & 0x0F;
    uint32_t strings = (status >> 4) & 0x0F;
    int returns = (status >> 24) & 1;
    if (strings)
        wolf_once(vm, ONCE_STRING_OP, "common-event string arguments are not implemented yet; not passed");
    int32_t values[16];
    for (uint32_t k = 0; k < numbers; k++)
        values[k] = 2 + k < c->nint ? wolf_value(vm, t, c->ints[2 + k]) : 0;
    int32_t ret = returns && 2 + numbers < c->nint ? c->ints[2 + numbers] : 0;
    int r = wolf_thread_call(t, vm->common[common].list, common, ret);
    if (r != WOLF_NEXT) return r;
    /* the arguments are written after the push, so "this common event" means the callee */
    for (uint32_t k = 0; k < numbers; k++)
        wolf_set(vm, t, 15000000 + common * 100 + argument_slot(k), values[k]);
    return WOLF_NEXT;
}

int wolf_h_call(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    int32_t target = c->nint >= 1 ? c->ints[0] : -1;
    if (target < COMMON_ID_FIRST || target > COMMON_ID_LAST) {
        wolf_once(vm, ONCE_MAP_EVENT_CALL, "calls to a map event's page (%d) are not implemented yet; ignored",
                  (int)target);
        return WOLF_NEXT;
    }
    return call_common(vm, t, c, target - COMMON_ID_FIRST);
}

int wolf_h_call_by_name(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    const char *name = c->nstr ? c->strs[0].text : "";
    for (uint32_t i = 0; i < vm->ncommon; i++)
        if (vm->common[i].name && strcmp(vm->common[i].name, name) == 0) return call_common(vm, t, c, (int32_t)i);
    wolf_once(vm, ONCE_NAME_ESCAPE, "a call to common event \"%s\", which does not exist; ignored", name);
    return WOLF_NEXT;
}

/* A list has ended: a common event called with a result target hands back its result variable,
 * written in the caller's own context. */
void wolf_return(wolf_vm *vm, wolf_thread *t) {
    wolf_call *done = top(t);
    int32_t common = done->common, target = done->ret_target;
    int have = 0;
    int32_t result = 0;
    if (target && common >= 0 && (uint32_t)common < vm->ncommon && vm->common[common].return_self >= 0 &&
        vm->common[common].return_self < CSELF_COUNT) {
        result = wolf_get(vm, t, 15000000 + common * 100 + vm->common[common].return_self);
        have = 1;
    }
    t->depth--;
    if (have && t->depth > 0) wolf_set(vm, t, target, result);
}
