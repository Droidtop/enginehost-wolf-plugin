/*
 * Call values (呼び出し値): which variable a number names. From the editor's manual,
 * help/06valueget.html (docs/behaviour/commands.md, "Call values").
 */
#include "vm_internal.h"
#include "wolf_data.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wolf_once(wolf_vm *vm, int which, const char *fmt, ...) {
    if (vm->once[which]) return;
    vm->once[which] = 1;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (vm->host.log) vm->host.log(vm->host.ctx, WOLF_LOG_WARN, msg);
}

/* xorshift32; the seed comes from the config (and later from a save). */
uint32_t wolf_random(wolf_vm *vm, uint32_t bound) {
    uint32_t x = vm->rng ? vm->rng : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    vm->rng = x;
    if (bound == UINT32_MAX) return x;
    return x % (bound + 1u);
}

typedef enum {
    K_NONE, K_LITERAL, K_MAP_SELF, K_CSELF, K_NORMAL, K_SPARE, K_STRING, K_RANDOM, K_SYSTEM,
    K_SYSTEM_STRING, K_CHARA, K_DB, K_REFUSED
} kind;

typedef struct {
    kind k;
    int32_t a; /* owner: map event, common event or spare block; for K_DB the database */
    int32_t b; /* index; for K_DB the type */
    int32_t c, d; /* K_DB only: the data (row) and the field */
} where;

static where decode(const wolf_vm *vm, const wolf_thread *t, int32_t v) {
    where w = {K_NONE, 0, 0, 0, 0};
    if (v < 1000000) { w.k = K_LITERAL; return w; }
    if (v >= 1000000000) {
        /* 1x AA BBBB CC: database x (0 user, 1 changeable, 3 system), type A, data B, field C */
        int32_t which = v / 100000000 - 10, r = v % 100000000;
        if (which == 0) w.a = WOLF_DB_USER;
        else if (which == 1) w.a = WOLF_DB_CHANGEABLE;
        else if (which == 3) w.a = WOLF_DB_SYSTEM;
        else w.a = -1; /* the 1,200,000,000s and 1,400,000,000 up name no database */
        w.k = K_DB; w.b = r / 1000000; w.c = r / 100 % 10000; w.d = r % 100;
        return w;
    }
    if (v >= 15000000) {
        int32_t r = v - 15000000;
        w.k = K_CSELF; w.a = r / 100; w.b = r % 100;
        return w;
    }
    if (v >= 9900000) { w.k = K_SYSTEM_STRING; w.b = v - 9900000; return w; }
    if (v >= 9100000 && v < 9200000) { w.k = K_CHARA; return w; }
    if (v >= 9000000 && v < 9100000) { w.k = K_SYSTEM; w.b = v - 9000000; return w; }
    if (v >= 8000000 && v < 9000000) { w.k = K_RANDOM; w.b = v - 8000000; return w; }
    if (v >= 3000000 && v < 4000000) { w.k = K_STRING; w.b = v - 3000000; return w; }
    if (v >= 2000000 && v < 3000000) {
        int32_t r = v - 2000000;
        if (r < 100000) { w.k = K_NORMAL; w.b = r; }
        else { w.k = K_SPARE; w.a = r / 100000; w.b = r % 100000; }
        return w;
    }
    if (v >= 1600000 && v < 1700000) {
        int32_t ce = t && t->depth ? t->stack[t->depth - 1].common : -1;
        w.k = K_CSELF; w.a = ce; w.b = v - 1600000;
        return w;
    }
    if (v >= 1500000 && v < 1600000) { w.k = K_REFUSED; return w; }
    if (v >= 1100000 && v < 1200000) {
        w.k = K_MAP_SELF; w.a = t ? t->map_event : -1; w.b = v - 1100000;
        return w;
    }
    if (v >= 1000000 && v < 1100000) {
        int32_t r = v - 1000000;
        w.k = K_MAP_SELF; w.a = r / 10; w.b = r % 10;
        return w;
    }
    (void)vm;
    return w;
}

int wolf_is_plain(int32_t v) {
    if (v < 1000000) return 1;
    if (v >= 1000000 && v < 1200000) return 1; /* map event self variables */
    if (v >= 1600000 && v < 1700000) return 1; /* this common event's self variables */
    if (v >= 15000000 && v < 1000000000) return 1;
    return v >= 2000000 && v < 2100000; /* normal variables */
}

