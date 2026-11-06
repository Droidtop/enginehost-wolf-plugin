/*
 * Pictures as quads, and the software renderer that draws them. Wolf's own runtime draws through
 * DX Library on Direct3D. The core turns each picture into textured quads once
 * (wolf_picture_quads); the plugin's GLES renderer draws those, and so does the software renderer
 * here, which is the fallback where no GPU path is available (Droidtop/tracker#858). What each
 * picture setting means is from the manual, help/04ev_picture.html; how the core turns it into
 * geometry and pixels is in docs/behaviour/drawing.md, with each choice marked.
 */
#include "vm_internal.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

/* Where the anchor sits in the picture, as fractions of its width and height. */
static void anchor_at(int anchor, double *fx, double *fy) {
    switch (anchor) {
    case WOLF_ANCHOR_CENTER: *fx = 0.5; *fy = 0.5; break;
    case WOLF_ANCHOR_BOTTOM_LEFT: *fx = 0; *fy = 1; break;
    case WOLF_ANCHOR_TOP_RIGHT: *fx = 1; *fy = 0; break;
    case WOLF_ANCHOR_BOTTOM_RIGHT: *fx = 1; *fy = 1; break;
    case WOLF_ANCHOR_TOP_CENTER: *fx = 0.5; *fy = 0; break;
    case WOLF_ANCHOR_BOTTOM_CENTER: *fx = 0.5; *fy = 1; break;
    default: *fx = 0; *fy = 0; break;
    }
}

/* The picture's own space to the screen: the anchor at (x, y), then zoom, then a turn about the
 * anchor [M]. */
typedef struct {
    double x, y, ax, ay, zx, zy, c, s;
} placement;

static void place(const placement *pl, double lx, double ly, double *X, double *Y) {
    double dx = (lx - pl->ax) * pl->zx, dy = (ly - pl->ay) * pl->zy;
    *X = pl->x + dx * pl->c - dy * pl->s;
    *Y = pl->y + dx * pl->s + dy * pl->c;
}

/* One quad: the picture-space rectangle l (x0, y0, x1, y1) shows the image rectangle u of file.
 * rgb tints it (a text colour; 0xFFFFFF leaves it as it is). */
static void emit_as(const wolf_picture *p, const char *file, const wolf_image *im, uint32_t rgb, const placement *pl,
                    const double l[4], const double u[4], wolf_quad_fn fn, void *ctx) {
    if (l[2] <= l[0] || l[3] <= l[1] || u[2] <= u[0] || u[3] <= u[1]) return;
    wolf_quad q;
    q.file = file;
    q.image = im;
    place(pl, l[0], l[1], &q.x[0], &q.y[0]);
    place(pl, l[2], l[1], &q.x[1], &q.y[1]);
    place(pl, l[2], l[3], &q.x[2], &q.y[2]);
    place(pl, l[0], l[3], &q.x[3], &q.y[3]);
    q.u0 = u[0];
    q.v0 = u[1];
    q.u1 = u[2];
    q.v1 = u[3];
    double o = p->opacity < 0 ? 0 : p->opacity > 255 ? 255 : p->opacity;
    q.opacity = o / 255.0;
    q.r = p->r / 100.0 * ((rgb >> 16) & 255) / 255.0;
    q.g = p->g / 100.0 * ((rgb >> 8) & 255) / 255.0;
    q.b = p->b / 100.0 * (rgb & 255) / 255.0;
    q.blend = p->blend;
    q.number = p->number;
    fn(ctx, &q);
}

static void emit(const wolf_picture *p, const wolf_image *im, const placement *pl, const double l[4],
                 const double u[4], wolf_quad_fn fn, void *ctx) {
    emit_as(p, p->content, im, 0xFFFFFFu, pl, l, u, fn, ctx);
}

static void set_placement(placement *pl, const wolf_picture *p, double sw, double sh) {
    double fx, fy;
    anchor_at(p->anchor, &fx, &fy);
    double th = p->angle / 10.0 * PI / 180.0;
    pl->x = p->x;
    pl->y = p->y;
    pl->ax = fx * sw;
    pl->ay = fy * sh;
    pl->zx = p->zoom_x / 100.0;
    pl->zy = p->zoom_y / 100.0;
    pl->c = cos(th);
    pl->s = sin(th);
}

/* A text picture: laid out once to learn its size (the anchor needs it), then each glyph as a quad. */
typedef struct {
    const wolf_picture *p;
    const placement *pl;
    wolf_quad_fn fn;
    void *ctx;
    uint32_t limit, laid; /* glyphs drawn at most (a message typing out), and laid out so far */
} text_out;

