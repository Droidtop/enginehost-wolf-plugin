/*
 * 150, Picture (ピクチャ): show, move, erase, and reset delays, over time, for one picture or a
 * range. What each part does is from the editor's manual, help/04ev_picture.html; the argument
 * layout from all 29,452 real commands in the owner's games, with mruby-wolf (read only) for the
 * first eleven numbers. docs/behaviour/commands.md, "Pictures", has the layout and the Unknowns.
 *
 * The core keeps the state; the plugin draws it. Values are worked out when the command runs;
 * a delayed command keeps them until its turn.
 */
#include "vm_internal.h"

#include <stdlib.h>
#include <string.h>

#define SAME (-1000000)          /* "同値": keep the current value */
#define PICTURES_MAX 65536u      /* pictures shown at once (ours) */
#define PENDING_MAX 65536u       /* delayed commands waiting (ours) */
#define RANGE_MAX 10000          /* pictures one command may touch (ours) */
#define FRAMES_MAX 1000000       /* longest process time or delay (ours) */

enum { OP_SHOW = 0, OP_MOVE = 1, OP_ERASE = 2, OP_DELAY_RESET = 3 };
enum { TYPE_FILE = 0, TYPE_FILE_VAR = 1, TYPE_TEXT = 2, TYPE_WINDOW = 3, TYPE_WINDOW_VAR = 4 };
enum { ZOOM_ONE = 0, ZOOM_APART = 3, ZOOM_SAME = 4 };
#define OPT_RELATIVE 0x10000u    /* bits 16-19 = 1: X and Y add to the current place */
#define OPT_RANGE 0x1000000u
#define OPT_FREE 0x4000000u      /* free transform: four corners (not built) */
#define COLOR_SET 0x2000000      /* the twelfth number: the colour numbers that follow apply */

enum { V_X, V_Y, V_OPACITY, V_ZOOM_X, V_ZOOM_Y, V_ANGLE, V_R, V_G, V_B, V_COUNT };

/* One command for one picture, worked out. */
typedef struct {
    int op;
    int32_t number;
    int has_content;        /* show: a new kind and content */
    int kind;
    char *content;          /* owned */
    int32_t div_x, div_y, pattern;
    int anchor, blend;      /* -1: keep */
    int32_t v[V_COUNT];     /* SAME: keep */
    int relative;
    int32_t frames;         /* process time */
} spec;

typedef struct {
    wolf_picture shown;
    char *content;          /* owned; shown.content points here */
    double from[V_COUNT], to[V_COUNT];
    int32_t frames, done;   /* a move in progress: frames long, done so far */
    int erasing;            /* goes when the move ends */
} pic;

typedef struct {
    spec s;
    int32_t wait;
} pending;

struct wolf_pictures {
    pic *list;              /* sorted by number */
    uint32_t n, cap;
    pending *later;
    uint32_t nlater, caplater;
};

static double *field(wolf_picture *p, int i) {
    switch (i) {
    case V_X: return &p->x;
    case V_Y: return &p->y;
    case V_OPACITY: return &p->opacity;
    case V_ZOOM_X: return &p->zoom_x;
    case V_ZOOM_Y: return &p->zoom_y;
    case V_ANGLE: return &p->angle;
    case V_R: return &p->r;
    case V_G: return &p->g;
    default: return &p->b;
    }
}

static struct wolf_pictures *pics(wolf_vm *vm) {
    if (!vm->pics) vm->pics = calloc(1, sizeof *vm->pics);
    return vm->pics;
}

void wolf_pictures_free(wolf_vm *vm) {
    struct wolf_pictures *ps = vm->pics;
    if (!ps) return;
    for (uint32_t i = 0; i < ps->n; i++) free(ps->list[i].content);
    for (uint32_t i = 0; i < ps->nlater; i++) free(ps->later[i].s.content);
    free(ps->list);
    free(ps->later);
    free(ps);
    vm->pics = NULL;
}

