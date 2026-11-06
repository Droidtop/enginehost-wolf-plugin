/*
 * Laying out text with its display codes: string pictures now, the message window next. What each
 * code does is from the editor's manual, help/06specialword.html; the layout choices it leaves
 * open are marked in docs/behaviour/drawing.md, "Text". The substitution codes (\v[], \s[], ...)
 * are replaced before this runs (text.c). Glyphs come from the plugin's font source
 * (wolf_render_source.glyph), so fonts stay pluggable and their rasterising can move into a
 * confined worker (Droidtop/tracker#815).
 */
#include "vm_internal.h"
#include "wolf_data.h"

#include <stdlib.h>
#include <string.h>

#define SIZE_MIN 1
#define SIZE_MAX_PX 512
#define COLOURS_TYPE 12 /* the system database's text colours: R, G, B (manual) */

typedef struct {
    const wolf_glyph *g;
    double x, dy;        /* the pen's x; a one-character vertical nudge (\my) */
    uint32_t rgb;
} item;

typedef struct {
    wolf_vm *vm;
    const wolf_render_source *src;
    wolf_text_fn fn;
    void *ctx;
    /* the pen */
    double x, top;
    uint32_t size, font, rgb;
    double tighten, nudge_x, nudge_y;
    double spacing;      /* added below each line that has a character in it */
    /* the line being built */
    item *items;
    uint32_t n, cap;
    uint32_t line_size;  /* the largest size used on the line */
    int32_t line_ascent;
    double width, height;
} lay;

static uint32_t colour(wolf_vm *vm, int32_t n) {
    const struct wolf_db *db = vm->db[WOLF_DB_SYSTEM];
    int32_t r, g, b;
    if (db && n >= 0 && wolf_db_number(db, COLOURS_TYPE, (uint32_t)n, 0, &r) &&
        wolf_db_number(db, COLOURS_TYPE, (uint32_t)n, 1, &g) && wolf_db_number(db, COLOURS_TYPE, (uint32_t)n, 2, &b)) {
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
        return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
    }
    return 0xFFFFFFu; /* no such colour: white [M] */
}

/* The line is done: its baseline goes below its top by the largest ascent on it. */
static void flush(lay *l, int newline) {
    double base = l->top + (double)l->line_ascent;
    for (uint32_t i = 0; i < l->n; i++) {
        const item *it = &l->items[i];
        if (l->fn && it->g->image)
            l->fn(l->ctx, it->g, it->x + it->g->left, base + it->dy - it->g->top, it->rgb);
    }
    double h = l->line_size ? (double)l->line_size : (double)l->size;
    if (l->top + h > l->height) l->height = l->top + h;
    if (newline) {
        /* the manual: the line spacing is not applied to a line holding nothing at all */
        l->top += h + (l->n ? l->spacing : 0);
    }
    l->n = 0;
    l->line_size = 0;
    l->line_ascent = 0;
}

static void put(lay *l, uint32_t cp) {
    const wolf_glyph *g = l->src->glyph(l->src->ctx, l->font, l->size, cp);
    if (!g && cp != '?') g = l->src->glyph(l->src->ctx, l->font, l->size, '?'); /* no font has it [M] */
    if (!g) return;
    if (l->n == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 64;
        item *it = realloc(l->items, cap * sizeof *it);
        if (!it) return;
        l->items = it;
        l->cap = cap;
    }
    item *it = &l->items[l->n++];
    it->g = g;
    it->x = l->x + l->nudge_x;
    it->dy = l->nudge_y;
    it->rgb = l->rgb;
    l->nudge_x = l->nudge_y = 0; /* \mx and \my move only the next character */
    l->x += (double)g->advance - l->tighten;
    if (l->x > l->width) l->width = l->x;
    if (l->size > l->line_size) l->line_size = l->size;
    if (g->ascent > l->line_ascent) l->line_ascent = g->ascent;
}

/* A number in brackets right after p ("[12]", "[-3]"); *end past the ']'. 0 when there is none. */
static int bracket(const char *p, int32_t *v, const char **end) {
    if (*p != '[') return 0;
    p++;
    int neg = *p == '-';
    if (neg || *p == '+') p++;
    if (*p < '0' || *p > '9') return 0;
    int64_t x = 0;
    while (*p >= '0' && *p <= '9') {
        x = x * 10 + (*p - '0');
        if (x > 1000000) return 0;
        p++;
    }
    if (*p != ']') return 0;
    *v = (int32_t)(neg ? -x : x);
    *end = p + 1;
    return 1;
}

static uint32_t clamp_size(int64_t s) { return (uint32_t)(s < SIZE_MIN ? SIZE_MIN : s > SIZE_MAX_PX ? SIZE_MAX_PX : s); }