static void glyph_quad(void *ctx, const wolf_glyph *g, double x, double y, uint32_t rgb) {
    text_out *t = ctx;
    if (t->laid++ >= t->limit) return;
    const double l[4] = {x, y, x + g->image->width, y + g->image->height};
    const double u[4] = {0, 0, g->image->width, g->image->height};
    emit_as(t->p, g->key, g->image, rgb, t->pl, l, u, t->fn, t->ctx);
}

uint32_t wolf_text_size(const wolf_vm *vm) {
    /* system variable 8, the base font size for messages and string pictures (manual,
     * 06systemvalue.html); 18 when the game leaves it at 0 [M] */
    int32_t v = wolf_get((wolf_vm *)vm, NULL, 9000008);
    return v > 0 ? (uint32_t)v : WOLF_TEXT_SIZE;
}

static uint32_t text_quads(const wolf_vm *vm, const wolf_render_source *src, const wolf_picture *p, uint32_t limit,
                           wolf_quad_fn fn, void *ctx) {
    double w = 0, h = 0;
    wolf_vm *mvm = (wolf_vm *)vm; /* layout logs once and reads the colour table; nothing else */
    uint32_t size = wolf_text_size(vm);
    wolf_text_layout(mvm, src, p->content, size, NULL, NULL, &w, &h);
    placement pl;
    set_placement(&pl, p, w, h);
    text_out t = {p, &pl, fn, ctx, limit, 0};
    wolf_text_layout(mvm, src, p->content, size, glyph_quad, &t, NULL, NULL);
    return t.laid;
}

uint32_t wolf_one_picture_quads(const wolf_vm *vm, const wolf_render_source *src, const wolf_picture *p,
                                uint32_t glyph_limit, wolf_quad_fn fn, void *ctx) {
    if (p->zoom_x == 0 || p->zoom_y == 0 || !p->content) return 0;
    if (p->kind == WOLF_PIC_TEXT) return src->glyph ? text_quads(vm, src, p, glyph_limit, fn, ctx) : 0;
    if (p->content[0] == '<') return 0; /* the special names: not yet */
    const wolf_image *im = src->image(src->ctx, p->content);
    if (!im || !im->argb || !im->width || !im->height) return 0;
    double sw, sh, ox = 0, oy = 0;
    int window = p->kind == WOLF_PIC_WINDOW;
    if (window) {
        if (p->div_x <= 0 || p->div_y <= 0 || p->div_x > 16384 || p->div_y > 16384) return 0;
        sw = p->div_x;
        sh = p->div_y;
    } else {
        uint32_t dx = p->div_x > 0 ? (uint32_t)p->div_x : 1, dy = p->div_y > 0 ? (uint32_t)p->div_y : 1;
        if (dx > im->width) dx = im->width;
        if (dy > im->height) dy = im->height;
        sw = (double)(im->width / dx);
        sh = (double)(im->height / dy);
        uint32_t piece = p->pattern > 0 ? (uint32_t)p->pattern - 1 : 0;
        if (piece >= dx * dy) piece = 0; /* a piece the split does not have: the first [M] */
        ox = (double)(piece % dx) * sw;
        oy = (double)(piece / dx) * sh;
    }
    placement pl;
    set_placement(&pl, p, sw, sh);
    if (!window) {
        const double l[4] = {0, 0, sw, sh}, u[4] = {ox, oy, ox + sw, oy + sh};
        emit(p, im, &pl, l, u, fn, ctx);
        return 0;
    }
    /* a window: the image cut 3x3; corners keep their size, the rest stretch [M]. A window
     * smaller than its corners shrinks them to fit [M]. */
    double icw = (double)(im->width / 3), ich = (double)(im->height / 3);
    double cw = icw * 2 > sw ? sw / 2 : icw, ch = ich * 2 > sh ? sh / 2 : ich;
    const double lx[4] = {0, cw, sw - cw, sw}, ly[4] = {0, ch, sh - ch, sh};
    const double ux[4] = {0, icw, im->width - icw, im->width}, uy[4] = {0, ich, im->height - ich, im->height};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) {
            const double l[4] = {lx[k], ly[r], lx[k + 1], ly[r + 1]}, u[4] = {ux[k], uy[r], ux[k + 1], uy[r + 1]};
            emit(p, im, &pl, l, u, fn, ctx);
        }
    return 0;
}