/* The place of number in the list, or where it would go (*found 0). */
static uint32_t find(const struct wolf_pictures *ps, int32_t number, int *found) {
    uint32_t lo = 0, hi = ps->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ps->list[mid].shown.number < number) lo = mid + 1;
        else hi = mid;
    }
    *found = lo < ps->n && ps->list[lo].shown.number == number;
    return lo;
}

static void drop(struct wolf_pictures *ps, uint32_t at) {
    free(ps->list[at].content);
    memmove(&ps->list[at], &ps->list[at + 1], (ps->n - at - 1) * sizeof *ps->list);
    ps->n--;
}

static char *dup(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

/* Starts a move of p toward s's values over s's frames (0: at once). */
static void start_move(pic *p, const spec *s) {
    for (int i = 0; i < V_COUNT; i++) {
        double cur = *field(&p->shown, i);
        p->from[i] = cur;
        double to = cur;
        if (s->v[i] != SAME) {
            to = (double)s->v[i];
            if (s->relative && (i == V_X || i == V_Y)) to = cur + (double)s->v[i];
        }
        p->to[i] = to;
    }
    if (s->anchor >= 0) p->shown.anchor = s->anchor;
    if (s->blend >= 0) p->shown.blend = s->blend;
    p->frames = s->frames;
    p->done = 0;
    if (s->frames <= 0) {
        for (int i = 0; i < V_COUNT; i++) *field(&p->shown, i) = p->to[i];
        p->frames = 0;
    }
}

static void apply(wolf_vm *vm, const spec *s) {
    struct wolf_pictures *ps = pics(vm);
    if (!ps) return;
    int found;
    uint32_t at = find(ps, s->number, &found);
    switch (s->op) {
    case OP_SHOW: {
        if (!found) {
            if (ps->n >= PICTURES_MAX) {
                wolf_once(vm, ONCE_PIC_LIMIT, "over %u pictures at once; picture %d is not shown", PICTURES_MAX,
                          (int)s->number);
                return;
            }
            if (ps->n == ps->cap) {
                uint32_t cap = ps->cap ? ps->cap * 2 : 16;
                pic *l = realloc(ps->list, cap * sizeof *l);
                if (!l) return;
                ps->list = l;
                ps->cap = cap;
            }
            memmove(&ps->list[at + 1], &ps->list[at], (ps->n - at) * sizeof *ps->list);
            ps->n++;
            pic *p = &ps->list[at];
            memset(p, 0, sizeof *p);
            p->shown.number = s->number;
            p->shown.zoom_x = p->shown.zoom_y = 100;
            p->shown.r = p->shown.g = p->shown.b = 100;
            p->shown.opacity = 255;
            /* a new picture with a process time fades in from nothing [M] */
            if (s->frames > 0) p->shown.opacity = 0;
            /* its first place is where it is shown, even when the rest moves over time [M] */
            if (s->v[V_X] != SAME) p->shown.x = s->v[V_X];
            if (s->v[V_Y] != SAME) p->shown.y = s->v[V_Y];
        }
        pic *p = &ps->list[at];
        char *content = dup(s->content ? s->content : "");
        if (!content) return;
        free(p->content);
        p->content = content;
        p->erasing = 0;
        p->shown.kind = s->kind;
        p->shown.content = p->content;
        p->shown.div_x = s->div_x;
        p->shown.div_y = s->div_y;
        p->shown.pattern = s->pattern;
        start_move(p, s);
        break;
    }
    case OP_MOVE:
        if (found) start_move(&ps->list[at], s); /* a picture not shown: nothing (manual) */
        break;
    case OP_ERASE:
        if (!found) break;
        if (s->frames <= 0) {
            drop(ps, at);
        } else { /* fades out, then goes [M] */
            spec fade = *s;
            for (int i = 0; i < V_COUNT; i++) fade.v[i] = SAME;
            fade.v[V_OPACITY] = 0;
            fade.anchor = fade.blend = -1;
            fade.relative = 0;
            start_move(&ps->list[at], &fade);
            ps->list[at].erasing = 1;
        }
        break;
    default:
        break;
    }
}

static void later(wolf_vm *vm, const spec *s, int32_t wait) {
    struct wolf_pictures *ps = pics(vm);
    if (!ps) return;
    if (ps->nlater >= PENDING_MAX) {
        wolf_once(vm, ONCE_PIC_LIMIT, "over %u delayed picture commands; one is dropped", PENDING_MAX);
        return;
    }
    if (ps->nlater == ps->caplater) {
        uint32_t cap = ps->caplater ? ps->caplater * 2 : 16;
        pending *l = realloc(ps->later, cap * sizeof *l);
        if (!l) return;
        ps->later = l;
        ps->caplater = cap;
    }
    pending *p = &ps->later[ps->nlater++];
    p->s = *s;
    p->s.content = s->content ? dup(s->content) : NULL;
    p->wait = wait;
}

/* 150's ops after the delay reset: drop the delayed commands for these numbers. */
static void forget_later(wolf_vm *vm, int32_t first, int32_t last) {
    struct wolf_pictures *ps = vm->pics;
    if (!ps) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < ps->nlater; i++) {
        pending *p = &ps->later[i];
        if (p->s.number >= first && p->s.number <= last) free(p->s.content);
        else ps->later[k++] = *p;
    }
    ps->nlater = k;
}

