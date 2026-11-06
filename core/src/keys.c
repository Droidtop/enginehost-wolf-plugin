/*
 * The player's keys: what is held and newly pressed each frame (wolf_vm_input), and 123, key
 * input. The command's layout and result codes are from the editor's manual (04ev_keyinput.html)
 * and the owner's games; choices the manual leaves open are marked in docs/behaviour/commands.md,
 * "Key input: 123".
 */
#include "vm_internal.h"

#define OPT_DIRS 0x0F    /* which directions count: 0 none, 1 all four, 7 up/down, 8 left/right */
#define OPT_CONFIRM 0x10
#define OPT_CANCEL 0x20
#define OPT_SUB 0x40
#define OPT_WAIT 0x80    /* wait until one is pressed */
#define OPT_KNOWN 0xFFu

void wolf_vm_input(wolf_vm *vm, uint32_t keys) {
    vm->pressed = keys & ~vm->keys;
    vm->keys = keys;
    if (vm->win) wolf_window_input(vm); /* the window answers first, and takes the keys it used */
}

/* The keys 123's options ask about, as WOLF_KEY_* bits. */
static uint32_t wanted(wolf_vm *vm, uint32_t opt) {
    uint32_t k = 0;
    switch (opt & OPT_DIRS) {
    case 0: break;
    case 1: k |= WOLF_KEY_UP | WOLF_KEY_DOWN | WOLF_KEY_LEFT | WOLF_KEY_RIGHT; break;
    case 7: k |= WOLF_KEY_UP | WOLF_KEY_DOWN; break;
    case 8: k |= WOLF_KEY_LEFT | WOLF_KEY_RIGHT; break;
    default:
        wolf_once(vm, ONCE_KEY_DIRS, "key input: direction set %u is not known; read as all four",
                  opt & OPT_DIRS);
        k |= WOLF_KEY_UP | WOLF_KEY_DOWN | WOLF_KEY_LEFT | WOLF_KEY_RIGHT;
    }
    if (opt & OPT_CONFIRM) k |= WOLF_KEY_CONFIRM;
    if (opt & OPT_CANCEL) k |= WOLF_KEY_CANCEL;
    if (opt & OPT_SUB) k |= WOLF_KEY_SUB;
    return k;
}

/* The manual's codes: confirm 10, cancel 11, sub 12, directions as the numeric keypad (2 down,
 * 4 left, 6 right, 8 up); 0 for none. With several at once, the first in that order [M]. */
static const struct { uint32_t key; int32_t code; } CODES[] = {
    {WOLF_KEY_CONFIRM, 10}, {WOLF_KEY_CANCEL, 11}, {WOLF_KEY_SUB, 12},
    {WOLF_KEY_DOWN, 2}, {WOLF_KEY_LEFT, 4}, {WOLF_KEY_RIGHT, 6}, {WOLF_KEY_UP, 8},
};
/* The key that answers among keys (0 for none), and its code in *code. */
static uint32_t answer(uint32_t keys, int32_t *code) {
    for (unsigned i = 0; i < sizeof CODES / sizeof CODES[0]; i++)
        if (keys & CODES[i].key) {
            *code = CODES[i].code;
            return CODES[i].key;
        }
    *code = 0;
    return 0;
}

/* 123: target, options[, key]. */
int wolf_h_key_input(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 2) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 123 has %d arguments; skipped", c->nint);
        return WOLF_NEXT;
    }
    uint32_t opt = (uint32_t)c->ints[1];
    if (c->nint >= 3) {
        /* the keyboard and pad forms: a key code in the third number. Not built. */
        wolf_once(vm, ONCE_KEY_KIND, "key input: the form with a key code (options 0x%x, key %d) is not built; "
                  "the target gets 0", opt, (int)c->ints[2]);
        wolf_set(vm, t, c->ints[0], 0);
        return WOLF_NEXT;
    }
    if (opt & ~OPT_KNOWN)
        wolf_once(vm, ONCE_KEY_OPTION, "key input: options 0x%x carry bits that are not known (0x%x); "
                  "read as the basic key input", opt, opt & ~OPT_KNOWN);
    uint32_t k = wanted(vm, opt);
    if (opt & OPT_WAIT) {
        /* waits for a new press, so a key still held from a message does not answer it [M] */
        int32_t code;
        uint32_t key = answer(vm->pressed & k, &code);
        if (!key) {
            t->stack[t->depth - 1].pc--; /* this command again next frame */
            return WOLF_YIELD;
        }
        vm->pressed &= ~key; /* one press answers one wait [M] */
        wolf_set(vm, t, c->ints[0], code);
        return WOLF_NEXT;
    }
    int32_t code;
    answer(vm->keys & k, &code); /* what is held now */
    wolf_set(vm, t, c->ints[0], code);
    return WOLF_NEXT;
}
