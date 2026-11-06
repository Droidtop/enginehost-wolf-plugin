/*
 * The built-in message window and choice list. What the runtime does is from the editor's manual:
 * system variables 1-14 (06systemvalue.html), the system database's type 10 window images
 * (05systemtype.html) and the FAQ's note that the window's background is drawn by the game's own
 * common event ("X[共]メッセージウィンドウ"). Choices the manual leaves open are marked in
 * docs/behaviour/drawing.md, "The message window".
 *
 * It is a wolf_ui (vm.h) the core implements itself: ui.c shows a message or a choice list through
 * it as through a plugin's, and the player's keys (wolf_vm_input) answer.
 */
#include "vm_internal.h"
#include "wolf_data.h"

#include <stdlib.h>
#include <string.h>

#define SYS(n) (9000000 + (n))
#define WINDOW_TYPE 10      /* system database: 0 the pause cursor, 1 the choice window, 2 the choice cursor */
#define CHOICES_MAX 15
#define PAD 8               /* choice window padding [M] */
#define FRAMES_PER_CURSOR_STEP 8 /* pause cursor animation [M] */

struct wolf_window {
    wolf_vm *vm;
    /* the message */
    char *text;
    double shown;           /* glyphs typed out so far */
    uint32_t total;         /* glyphs in the message, learnt when it is first drawn (0: not yet) */
    int auto_close;         /* \^: the message ends without a key once typed out */
    int32_t x, y;           /* where it was opened (Sys1, Sys2 at the time) */
    /* the choices */
    char *choice[CHOICES_MAX];
    uint32_t count;
    int cancel;
    int32_t cursor;
    int32_t cx, cy;         /* Sys3, Sys4 at the time */
    /* keys */
    uint32_t keys;
    uint64_t frame;
};