void wolf_vm_pictures_tick(wolf_vm *vm) {
    struct wolf_pictures *ps = vm->pics;
    if (!ps) return;
    /* moves and fades */
    for (uint32_t i = 0; i < ps->n;) {
        pic *p = &ps->list[i];
        if (p->frames > 0) {
            p->done++;
            double f = (double)p->done / (double)p->frames;
            for (int k = 0; k < V_COUNT; k++) *field(&p->shown, k) = p->from[k] + (p->to[k] - p->from[k]) * f;
            if (p->done >= p->frames) p->frames = 0;
        }
        if (p->frames == 0 && p->erasing) { drop(ps, i); continue; }
        i++;
    }
    /* delayed commands whose turn has come, in the order they were given */
    uint32_t k = 0, n = ps->nlater;
    pending *due = NULL;
    uint32_t ndue = 0;
    if (n) due = malloc(n * sizeof *due);
    for (uint32_t i = 0; i < n; i++) {
        pending *p = &ps->later[i];
        if (--p->wait <= 0 && due) due[ndue++] = *p;
        else ps->later[k++] = *p;
    }
    ps->nlater = k;
    for (uint32_t i = 0; i < ndue; i++) {
        apply(vm, &due[i].s);
        free(due[i].s.content);
    }
    free(due);
}

uint32_t wolf_vm_picture_count(const wolf_vm *vm) { return vm->pics ? vm->pics->n : 0; }

const wolf_picture *wolf_vm_picture_at(const wolf_vm *vm, uint32_t index) {
    return vm->pics && index < vm->pics->n ? &vm->pics->list[index].shown : NULL;
}

static int32_t arg(wolf_vm *vm, const wolf_thread *t, const wolf_cmd *c, int i, int32_t dflt) {
    if (i >= c->nint) return dflt;
    int32_t raw = c->ints[i];
    return raw == SAME ? SAME : wolf_value(vm, t, raw);
}

static int32_t clamp_frames(int32_t f) { return f < 0 ? 0 : f > FRAMES_MAX ? FRAMES_MAX : f; }