/* The slot a numeric call value names, or NULL. */
static int32_t *slot(wolf_vm *vm, where w) {
    switch (w.k) {
    case K_NORMAL:
        return (uint32_t)w.b < vm->cfg.normal ? &vm->normal[w.b] : NULL;
    case K_SPARE:
        if (w.a < 1 || w.a > 9 || (uint32_t)w.b >= vm->cfg.spare) return NULL;
        return &vm->spare[(size_t)(w.a - 1) * vm->cfg.spare + (size_t)w.b];
    case K_SYSTEM:
        return (uint32_t)w.b < vm->cfg.system ? &vm->system[w.b] : NULL;
    case K_MAP_SELF:
        if (w.a < 0 || (uint32_t)w.a >= vm->cfg.map_events || w.b < 0 || w.b >= MAP_SELF_COUNT) return NULL;
        return &vm->map_self[(size_t)w.a * MAP_SELF_COUNT + (size_t)w.b];
    case K_CSELF:
        if (w.a < 0 || (uint32_t)w.a >= vm->ncommon || w.b < 0 || w.b >= CSELF_COUNT) return NULL;
        if (w.b >= CSELF_STR_FIRST && w.b <= CSELF_STR_LAST) return NULL;
        return &vm->cself[(size_t)w.a * CSELF_COUNT + (size_t)w.b];
    default:
        return NULL;
    }
}

static char **str_slot(wolf_vm *vm, where w) {
    switch (w.k) {
    case K_STRING:
        return (uint32_t)w.b < vm->cfg.strings ? &vm->strings[w.b] : NULL;
    case K_SYSTEM_STRING:
        return (uint32_t)w.b < vm->cfg.system_strings ? &vm->system_strings[w.b] : NULL;
    case K_CSELF:
        if (w.a < 0 || (uint32_t)w.a >= vm->ncommon || w.b < CSELF_STR_FIRST || w.b > CSELF_STR_LAST) return NULL;
        return &vm->cself_str[(size_t)w.a * 5 + (size_t)(w.b - CSELF_STR_FIRST)];
    default:
        return NULL;
    }
}

static struct wolf_db *db_of(wolf_vm *vm, where w) {
    return w.k == K_DB && w.a >= 0 ? vm->db[w.a] : NULL;
}

static void not_yet(wolf_vm *vm, where w, int32_t v) {
    if (w.k == K_DB)
        wolf_once(vm, ONCE_DB, "database call value %d names no field of that kind; read as 0 or \"\", writes ignored",
                  (int)v);
    else if (w.k == K_CHARA) wolf_once(vm, ONCE_CHARA, "character call values (%d) are not implemented yet; read as 0", (int)v);
    else if (w.k == K_REFUSED) wolf_once(vm, ONCE_RANGE_1500000, "call value %d is in the 1,500,000 block, which is never written", (int)v);
}

int32_t wolf_get(wolf_vm *vm, const wolf_thread *t, int32_t ref) {
    where w = decode(vm, t, ref);
    if (w.k == K_LITERAL) return ref;
    if (w.k == K_RANDOM) return (int32_t)wolf_random(vm, (uint32_t)w.b);
    int32_t *p = slot(vm, w);
    if (p) return *p;
    int32_t n;
    struct wolf_db *db = db_of(vm, w);
    if (db && wolf_db_number(db, (uint32_t)w.b, (uint32_t)w.c, (uint32_t)w.d, &n)) return n;
    not_yet(vm, w, ref);
    return 0;
}

void wolf_set(wolf_vm *vm, const wolf_thread *t, int32_t ref, int32_t value) {
    where w = decode(vm, t, ref);
    int32_t *p = slot(vm, w);
    struct wolf_db *db = db_of(vm, w);
    if (p) *p = value;
    else if (!db || !wolf_db_set_number(db, (uint32_t)w.b, (uint32_t)w.c, (uint32_t)w.d, value)) not_yet(vm, w, ref);
}

const char *wolf_get_str(wolf_vm *vm, const wolf_thread *t, int32_t ref) {
    where w = decode(vm, t, ref);
    char **p = str_slot(vm, w);
    if (p) return *p ? *p : "";
    struct wolf_db *db = db_of(vm, w);
    const char *s = db ? wolf_db_text(db, (uint32_t)w.b, (uint32_t)w.c, (uint32_t)w.d) : NULL;
    if (s) return s;
    not_yet(vm, w, ref);
    return "";
}

void wolf_set_str(wolf_vm *vm, const wolf_thread *t, int32_t ref, const char *value) {
    where w = decode(vm, t, ref);
    char **p = str_slot(vm, w);
    if (!p) {
        struct wolf_db *db = db_of(vm, w);
        if (!db || !wolf_db_set_text(db, (uint32_t)w.b, (uint32_t)w.c, (uint32_t)w.d, value)) not_yet(vm, w, ref);
        return;
    }
    size_t n = strlen(value);
    char *copy = malloc(n + 1);
    if (!copy) return;
    memcpy(copy, value, n + 1);
    free(*p);
    *p = copy;
}

int32_t wolf_value(wolf_vm *vm, const wolf_thread *t, int32_t v) {
    return v < 1000000 ? v : wolf_get(vm, t, v);
}