static char *dup(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

static void clear_choices(struct wolf_window *w) {
    for (uint32_t i = 0; i < w->count; i++) free(w->choice[i]);
    w->count = 0;
}

static void clear_message(struct wolf_window *w) {
    free(w->text);
    w->text = NULL;
    wolf_set(w->vm, NULL, SYS(12), 0); /* "文章の表示 実行中?" */
}

static void on_message(void *ctx, const char *text) {
    struct wolf_window *w = ctx;
    clear_message(w);
    w->text = dup(text);
    w->shown = 0;
    w->total = 0;
    w->auto_close = strstr(text, "\\^") != NULL;
    w->x = wolf_get(w->vm, NULL, SYS(1));
    w->y = wolf_get(w->vm, NULL, SYS(2));
    wolf_set(w->vm, NULL, SYS(12), 1);
}

static int shown_choice(const struct wolf_window *w, int32_t i) {
    return i >= 0 && (uint32_t)i < w->count && w->choice[i] && w->choice[i][0];
}

static void on_choices(void *ctx, const char *const *texts, uint32_t count, int cancel) {
    struct wolf_window *w = ctx;
    clear_choices(w);
    if (count > CHOICES_MAX) count = CHOICES_MAX;
    for (uint32_t i = 0; i < count; i++) w->choice[i] = dup(texts[i] ? texts[i] : "");
    w->count = count;
    w->cancel = cancel;
    w->cx = wolf_get(w->vm, NULL, SYS(3));
    w->cy = wolf_get(w->vm, NULL, SYS(4));
    /* system variable 7: where the cursor starts; else the first shown choice */
    w->cursor = wolf_get(w->vm, NULL, SYS(7));
    if (!shown_choice(w, w->cursor))
        for (w->cursor = 0; (uint32_t)w->cursor < count && !shown_choice(w, w->cursor); w->cursor++) {}
}

static void on_close(void *ctx) {
    struct wolf_window *w = ctx;
    clear_message(w);
    clear_choices(w);
}

int wolf_vm_use_window(wolf_vm *vm) {
    if (!vm->win) {
        vm->win = calloc(1, sizeof *vm->win);
        if (!vm->win) return -1;
        vm->win->vm = vm;
    }
    wolf_ui ui = {vm->win, on_message, on_choices, on_close};
    wolf_vm_set_ui(vm, &ui);
    return 0;
}

void wolf_window_free(wolf_vm *vm) {
    if (!vm->win) return;
    free(vm->win->text);
    clear_choices(vm->win);
    free(vm->win);
    vm->win = NULL;
}

/* Moves the cursor by step to the next shown choice, round the list [M: the runtime wraps]. */
static void move(struct wolf_window *w, int step) {
    for (uint32_t k = 0; k < w->count; k++) {
        w->cursor = (int32_t)(((int64_t)w->cursor + step + (int64_t)w->count) % (int64_t)w->count);
        if (shown_choice(w, w->cursor)) return;
    }
}

void wolf_vm_input(wolf_vm *vm, uint32_t keys) {
    struct wolf_window *w = vm->win;
    if (!w) return;
    uint32_t pressed = keys & ~w->keys;
    w->keys = keys;
    w->frame++;
    if (w->text) {
        /* system variable 9: characters a second; 0 or less types out at once [M] */
        int32_t speed = wolf_get(vm, NULL, SYS(9));
        int typed = w->total && w->shown >= w->total;
        if (speed <= 0) w->shown = 1e9;
        else w->shown += speed / 60.0;
        if (pressed & WOLF_KEY_CONFIRM) {
            if (!typed && speed > 0) {
                w->shown = 1e9; /* a key while typing shows the rest [M] */
            } else {
                clear_message(w);
                wolf_vm_message_closed(vm);
            }
        } else if (typed && w->auto_close) {
            clear_message(w);
            wolf_vm_message_closed(vm);
        }
    }
    if (w->count) {
        if (pressed & WOLF_KEY_UP) move(w, -1);
        if (pressed & WOLF_KEY_DOWN) move(w, 1);
        if (pressed & WOLF_KEY_CONFIRM && shown_choice(w, w->cursor)) {
            int32_t picked = w->cursor;
            clear_choices(w);
            wolf_vm_choice_made(vm, picked);
        } else if (pressed & WOLF_KEY_CANCEL && w->cancel) {
            clear_choices(w);
            wolf_vm_choice_made(vm, -1);
        }
    }
}

/* ---- drawing ------------------------------------------------------------------------------ */

static wolf_picture base(const char *content, int kind, double x, double y) {
    wolf_picture p;
    memset(&p, 0, sizeof p);
    p.number = 100000; /* between the bands; only for the log */
    p.kind = kind;
    p.content = content;
    p.div_x = p.div_y = p.pattern = 1;
    p.x = x;
    p.y = y;
    p.opacity = 255;
    p.zoom_x = p.zoom_y = 100;
    p.r = p.g = p.b = 100;
    return p;
}

/* A system database type 10 row: its file, and for the pause cursor its vertical split. */
static const char *window_file(const wolf_vm *vm, uint32_t row, int32_t *split) {
    const struct wolf_db *db = vm->db[WOLF_DB_SYSTEM];
    if (!db) return NULL;
    const char *f = wolf_db_text(db, WINDOW_TYPE, row, 0);
    if (split && !wolf_db_number(db, WINDOW_TYPE, row, 1, split)) *split = 1;
    return f && *f ? f : NULL;
}

void wolf_window_quads(const wolf_vm *vm, const wolf_render_source *src, uint32_t width, uint32_t height,
                       wolf_quad_fn fn, void *ctx) {
    struct wolf_window *w = vm->win;
    if (!w || !src || !fn) return;
    uint32_t size = wolf_text_size(vm);
    double line = size + WOLF_TEXT_SPACING;
    if (w->text) {
        /* -1: the runtime's default place [M: bottom left, four lines up] */
        double x = w->x >= 0 ? w->x : 16, y = w->y >= 0 ? w->y : (double)height - 4 * line - 16;
        wolf_picture p = base(w->text, WOLF_PIC_TEXT, x, y);
        uint32_t limit = w->shown >= 4e9 ? UINT32_MAX : (uint32_t)w->shown;
        uint32_t laid = wolf_one_picture_quads(vm, src, &p, limit, fn, ctx);
        w->total = laid; /* the window is the core's own: learning the length here is safe */
        int32_t split = 1;
        const char *pause = laid && w->shown >= laid ? window_file(vm, 0, &split) : NULL;
        if (pause) {
            /* the pause cursor at the window's top left plus Sys5, Sys6, cycling its pieces [M] */
            wolf_picture c = base(pause, WOLF_PIC_FILE, x + wolf_get((wolf_vm *)vm, NULL, SYS(5)),
                                  y + wolf_get((wolf_vm *)vm, NULL, SYS(6)));
            c.div_y = split > 0 ? split : 1;
            c.pattern = (int32_t)(w->frame / FRAMES_PER_CURSOR_STEP % (uint64_t)c.div_y) + 1;
            wolf_one_picture_quads(vm, src, &c, UINT32_MAX, fn, ctx);
        }
    }
    if (w->count) {
        /* each choice a line; the widest sets the window */
        double widest = 0;
        for (uint32_t i = 0; i < w->count; i++) {
            if (!shown_choice(w, (int32_t)i)) continue;
            double tw = 0;
            wolf_text_layout((wolf_vm *)vm, src, w->choice[i], size, NULL, NULL, &tw, NULL);
            if (tw > widest) widest = tw;
        }
        uint32_t lines = 0;
        for (uint32_t i = 0; i < w->count; i++) lines += shown_choice(w, (int32_t)i) ? 1u : 0u;
        double ww = widest + 2 * PAD, wh = lines * line + 2 * PAD;
        /* Sys3, Sys4 place the window's top centre; -1: the screen's centre [M] */
        double cx = w->cx >= 0 ? w->cx : width / 2.0, cy = w->cy >= 0 ? w->cy : (height - wh) / 2.0;
        double left = cx - ww / 2, top = cy;
        const char *frame = wolf_get((wolf_vm *)vm, NULL, SYS(14)) ? window_file(vm, 1, NULL) : NULL;
        if (frame) {
            wolf_picture f = base(frame, WOLF_PIC_WINDOW, left, top);
            f.div_x = (int32_t)ww;
            f.div_y = (int32_t)wh;
            wolf_one_picture_quads(vm, src, &f, UINT32_MAX, fn, ctx);
        }
        uint32_t row = 0;
        for (uint32_t i = 0; i < w->count; i++) {
            if (!shown_choice(w, (int32_t)i)) continue;
            double ly = top + PAD + row * line;
            const char *cur = (int32_t)i == w->cursor ? window_file(vm, 2, NULL) : NULL;
            if (cur) { /* the cursor as a window around its line [M] */
                wolf_picture k = base(cur, WOLF_PIC_WINDOW, left + PAD / 2.0, ly);
                k.div_x = (int32_t)(ww - PAD);
                k.div_y = (int32_t)line;
                wolf_one_picture_quads(vm, src, &k, UINT32_MAX, fn, ctx);
            }
            wolf_picture t = base(w->choice[i], WOLF_PIC_TEXT, left + PAD, ly);
            wolf_one_picture_quads(vm, src, &t, UINT32_MAX, fn, ctx);
            row++;
        }
    }
}
