/*
 * 122, Set String (文字列操作). What each assignment does is from the editor's manual,
 * help/04ev_string.html; the argument layout from the owner's games' data (all 3,447 commands 122
 * in the survey games). docs/behaviour/commands.md has the table, and the forms not built.
 */
#include "vm_internal.h"

#include <stdlib.h>
#include <string.h>

enum { SRC_TEXT = 0, SRC_STRING = 1, SRC_INDIRECT = 2, SRC_KEYBOARD = 3 };
enum {
    OP_SET = 0, OP_ADD = 1, OP_COPY_LINE = 2, OP_CUT_LINE = 3, OP_CUT_CHAR = 4,
    OP_FILE_READ = 5, OP_FILE_WRITE = 6, OP_FILE_LIST = 7,
    OP_REMOVE = 8, OP_REPLACE = 9, OP_UP_TO = 10, OP_AFTER = 11,
};
#define TARGET_IS_VARIABLE 0x10 /* 代入先を変数で指定: the target's call value is held by a variable */
#define NOT_FOUND "<<NotFound>>"

static char *dup_n(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static char *join(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na + nb > WOLF_TEXT_MAX) nb = na > WOLF_TEXT_MAX ? 0 : WOLF_TEXT_MAX - na;
    char *d = malloc(na + nb + 1);
    if (!d) return NULL;
    memcpy(d, a, na);
    memcpy(d + na, b, nb);
    d[na + nb] = 0;
    return d;
}

/* Every `find` in s replaced by `with` (find not empty). */
static char *replace_all(const char *s, const char *find, const char *with) {
    size_t nf = strlen(find), nw = strlen(with);
    size_t count = 0;
    for (const char *p = strstr(s, find); p; p = strstr(p + nf, find)) count++;
    size_t ns = strlen(s);
    if (nw > nf && (ns > WOLF_TEXT_MAX || count > (WOLF_TEXT_MAX - ns) / (nw - nf))) return NULL; /* too long */
    size_t n = ns - count * nf + count * nw;
    char *d = malloc(n + 1), *q = d;
    if (!d) return NULL;
    const char *p = s;
    for (const char *m = strstr(p, find); m; m = strstr(p, find)) {
        memcpy(q, p, (size_t)(m - p));
        q += m - p;
        memcpy(q, with, nw);
        q += nw;
        p = m + nf;
    }
    memcpy(q, p, strlen(p) + 1);
    return d;
}

/* The first line of s and the length it takes, newline included ("\r\n" or "\n"). */
static size_t first_line(const char *s, size_t *taken) {
    const char *nl = strchr(s, '\n');
    if (!nl) { *taken = strlen(s); return *taken; }
    *taken = (size_t)(nl - s) + 1;
    return nl > s && nl[-1] == '\r' ? (size_t)(nl - s) - 1 : (size_t)(nl - s);
}

int wolf_h_set_string(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 2) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 122 has %d arguments; skipped", c->nint);
        return WOLF_NEXT;
    }
    uint32_t packed = (uint32_t)c->ints[1];
    int src = (int)(packed & 0x0F);
    int op = (int)((packed >> 8) & 0xFF);
    uint32_t ext = (packed >> 16) & 0xFF;
    int32_t target = c->ints[0];
    if (packed & TARGET_IS_VARIABLE) target = wolf_get(vm, t, target);

    if (ext) {
        wolf_once(vm, ONCE_STR_EXT, "a command 122 uses the extended forms (third byte %u: regular expressions, "
                  "hidden codes), which are not built yet; skipped", ext);
        return WOLF_NEXT;
    }
    if (op == OP_FILE_READ || op == OP_FILE_WRITE || op == OP_FILE_LIST) {
        wolf_once(vm, ONCE_STR_FILE, "a command 122 reads or writes a file (assignment %d), which is not built "
                  "yet; skipped", op);
        return WOLF_NEXT;
    }
    if (op > OP_AFTER) {
        wolf_once(vm, ONCE_STR_SOURCE, "a command 122 uses assignment %d, which is not known; skipped", op);
        return WOLF_NEXT;
    }
    if (src == SRC_KEYBOARD) {
        wolf_once(vm, ONCE_STR_SOURCE, "a command 122 asks for keyboard input, which is not built yet; skipped");
        return WOLF_NEXT;
    }
    if (src > SRC_INDIRECT) {
        wolf_once(vm, ONCE_STR_SOURCE, "a command 122 takes its text from source %d, which is not known; skipped", src);
        return WOLF_NEXT;
    }

    /* The right side: the command's text with its codes replaced, or a string variable named
     * directly or through a variable. A variable right side is also what the cuts shorten. */
    char *right = NULL;
    int32_t right_ref = 0;
    if (src == SRC_TEXT) {
        right = wolf_expand(vm, t, c->nstr ? c->strs[0].text : "");
    } else {
        if (c->nint < 3) {
            wolf_once(vm, ONCE_BAD_ARGS, "a command 122 names no string variable to read; skipped");
            return WOLF_NEXT;
        }
        right_ref = src == SRC_STRING ? c->ints[2] : wolf_get(vm, t, c->ints[2]);
        const char *s = wolf_get_str(vm, t, right_ref);
        right = dup_n(s, strlen(s));
    }
    if (!right) return WOLF_NEXT;

    const char *left = wolf_get_str(vm, t, target);
    char *result = NULL;
    switch (op) {
    case OP_SET:
        result = right;
        right = NULL;
        break;
    case OP_ADD:
        result = join(left, right);
        break;
    case OP_COPY_LINE:
    case OP_CUT_LINE: {
        size_t taken, n = first_line(right, &taken);
        result = dup_n(right, n);
        if (op == OP_CUT_LINE && right_ref) wolf_set_str(vm, t, right_ref, right + taken);
        break;
    }
    case OP_CUT_CHAR: {
        size_t n = wolf_char_len(vm, right);
        result = dup_n(right, n);
        if (right_ref) wolf_set_str(vm, t, right_ref, right + n);
        break;
    }
    case OP_REMOVE:
        result = *right ? replace_all(left, right, "") : dup_n(left, strlen(left));
        break;
    case OP_REPLACE: {
        /* the text to find is the right side; what replaces it is the command's second text */
        char *with = wolf_expand(vm, t, c->nstr > 1 ? c->strs[1].text : "");
        if (with) result = *right ? replace_all(left, right, with) : dup_n(left, strlen(left));
        free(with);
        break;
    }
    case OP_UP_TO:
    case OP_AFTER: {
        const char *m = *right ? strstr(left, right) : NULL;
        if (!m) result = dup_n(NOT_FOUND, strlen(NOT_FOUND));
        else if (op == OP_UP_TO) result = dup_n(left, (size_t)(m - left));
        else result = dup_n(m + strlen(right), strlen(m + strlen(right)));
        break;
    }
    default:
        break;
    }
    if (result) wolf_set_str(vm, t, target, result);
    free(result);
    free(right);
    return WOLF_NEXT;
}