/* One code at p (just past the backslash). Returns where the text goes on. */
static const char *code(lay *l, const char *p) {
    int32_t v;
    const char *end;
    if (*p == '\\') { put(l, '\\'); return p + 1; }
    size_t n = 0;
    while ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z')) n++;
#define IS(name) (n == sizeof name - 1 && strncmp(p, name, n) == 0)
    if (IS("c") && bracket(p + n, &v, &end)) { l->rgb = colour(l->vm, v); return end; }
    if (IS("f") && p[1] == '+' && bracket(p + 2, &v, &end)) { l->size = clamp_size((int64_t)l->size + v); return end; }
    if (IS("f") && bracket(p + n, &v, &end)) { l->size = clamp_size(v); return end; }
    if (IS("font") && bracket(p + n, &v, &end)) { l->font = v >= 0 && v <= 3 ? (uint32_t)v : 0; return end; }
    if (IS("space") && bracket(p + n, &v, &end)) { l->spacing = v; return end; }
    if (IS("ax") && bracket(p + n, &v, &end)) { l->x = v; return end; }
    if (IS("ay") && bracket(p + n, &v, &end)) { flush(l, 0); l->top = v; return end; }
    if (IS("mx") && bracket(p + n, &v, &end)) { l->nudge_x = v; return end; }
    if (IS("my") && bracket(p + n, &v, &end)) { l->nudge_y = v; return end; }
    if (n == 0 && *p == '-' && bracket(p + 1, &v, &end)) { l->tighten = v; return end; }
    if (IS("r") && p[1] == '[') {
        /* \r[text,ruby]: the layout loop draws the text and skips the ruby [M: ruby not drawn yet] */
        wolf_once(l->vm, ONCE_TEXT_CODE, "text uses ruby, \\r[...], which is not drawn yet; the base text is");
        return p + 2;
    }
#undef IS
    /* anything else: a display code not built yet (edges, shadows, icons, timing), consumed */
    if (n || *p == '!' || *p == '.' || *p == '^' || *p == '>' || *p == '<') {
        wolf_once(l->vm, ONCE_TEXT_CODE, "text uses display codes not drawn yet (edges, shadows, icons, ...); they "
                  "are skipped");
        const char *q = p + (n ? n : 1);
        if ((*q == '+' || *q == '-') && n) q++;
        if (*q == '[') {
            const char *close = strchr(q, ']');
            if (close) return close + 1;
        }
        return q;
    }
    put(l, '\\'); /* a lone backslash */
    return p;
}

/* The next character's code point, and its length. Shift-JIS double-byte characters need a
 * conversion table that is not built yet: they come out as U+FFFD. */
static uint32_t next_cp(wolf_vm *vm, const unsigned char *s, size_t *len) {
    if (vm->cfg.shift_jis) {
        if (*s >= 0xA1 && *s <= 0xDF) { *len = 1; return 0xFF61u + (*s - 0xA1u); } /* half-width katakana */
        if (((*s >= 0x81 && *s <= 0x9F) || (*s >= 0xE0 && *s <= 0xFC)) && s[1]) {
            *len = 2;
            wolf_once(vm, ONCE_TEXT_SJIS, "Shift-JIS text needs a conversion table that is not built yet; its "
                      "double-byte characters are not drawn");
            return 0xFFFDu;
        }
        *len = 1;
        return *s;
    }
    if (*s < 0x80) { *len = 1; return *s; }
    int n = *s >= 0xF0 ? 4 : *s >= 0xE0 ? 3 : *s >= 0xC0 ? 2 : 1;
    uint32_t cp = n == 4 ? (*s & 0x07u) : n == 3 ? (*s & 0x0Fu) : n == 2 ? (*s & 0x1Fu) : 0xFFFDu;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *len = (size_t)i; return 0xFFFDu; }
        cp = cp << 6 | (s[i] & 0x3Fu);
    }
    *len = (size_t)n;
    return n == 1 ? 0xFFFDu : cp;
}

void wolf_text_layout(wolf_vm *vm, const wolf_render_source *src, const char *text, uint32_t size,
                      wolf_text_fn fn, void *ctx, double *width, double *height) {
    lay l;
    memset(&l, 0, sizeof l);
    l.vm = vm;
    l.src = src;
    l.fn = fn;
    l.ctx = ctx;
    l.size = clamp_size(size);
    l.rgb = colour(vm, 0); /* the default colour is the colour table's first row (manual) */
    l.spacing = WOLF_TEXT_SPACING;
    /* \r[text,ruby]: 0 outside it, 2 in its text (drawn), 1 in its ruby (skipped) */
    int ruby = 0;
    if (src && src->glyph) {
        const char *p = text;
        while (*p) {
            if (ruby == 1) {
                if (*p == ']') ruby = 0;
                p++;
                continue;
            }
            if (ruby == 2 && (*p == ',' || *p == ']')) {
                ruby = *p == ',' ? 1 : 0;
                p++;
                continue;
            }
            if (*p == '\\') {
                const char *q = code(&l, p + 1);
                if (q == p + 3 && p[1] == 'r' && p[2] == '[') ruby = 2;
                p = q;
                continue;
            }
            if (*p == '\r') { p++; continue; }
            if (*p == '\n') { flush(&l, 1); l.x = 0; p++; continue; }
            size_t len;
            uint32_t cp = next_cp(vm, (const unsigned char *)p, &len);
            put(&l, cp);
            p += len;
        }
        flush(&l, 0);
    }
    free(l.items);
    if (width) *width = l.width;
    if (height) *height = l.height;
}