int wolf_h_picture(wolf_vm *vm, wolf_thread *t, const wolf_cmd *c) {
    if (c->nint < 2) {
        wolf_once(vm, ONCE_BAD_ARGS, "a command 150 has %d arguments; skipped", c->nint);
        return WOLF_NEXT;
    }
    uint32_t options = (uint32_t)c->ints[0];
    int op = (int)(options & 0xF);
    int32_t first = arg(vm, t, c, 1, 0), last = first;
    int range = (options & OPT_RANGE) != 0;

    spec s;
    memset(&s, 0, sizeof s);
    s.op = op;
    s.anchor = s.blend = -1;
    for (int i = 0; i < V_COUNT; i++) s.v[i] = SAME;
    int32_t delay = 0;

    switch (op) {
    case OP_DELAY_RESET:
        if (range) last = arg(vm, t, c, 2, first);
        if (last < first) { int32_t x = first; first = last; last = x; }
        forget_later(vm, first, last);
        return WOLF_NEXT;
    case OP_ERASE:
        s.frames = clamp_frames(arg(vm, t, c, 2, 0));
        delay = c->nint >= 4 ? arg(vm, t, c, 3, 0) : 0;
        if (range) last = arg(vm, t, c, 5, first);
        break;
    case OP_SHOW:
    case OP_MOVE: {
        if (options & OPT_FREE)
            wolf_once(vm, ONCE_PIC_TRANSFORM, "a command 150 uses free transform (four corners), which is not built; "
                      "drawn as a rectangle");
        int type = (int)((options >> 4) & 0x7);
        uint32_t blend = (options >> 8) & 0xF, anchor = (options >> 12) & 0xF, zoom = (options >> 20) & 0xF;
        s.frames = clamp_frames(arg(vm, t, c, 2, 0));
        s.div_x = arg(vm, t, c, 3, 1);
        s.div_y = arg(vm, t, c, 4, 1);
        s.pattern = arg(vm, t, c, 5, 1);
        s.v[V_OPACITY] = arg(vm, t, c, 6, SAME);
        s.v[V_X] = arg(vm, t, c, 7, SAME);
        s.v[V_Y] = arg(vm, t, c, 8, SAME);
        if (zoom != ZOOM_SAME) {
            s.v[V_ZOOM_X] = arg(vm, t, c, 9, SAME);
            s.v[V_ZOOM_Y] = zoom == ZOOM_APART ? arg(vm, t, c, 18, s.v[V_ZOOM_X]) : s.v[V_ZOOM_X];
        }
        s.v[V_ANGLE] = arg(vm, t, c, 10, SAME);
        if (c->nint > 12 && c->ints[12] == COLOR_SET) {
            s.v[V_R] = arg(vm, t, c, 15, SAME);
            s.v[V_G] = arg(vm, t, c, 16, SAME);
            s.v[V_B] = arg(vm, t, c, 17, SAME);
        }
        delay = c->nint > 13 ? arg(vm, t, c, 13, 0) : 0;
        if (range) last = arg(vm, t, c, 14, first);
        s.relative = (options & OPT_RELATIVE) != 0;
        s.blend = blend == 0xF ? -1 : (int)(blend > 3 ? 0 : blend);
        s.anchor = anchor > 6 ? 0 : (int)anchor;
        if (op == OP_SHOW) {
            s.has_content = 1;
            const char *src = "";
            if (type == TYPE_FILE_VAR || type == TYPE_WINDOW_VAR) src = wolf_get_str(vm, t, c->nint > 11 ? c->ints[11] : 0);
            else if (c->nstr) src = c->strs[0].text;
            s.kind = type == TYPE_TEXT ? WOLF_PIC_TEXT : type >= TYPE_WINDOW ? WOLF_PIC_WINDOW : WOLF_PIC_FILE;
            /* file names may hold substitution codes from 3.345 on; texts always may */
            s.content = wolf_expand(vm, t, src);
            if (!s.content) return WOLF_NEXT;
        }
        break;
    }
    default:
        wolf_once(vm, ONCE_PIC_OP, "a command 150 asks for operation %d, which is not known; skipped", op);
        return WOLF_NEXT;
    }

    if (last < first) { int32_t x = first; first = last; last = x; }
    if ((int64_t)last - first >= RANGE_MAX) {
        wolf_once(vm, ONCE_PIC_LIMIT, "a command 150 covers pictures %d to %d; only the first %d are used", (int)first,
                  (int)last, RANGE_MAX);
        last = first + RANGE_MAX - 1;
    }
    delay = clamp_frames(delay);
    for (int32_t n = first;; n++) {
        s.number = n;
        if (delay > 0) later(vm, &s, delay);
        else apply(vm, &s);
        if (n == last) break;
    }
    free(s.content);
    return WOLF_NEXT;
}
