/*
 * The substitution codes of Wolf text: \v[], \v1[] to \v9[], \self[], \cself[], \sys[], \udb[],
 * \cdb[], \sdb[], \s[] and \sysS[]. From the editor's manual, help/06specialword.html; the order
 * and the rest are in docs/behaviour/commands.md, "Text".
 *
 * The manual gives the codes an order (\v first, \sysS last) and lets a code's brackets hold any
 * code that comes before it, so \cself[\v[12]] works and \v[\cself[2]] does not. The core does
 * one pass per code in that order, each replacing every occurrence whose brackets hold only a
 * number (or, for the databases, numbers joined by ':'). An inner code earlier in the order has
 * been replaced by then. Everything else, the display codes (\c[], \f[], \!, ...) and "\\", is
 * left as it is for whoever shows the text.
 */
#include "vm_internal.h"
#include "wolf_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *p;
    size_t n, cap;
    int cut; /* the text reached WOLF_TEXT_MAX */
    int nomem;
} out;

static void add(out *o, const char *s, size_t n) {
    if (o->nomem) return;
    if (o->n + n > WOLF_TEXT_MAX) {
        n = WOLF_TEXT_MAX - o->n;
        o->cut = 1;
    }
    if (o->n + n + 1 > o->cap) {
        size_t cap = (o->n + n + 1) * 2;
        if (cap > WOLF_TEXT_MAX + 1) cap = WOLF_TEXT_MAX + 1;
        char *q = realloc(o->p, cap);
        if (!q) { o->nomem = 1; return; }
        o->p = q;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

static void add_number(out *o, int32_t v) {
    char b[16];
    int n = snprintf(b, sizeof b, "%d", (int)v);
    add(o, b, (size_t)n);
}

enum { C_V, C_V_SPARE, C_SELF, C_CSELF, C_SYS, C_UDB, C_CDB, C_SDB, C_S, C_SYSS, C_PASSES };

/* The code at s (just past the backslash), as a pass id, and the length of its name. A spare
 * variable's block (1-9) goes to *block. Names are matched longest first: "sysS" before "sys",
 * "self" before "s". */
static int code_at(const char *s, size_t *name_len, int *block) {
    static const struct { const char *name; int id; } names[] = {
        {"sysS", C_SYSS}, {"cself", C_CSELF}, {"self", C_SELF}, {"sys", C_SYS},
        {"udb", C_UDB}, {"cdb", C_CDB}, {"sdb", C_SDB}, {"s", C_S},
    };
    if (s[0] == 'v') {
        if (s[1] >= '1' && s[1] <= '9' && s[2] == '[') { *block = s[1] - '0'; *name_len = 2; return C_V_SPARE; }
        if (s[1] == '[') { *name_len = 1; return C_V; }
        return -1;
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t n = strlen(names[i].name);
        if (strncmp(s, names[i].name, n) == 0 && s[n] == '[') { *name_len = n; return names[i].id; }
    }
    return -1;
}

/* Up to three numbers inside the brackets at s ("[12]", "[1:2:3]"), each optionally negative.
 * Returns how many, with *end just past the ']', or 0 when the brackets hold anything else. */
static int numbers(const char *s, int32_t v[3], const char **end, int allow_three) {
    if (*s != '[') return 0;
    s++;
    int n = 0;
    for (;;) {
        int neg = 0;
        if (*s == '-') { neg = 1; s++; }
        if (*s < '0' || *s > '9') return 0;
        int64_t x = 0;
        while (*s >= '0' && *s <= '9') {
            x = x * 10 + (*s - '0');
            if (x > 2147483647) return 0;
            s++;
        }
        if (n == 3) return 0;
        v[n++] = (int32_t)(neg ? -x : x);
        if (*s == ']') { *end = s + 1; return n; }
        if (*s != ':' || !allow_three) return 0;
        s++;
    }
}

/* What one code stands for, added to o. 0 when the code's brackets do not fit it (left as it is). */
static int substitute(wolf_vm *vm, const wolf_thread *t, int id, int block, const int32_t *v, int n, out *o) {
    switch (id) {
    case C_V: if (n != 1) return 0; add_number(o, wolf_get(vm, t, 2000000 + v[0])); return 1;
    case C_V_SPARE: if (n != 1) return 0; add_number(o, wolf_get(vm, t, 2000000 + 100000 * block + v[0])); return 1;
    case C_SELF: if (n != 1) return 0; add_number(o, wolf_get(vm, t, 1100000 + v[0])); return 1;
    case C_SYS: if (n != 1) return 0; add_number(o, wolf_get(vm, t, 9000000 + v[0])); return 1;
    case C_S: {
        if (n != 1) return 0;
        const char *s = wolf_get_str(vm, t, 3000000 + v[0]);
        add(o, s, strlen(s));
        return 1;
    }
    case C_SYSS: {
        if (n != 1) return 0;
        const char *s = wolf_get_str(vm, t, 9900000 + v[0]);
        add(o, s, strlen(s));
        return 1;
    }
    case C_CSELF: {
        if (n != 1) return 0;
        int32_t ref = 1600000 + v[0];
        if (v[0] >= CSELF_STR_FIRST && v[0] <= CSELF_STR_LAST) {
            const char *s = wolf_get_str(vm, t, ref);
            add(o, s, strlen(s));
        } else {
            add_number(o, wolf_get(vm, t, ref));
        }
        return 1;
    }
    case C_UDB: case C_CDB: case C_SDB: {
        int which = id == C_UDB ? WOLF_DB_USER : id == C_CDB ? WOLF_DB_CHANGEABLE : WOLF_DB_SYSTEM;
        const struct wolf_db *db = vm->db[which];
        for (int i = 0; i < n; i++) if (v[i] < 0) return 0;
        if (!db || (uint32_t)v[0] >= db->ntypes) { add(o, "", 0); return 1; }
        const wolf_db_type *ty = &db->types[v[0]];
        if (n == 1) { /* the type's name */
            add(o, ty->name ? ty->name : "", ty->name ? strlen(ty->name) : 0);
            return 1;
        }
        if ((uint32_t)v[1] >= ty->nrows) return 1;
        if (n == 2) { /* the data's name */
            const char *name = ty->rows[v[1]].name;
            add(o, name ? name : "", name ? strlen(name) : 0);
            return 1;
        }
        int32_t x;
        const char *s = wolf_db_text(db, (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2]);
        if (s) add(o, s, strlen(s));
        else if (wolf_db_number(db, (uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2], &x)) add_number(o, x);
        return 1;
    }
    default:
        return 0;
    }
}

/* One pass for code `pass`, src to a new text. */
static char *one_pass(wolf_vm *vm, const wolf_thread *t, const char *src, int pass, int *cut) {
    out o = {0};
    add(&o, "", 0);
    const char *s = src;
    while (*s) {
        const char *bs = strchr(s, '\\');
        if (!bs) { add(&o, s, strlen(s)); break; }
        add(&o, s, (size_t)(bs - s));
        if (bs[1] == '\\') { add(&o, bs, 2); s = bs + 2; continue; } /* "\\" is a backslash, not a code */
        size_t name_len = 0;
        int block = 0;
        int id = code_at(bs + 1, &name_len, &block);
        int32_t v[3];
        const char *end = NULL;
        int n = id == pass ? numbers(bs + 1 + name_len, v, &end, id == C_UDB || id == C_CDB || id == C_SDB) : 0;
        if (n && substitute(vm, t, id, block, v, n, &o)) {
            s = end;
        } else {
            add(&o, bs, 1);
            s = bs + 1;
        }
        if (o.cut || o.nomem) break;
    }
    if (o.cut) *cut = 1;
    if (o.nomem) { free(o.p); return NULL; }
    return o.p;
}

char *wolf_expand(wolf_vm *vm, const wolf_thread *t, const char *src) {
    size_t n = strlen(src);
    char *cur = malloc(n + 1);
    if (!cur) return NULL;
    memcpy(cur, src, n + 1);
    int cut = 0;
    for (int pass = 0; pass < C_PASSES; pass++) {
        if (!strchr(cur, '\\')) break;
        char *next = one_pass(vm, t, cur, pass, &cut);
        free(cur);
        if (!next) return NULL;
        cur = next;
    }
    if (strstr(cur, "\\xy")) wolf_once(vm, ONCE_TEXT_XY, "\\xy[] and \\xyN[] (XY arrays) are not built yet; left as written");
    if (cut) wolf_once(vm, ONCE_TEXT_LONG, "a text grew past %u bytes and was cut", WOLF_TEXT_MAX);
    return cur;
}

size_t wolf_char_len(const wolf_vm *vm, const char *s) {
    const unsigned char *u = (const unsigned char *)s;
    if (!*u) return 0;
    if (*u == '\\' && u[1]) {
        if (u[1] == '\\') return 2;
        /* a display or substitution code: "\" and a name, then its brackets if any */
        size_t i = 1;
        while ((u[i] >= 'a' && u[i] <= 'z') || (u[i] >= 'A' && u[i] <= 'Z')) i++;
        if (i == 1) {
            /* "\!", "\.", "\^", "\>", "\<", "\-[..]", "\A+" style: one sign, then brackets */
            i = 2;
        } else if (u[i] == '+' || u[i] == '-') {
            if (u[i + 1] != '[') return i + 1; /* \A+ \A- */
            i++;
        }
        if (u[i] == '[') {
            /* \r[text,ruby] holds text, and only "\r[" goes (04ev_string.html) */
            if (i == 2 && u[1] == 'r') return 3;
            size_t j = i + 1;
            while (u[j] && u[j] != ']') j++;
            return u[j] == ']' ? j + 1 : i;
        }
        return i;
    }
    if (vm->cfg.shift_jis) {
        if (((*u >= 0x81 && *u <= 0x9F) || (*u >= 0xE0 && *u <= 0xFC)) && u[1]) return 2;
        return 1;
    }
    size_t len = *u < 0x80 ? 1 : *u >= 0xF0 ? 4 : *u >= 0xE0 ? 3 : *u >= 0xC0 ? 2 : 1;
    for (size_t i = 1; i < len; i++)
        if ((u[i] & 0xC0) != 0x80) return i; /* broken UTF-8: up to the break */
    return len;
}