void wolf_picture_quads(const wolf_vm *vm, const wolf_render_source *src, int32_t from, int32_t to, wolf_quad_fn fn,
                        void *ctx) {
    if (!src || !src->image || !fn) return;
    for (uint32_t i = 0; i < wolf_vm_picture_count(vm); i++) {
        const wolf_picture *p = wolf_vm_picture_at(vm, i);
        if (p->number < from || p->number > to) continue;
        wolf_one_picture_quads(vm, src, p, UINT32_MAX, fn, ctx);
    }
}

/* ---- the software renderer --------------------------------------------------------------- */

static uint32_t clamp255(double v) { return v <= 0 ? 0u : v >= 255 ? 255u : (uint32_t)(v + 0.5); }

static uint32_t blend(uint32_t dst, uint32_t src, const wolf_quad *q) {
    double a = (double)(src >> 24) / 255.0 * q->opacity;
    if (a <= 0) return dst;
    double sc[3] = {(double)((src >> 16) & 255) * q->r, (double)((src >> 8) & 255) * q->g, (double)(src & 255) * q->b};
    double dc[3] = {(double)((dst >> 16) & 255), (double)((dst >> 8) & 255), (double)(dst & 255)};
    uint32_t out[3];
    for (int i = 0; i < 3; i++) {
        double s = sc[i] > 255 ? 255 : sc[i] < 0 ? 0 : sc[i], d = dc[i], r;
        switch (q->blend) {
        case WOLF_BLEND_ADD: r = d + s * a; break;
        case WOLF_BLEND_SUBTRACT: r = d - s * a; break;
        case WOLF_BLEND_MULTIPLY: r = d + (d * s / 255.0 - d) * a; break;
        default: r = d + (s - d) * a; break;
        }
        out[i] = clamp255(r);
    }
    return 0xFF000000u | out[0] << 16 | out[1] << 8 | out[2];
}

typedef struct {
    uint32_t *frame;
    uint32_t w, h;
} canvas;

/* A quad is a parallelogram: each pixel centre is mapped back into the source rectangle
 * (nearest pixel) [M: the runtime's filtering is Unknown]. */
static void draw_quad(void *ctx, const wolf_quad *q) {
    const canvas *cv = ctx;
    double e1x = q->x[1] - q->x[0], e1y = q->y[1] - q->y[0], e2x = q->x[3] - q->x[0], e2y = q->y[3] - q->y[0];
    double det = e1x * e2y - e1y * e2x;
    if (det == 0) return;
    double minx = q->x[0], maxx = q->x[0], miny = q->y[0], maxy = q->y[0];
    for (int k = 1; k < 4; k++) {
        if (q->x[k] < minx) minx = q->x[k];
        if (q->x[k] > maxx) maxx = q->x[k];
        if (q->y[k] < miny) miny = q->y[k];
        if (q->y[k] > maxy) maxy = q->y[k];
    }
    double x0 = floor(minx), y0 = floor(miny), x1 = ceil(maxx), y1 = ceil(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cv->w) x1 = cv->w;
    if (y1 > cv->h) y1 = cv->h;
    const wolf_image *im = q->image;
    double du = q->u1 - q->u0, dv = q->v1 - q->v0;
    for (double Y = y0; Y < y1; Y++) {
        for (double X = x0; X < x1; X++) {
            double px = X + 0.5 - q->x[0], py = Y + 0.5 - q->y[0];
            double a = (px * e2y - py * e2x) / det, b = (e1x * py - e1y * px) / det;
            if (a < 0 || b < 0 || a >= 1 || b >= 1) continue;
            uint32_t sx = (uint32_t)(q->u0 + a * du), sy = (uint32_t)(q->v0 + b * dv);
            if (sx >= im->width || sy >= im->height) continue;
            uint32_t *d = &cv->frame[(size_t)Y * cv->w + (size_t)X];
            *d = blend(*d, im->argb[(size_t)sy * im->width + sx], q);
        }
    }
}

void wolf_render_pictures(const wolf_vm *vm, const wolf_render_source *src, uint32_t *frame, uint32_t width,
                          uint32_t height, int32_t from, int32_t to) {
    if (!frame) return;
    canvas cv = {frame, width, height};
    wolf_picture_quads(vm, src, from, to, draw_quad, &cv);
}

void wolf_render_window(const wolf_vm *vm, const wolf_render_source *src, uint32_t *frame, uint32_t width,
                        uint32_t height) {
    if (!frame) return;
    canvas cv = {frame, width, height};
    wolf_window_quads(vm, src, width, height, draw_quad, &cv);
}
