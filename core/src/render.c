/*
 * Drawing pictures in software. Wolf's own runtime draws through DX Library on Direct3D; the
 * core draws into the frame Enginehost's isolated process hands it (an isolated core has no GL:
 * Enginehost docs/engine-sandbox.md). What each picture setting means is from the manual,
 * help/04ev_picture.html; how the core turns it into pixels is in docs/behaviour/drawing.md,
 * with each choice marked.
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

/* What a picture shows, as a sampling rule over a source size. */
typedef struct {
    const wolf_image *img;
    int window;
    uint32_t sw, sh;  /* the picture's size before zoom */
    uint32_t ox, oy;  /* the piece's corner in the image (file pictures) */
    uint32_t cell_w, cell_h; /* the image's 3x3 cells (windows) */
} source;

/* One window axis: a corner cell at each end, the middle stretched [M]. */
static uint32_t window_axis(uint32_t at, uint32_t size, uint32_t cell, uint32_t image) {
    if (at < cell) return at;
    if (at >= size - cell) return image - (size - at);
    uint32_t mid = size - 2 * cell, imid = image - 2 * cell;
    return cell + (uint32_t)((uint64_t)(at - cell) * imid / (mid ? mid : 1));
}

static uint32_t sample(const source *s, uint32_t u, uint32_t v) {
    const wolf_image *im = s->img;
    uint32_t x, y;
    if (s->window) {
        x = window_axis(u, s->sw, s->cell_w, im->width);
        y = window_axis(v, s->sh, s->cell_h, im->height);
    } else {
        x = s->ox + u;
        y = s->oy + v;
    }
    if (x >= im->width || y >= im->height) return 0;
    return im->argb[(size_t)y * im->width + x];
}

static uint32_t clamp255(double v) { return v <= 0 ? 0u : v >= 255 ? 255u : (uint32_t)(v + 0.5); }

static uint32_t blend(uint32_t dst, uint32_t src, const wolf_picture *p) {
    double a = (double)(src >> 24) / 255.0 * (p->opacity < 0 ? 0 : p->opacity > 255 ? 255 : p->opacity) / 255.0;
    if (a <= 0) return dst;
    double sc[3] = {(double)((src >> 16) & 255) * p->r / 100.0, (double)((src >> 8) & 255) * p->g / 100.0,
                    (double)(src & 255) * p->b / 100.0};
    double dc[3] = {(double)((dst >> 16) & 255), (double)((dst >> 8) & 255), (double)(dst & 255)};
    uint32_t out[3];
    for (int i = 0; i < 3; i++) {
        double s = sc[i] > 255 ? 255 : sc[i] < 0 ? 0 : sc[i], d = dc[i], r;
        switch (p->blend) {
        case WOLF_BLEND_ADD: r = d + s * a; break;
        case WOLF_BLEND_SUBTRACT: r = d - s * a; break;
        case WOLF_BLEND_MULTIPLY: r = d + (d * s / 255.0 - d) * a; break;
        default: r = d + (s - d) * a; break;
        }
        out[i] = clamp255(r);
    }
    return 0xFF000000u | out[0] << 16 | out[1] << 8 | out[2];
}

static void draw(const wolf_picture *p, const source *s, uint32_t *frame, uint32_t fw, uint32_t fh) {
    double zx = p->zoom_x / 100.0, zy = p->zoom_y / 100.0;
    if (zx == 0 || zy == 0 || s->sw == 0 || s->sh == 0) return;
    double fx, fy;
    anchor_at(p->anchor, &fx, &fy);
    /* the picture's own space: the anchor at the origin, then zoom, then turn about the anchor [M] */
    double ax = fx * s->sw, ay = fy * s->sh;
    double th = p->angle / 10.0 * PI / 180.0, c = cos(th), sn = sin(th);
    /* the bounding box on screen, from the four corners */
    double minx = 1e18, miny = 1e18, maxx = -1e18, maxy = -1e18;
    for (int k = 0; k < 4; k++) {
        double lx = ((k & 1) ? (double)s->sw : 0) - ax, ly = ((k & 2) ? (double)s->sh : 0) - ay;
        lx *= zx;
        ly *= zy;
        double X = p->x + lx * c - ly * sn, Y = p->y + lx * sn + ly * c;
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Y < miny) miny = Y;
        if (Y > maxy) maxy = Y;
    }
    double x0 = floor(minx), y0 = floor(miny), x1 = ceil(maxx), y1 = ceil(maxy);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    for (double Y = y0; Y < y1; Y++) {
        for (double X = x0; X < x1; X++) {
            /* the pixel's centre back into the picture */
            double dx = X + 0.5 - p->x, dy = Y + 0.5 - p->y;
            double lx = (dx * c + dy * sn) / zx + ax, ly = (-dx * sn + dy * c) / zy + ay;
            if (lx < 0 || ly < 0 || lx >= s->sw || ly >= s->sh) continue;
            uint32_t *d = &frame[(size_t)Y * fw + (size_t)X];
            *d = blend(*d, sample(s, (uint32_t)lx, (uint32_t)ly), p);
        }
    }
}

void wolf_render_pictures(const wolf_vm *vm, const wolf_render_source *src, uint32_t *frame, uint32_t width,
                          uint32_t height, int32_t from, int32_t to) {
    if (!src || !src->image || !frame) return;
    for (uint32_t i = 0; i < wolf_vm_picture_count(vm); i++) {
        const wolf_picture *p = wolf_vm_picture_at(vm, i);
        if (p->number < from || p->number > to) continue;
        if (p->kind == WOLF_PIC_TEXT || !p->content || p->content[0] == '<') continue; /* not yet */
        const wolf_image *im = src->image(src->ctx, p->content);
        if (!im || !im->argb || !im->width || !im->height) continue;
        source s;
        memset(&s, 0, sizeof s);
        s.img = im;
        if (p->kind == WOLF_PIC_WINDOW) {
            if (p->div_x <= 0 || p->div_y <= 0 || p->div_x > 16384 || p->div_y > 16384) continue;
            s.window = 1;
            s.sw = (uint32_t)p->div_x;
            s.sh = (uint32_t)p->div_y;
            s.cell_w = im->width / 3;
            s.cell_h = im->height / 3;
            if (s.sw < 2 * s.cell_w) s.cell_w = s.sw / 2; /* smaller than its corners: corners shrink [M] */
            if (s.sh < 2 * s.cell_h) s.cell_h = s.sh / 2;
        } else {
            uint32_t dx = p->div_x > 0 ? (uint32_t)p->div_x : 1, dy = p->div_y > 0 ? (uint32_t)p->div_y : 1;
            if (dx > im->width) dx = im->width;
            if (dy > im->height) dy = im->height;
            s.sw = im->width / dx;
            s.sh = im->height / dy;
            uint32_t piece = p->pattern > 0 ? (uint32_t)p->pattern - 1 : 0;
            if (piece >= dx * dy) piece = 0; /* a piece the split does not have: the first [M] */
            s.ox = piece % dx * s.sw;
            s.oy = piece / dx * s.sh;
        }
        draw(p, &s, frame, width, height);
    }
}
