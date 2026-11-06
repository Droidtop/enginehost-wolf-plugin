/*
 * 101 (show message), 102 (choices), 104 and 105 (force them shut). What they do is from the
 * editor's manual, help/04ev_text.html and help/04ev_select.html; 102's layout from the owner's
 * games' data, checked against take-cheeze's mruby-wolf (read only). docs/behaviour/commands.md,
 * "Messages and choices", has the rules and the Unknowns.
 *
 * One window at a time. A thread that shows one waits in its command (the handler steps back and
 * yields each frame) until the plugin answers through wolf_vm_message_closed or
 * wolf_vm_choice_made. Another thread that reaches 101 or 102 meanwhile waits its turn.
 */
#include "vm_internal.h"

#include <stdlib.h>
#include <string.h>

#define CHOICES_MAX 15        /* the count is four bits */
#define FORCED (-2)           /* ui_result after 104 */
#define CANCEL_SEPARATE 0     /* 102's cancel field: a 421 branch */
#define CANCEL_DISABLED 1     /* the cancel key does nothing; 2 + n: as if choice n was picked */
#define EXTRA_LEFT 0x1        /* 102's extra branches (402 markers), in this order [M] */
#define EXTRA_RIGHT 0x2
#define EXTRA_FORCED 0x4

static void stay(wolf_thread *t) { t->stack[t->depth - 1].pc--; }

void wolf_vm_set_ui(wolf_vm *vm, const wolf_ui *ui) {
    vm->has_ui = ui != NULL;
    if (ui) vm->ui = *ui;
    else memset(&vm->ui, 0, sizeof vm->ui);
}

void wolf_vm_message_closed(wolf_vm *vm) {
    if (vm->ui_kind == UI_MESSAGE) vm->ui_answered = 1;
}

void wolf_vm_choice_made(wolf_vm *vm, int32_t index) {
    if (vm->ui_kind != UI_CHOICES) return;
    vm->ui_answered = 1;
    vm->ui_result = index < 0 ? -1 : index;
}

int wolf_vm_ui_busy(const wolf_vm *vm) { return vm->ui_kind != UI_NONE && !vm->ui_answered; }

static void release(wolf_vm *vm) {
    vm->ui_kind = UI_NONE;
    vm->ui_owner = NULL;
    vm->ui_cmd = NULL;
    vm->ui_answered = 0;
}

/* Another thread has the window: wait for it. */
static int busy_elsewhere(const wolf_vm *vm, const wolf_thread *t, const wolf_cmd *c) {
    return vm->ui_kind != UI_NONE && !(vm->ui_owner == t && vm->ui_cmd == c);
}

/* "@N" alone on the first line: system variable 0 (the face number) becomes N, and the line goes
 * (manual 04ev_text.html). Returns where the shown text starts. */
static const char *face_line(wolf_vm *vm, const wolf_thread *t, const char *s) {
    if (s[0] != '@' || s[1] < '0' || s[1] > '9') return s;
    const char *p = s + 1;
    int64_t n = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        if (n > 999999999) return s;
        p++;
    }
    if (*p == '\r') p++;
    if (*p != '\n' && *p != 0) return s;
    wolf_set(vm, t, 9000000, (int32_t)n);
    return *p ? p + 1 : p;
}

int wolf_h_message(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (busy_elsewhere(vm, t, c)) { stay(t); return WOLF_YIELD; }
    if (vm->ui_kind == UI_MESSAGE) { /* ours: closed yet? */
        if (!vm->ui_answered) { stay(t); return WOLF_YIELD; }
        release(vm);
        return WOLF_NEXT;
    }
    char *text = wolf_expand(vm, t, c->nstr ? c->strs[0].text : "");
    if (!text) return WOLF_NEXT;
    const char *shown = face_line(vm, t, text);
    if (!vm->has_ui || !vm->ui.message) {
        wolf_once(vm, ONCE_UI_HEADLESS, "no message window: messages are logged and choices take their first entry");
        wolf_vm_log(vm, WOLF_LOG_INFO, "message: %s", shown);
        free(text);
        return WOLF_NEXT;
    }
    vm->ui_kind = UI_MESSAGE;
    vm->ui_owner = t;
    vm->ui_cmd = c;
    vm->ui_answered = 0;
    vm->ui.message(vm->ui.ctx, shown);
    free(text);
    stay(t);
    return WOLF_YIELD;
}

/* Where a choice's answer leads, once it is known. */
static int follow(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c, int32_t result) {
    uint32_t options = (uint32_t)c->ints[0];
    uint32_t cancel = (options >> 4) & 0xF, extra = (options >> 8) & 0x7;
    if (result == FORCED) {
        /* 402 markers stand for the extra branches in bit order [M] */
        uint32_t nth = (extra & EXTRA_LEFT ? 1u : 0u) + (extra & EXTRA_RIGHT ? 1u : 0u);
        if (extra & EXTRA_FORCED) wolf_flow_to_marker(t, c, 402, nth);
        else wolf_flow_to_marker(t, c, 499, 0); /* no such branch: past the end [M] */
        return WOLF_NEXT;
    }
    uint32_t count = options & 0xF;
    if (cancel >= 2 && cancel - 2 >= count) cancel = CANCEL_DISABLED;
    if (result < 0) {
        if (cancel == CANCEL_SEPARATE) wolf_flow_to_marker(t, c, 421, 0);
        else if (cancel == CANCEL_DISABLED) wolf_flow_to_marker(t, c, 499, 0); /* a UI that cancelled anyway */
        else wolf_flow_to_marker(t, c, 401, cancel - 2u);
        return WOLF_NEXT;
    }
    if ((uint32_t)result >= count) {
        wolf_once(vm, ONCE_BAD_ARGS, "the choice list answered %d of %u; the branch is skipped", (int)result, count);
        wolf_flow_to_marker(t, c, 499, 0);
        return WOLF_NEXT;
    }
    wolf_flow_to_marker(t, c, 401, (uint32_t)result);
    return WOLF_NEXT;
}

int wolf_h_choices(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (busy_elsewhere(vm, t, c)) { stay(t); return WOLF_YIELD; }
    if (vm->ui_kind == UI_CHOICES) {
        if (!vm->ui_answered) { stay(t); return WOLF_YIELD; }
        int32_t result = vm->ui_result;
        release(vm);
        return follow(vm, t, c, result);
    }
    if (c->nint < 1) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 102 has no arguments; its branch is skipped");
        wolf_flow_to_marker(t, c, 499, 0);
        return WOLF_NEXT;
    }
    uint32_t options = (uint32_t)c->ints[0];
    uint32_t count = options & 0xF, cancel = (options >> 4) & 0xF, extra = (options >> 8) & 0x7;
    if (extra & (EXTRA_LEFT | EXTRA_RIGHT))
        wolf_once(vm, ONCE_UI_EXTRA, "a command 102 has left/right-key branches, which are not built; they are never taken");
    if (cancel >= 2 && cancel - 2 >= count) cancel = CANCEL_DISABLED; /* names no choice [M] */

    char *texts[CHOICES_MAX];
    int32_t first = -1;
    for (uint32_t i = 0; i < count; i++) {
        texts[i] = wolf_expand(vm, t, i < c->nstr ? c->strs[i].text : "");
        if (!texts[i]) {
            for (uint32_t k = 0; k < i; k++) free(texts[k]);
            return WOLF_NEXT;
        }
        if (first < 0 && texts[i][0]) first = (int32_t)i;
    }
    int r;
    if (first < 0) {
        /* every choice is empty: the whole command is skipped (manual 04ev_select.html) */
        wolf_flow_to_marker(t, c, 499, 0);
        r = WOLF_NEXT;
    } else if (!vm->has_ui || !vm->ui.choices) {
        wolf_once(vm, ONCE_UI_HEADLESS, "no message window: messages are logged and choices take their first entry");
        wolf_vm_log(vm, WOLF_LOG_INFO, "choices: took \"%s\"", texts[first]);
        r = follow(vm, t, c, first);
    } else {
        vm->ui_kind = UI_CHOICES;
        vm->ui_owner = t;
        vm->ui_cmd = c;
        vm->ui_answered = 0;
        vm->ui_result = -1;
        vm->ui.choices(vm->ui.ctx, (const char *const *)texts, count, cancel != CANCEL_DISABLED);
        stay(t);
        r = WOLF_YIELD;
    }
    for (uint32_t i = 0; i < count; i++) free(texts[i]);
    return r;
}

/* 104: a choice list that is up closes, taking its forced branch. */
int wolf_h_stop_choices(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)t; (void)c;
    vm->flag_stop_choices = 1;
    if (vm->ui_kind == UI_CHOICES && !vm->ui_answered) {
        if (vm->ui.close) vm->ui.close(vm->ui.ctx);
        vm->ui_answered = 1;
        vm->ui_result = FORCED;
    }
    return WOLF_NEXT;
}

/* 105: a message that is up closes as if the player had closed it. */
int wolf_h_stop_message(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    (void)t; (void)c;
    vm->flag_stop_message = 1;
    if (vm->ui_kind == UI_MESSAGE && !vm->ui_answered) {
        if (vm->ui.close) vm->ui.close(vm->ui.ctx);
        vm->ui_answered = 1;
    }
    return WOLF_NEXT;
}
