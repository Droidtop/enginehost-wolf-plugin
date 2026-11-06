/* See wolf_fonts.h. */
#include "wolf_fonts.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define FONT_FILES_MAX 32
#define FONT_BYTES_MAX (64u << 20)   /* one font file (ours) */
#define GLYPH_BYTES_MAX (32u << 20)  /* rasterised glyphs kept; past this the cache starts over */
#define GLYPHS_MAX 8192u
#define NAMED 4                      /* Game.dat's main font and three sub-fonts */

typedef struct {
    uint8_t *data;
    stbtt_fontinfo info;
    char name[64]; /* the file's name, for the log */
} face;

typedef struct {
    uint32_t font, size, cp;
    char key[48];
    wolf_image img;
    wolf_glyph g;
} glyph;

struct wolf_fonts {
    face faces[FONT_FILES_MAX];
    int nfaces;
    int named[NAMED];  /* the face for font 0-3, or -1 */
    glyph **glyphs;    /* each allocated on its own: a glyph handed out never moves */
    uint32_t nglyphs, capglyphs;
    size_t glyph_bytes;
    wolf_vm *vm;
    int told;
};

static int is_font(const char *name) {
    size_t n = strlen(name);
    return n > 4 && (!strcasecmp(name + n - 4, ".ttf") || !strcasecmp(name + n - 4, ".otf") ||
                     !strcasecmp(name + n - 4, ".ttc"));
}

/* Every font file directly in dir (plus each face of a collection's first). */
static void scan(wolf_fonts *f, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && f->nfaces < FONT_FILES_MAX) {
        if (!is_font(e->d_name)) continue;
        char path[4096];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path) continue;
        uint8_t *data;
        size_t len;
        if (wolf_slurp(path, &data, &len) != 0) continue;
        int offset = len <= FONT_BYTES_MAX && len <= 0x7FFFFFFF ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
        face *fc = &f->faces[f->nfaces];
        if (offset < 0 || !stbtt_InitFont(&fc->info, data, offset)) {
            free(data);
            continue;
        }
        fc->data = data;
        snprintf(fc->name, sizeof fc->name, "%s", e->d_name);
        f->nfaces++;
    }
    closedir(d);
}

/* The face whose family name is name (as Game.dat writes it), or -1. */
static int match(wolf_fonts *f, const char *name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < f->nfaces; i++) {
        const uint8_t *data = f->faces[i].data;
        if (stbtt_FindMatchingFont(data, name, STBTT_MACSTYLE_DONTCARE) >= 0 ||
            stbtt_FindMatchingFont(data, name, STBTT_MACSTYLE_NONE) >= 0)
            return i;
    }
    return -1;
}

wolf_fonts *wolf_fonts_open(const char *folder, const wolf_game *g, wolf_vm *vm) {
    wolf_fonts *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->vm = vm;
    scan(f, folder);
    char data[4096];
    if (wolf_find_ci(folder, "Data", S_IFDIR, data, sizeof data) == 0) scan(f, data);
    for (int k = 0; k < NAMED; k++) {
        const char *name = g && (uint32_t)(3 + k) < g->ntexts ? g->texts[3 + k] : NULL;
        f->named[k] = match(f, name);
        /* a name the game's files don't carry: its first font [M] */
        if (f->named[k] < 0 && f->nfaces) f->named[k] = k == 0 ? 0 : f->named[0];
        if (vm && f->nfaces)
            wolf_vm_log(vm, WOLF_LOG_INFO, "font %d (\"%s\"): %s", k, name ? name : "", f->faces[f->named[k]].name);
    }
    if (vm && !f->nfaces)
        wolf_vm_log(vm, WOLF_LOG_WARN, "the game ships no font files; text is not drawn until Enginehost provides "
                    "a default font set (Droidtop/tracker#815)");
    return f;
}

static void glyphs_clear(wolf_fonts *f) {
    for (uint32_t i = 0; i < f->nglyphs; i++) {
        free((void *)f->glyphs[i]->img.argb);
        free(f->glyphs[i]);
    }
    f->nglyphs = 0;
    f->glyph_bytes = 0;
}

void wolf_fonts_frame(wolf_fonts *f) {
    if (f && (f->glyph_bytes > GLYPH_BYTES_MAX || f->nglyphs >= GLYPHS_MAX)) glyphs_clear(f);
}

const wolf_glyph *wolf_fonts_glyph(wolf_fonts *f, uint32_t font, uint32_t size, uint32_t cp) {
    if (!f || !f->nfaces || font >= NAMED || size == 0) return NULL;
    for (uint32_t i = 0; i < f->nglyphs; i++) {
        glyph *g = f->glyphs[i];
        if (g->font == font && g->size == size && g->cp == cp) return &g->g;
    }
    if (f->nglyphs >= GLYPHS_MAX) return NULL; /* full until the next frame */
    /* the named font first; a character it lacks comes from the game's other fonts, in the order
     * they were found, as Windows fills missing characters from other fonts [M: the order] */
    const stbtt_fontinfo *info = &f->faces[f->named[font]].info;
    int index = stbtt_FindGlyphIndex(info, (int)cp);
    for (int k = 0; index == 0 && cp != ' ' && k < f->nfaces; k++) {
        if (k == f->named[font]) continue;
        int other = stbtt_FindGlyphIndex(&f->faces[k].info, (int)cp);
        if (other) {
            info = &f->faces[k].info;
            index = other;
        }
    }
    if (index == 0 && cp != ' ') return NULL;
    if (f->nglyphs == f->capglyphs) {
        uint32_t cap = f->capglyphs ? f->capglyphs * 2 : 256;
        glyph **gs = realloc(f->glyphs, cap * sizeof *gs);
        if (!gs) return NULL;
        f->glyphs = gs;
        f->capglyphs = cap;
    }
    glyph *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    f->glyphs[f->nglyphs] = g;
    g->font = font;
    g->size = size;
    g->cp = cp;
    snprintf(g->key, sizeof g->key, "\x01glyph:%u:%u:%u", font, size, cp);
    /* the size is the font's pixel height [M: what the runtime measures is Unknown] */
    float scale = stbtt_ScaleForPixelHeight(info, (float)size);
    int ascent, descent, gap, advance, lsb, x0, y0, x1, y1;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &gap);
    stbtt_GetGlyphHMetrics(info, index, &advance, &lsb);
    stbtt_GetGlyphBitmapBox(info, index, scale, scale, &x0, &y0, &x1, &y1);
    g->g.key = g->key;
    g->g.left = x0;
    g->g.top = -y0;
    g->g.advance = (int32_t)(advance * scale + 0.5f);
    g->g.ascent = (int32_t)(ascent * scale + 0.5f);
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w <= 2048 && h <= 2048) {
        uint8_t *alpha = malloc((size_t)w * h);
        uint32_t *argb = malloc((size_t)w * h * 4);
        if (alpha && argb) {
            stbtt_MakeGlyphBitmap(info, alpha, w, h, w, scale, scale, index);
            for (size_t i = 0; i < (size_t)w * h; i++) argb[i] = (uint32_t)alpha[i] << 24 | 0xFFFFFFu;
            g->img.width = (uint32_t)w;
            g->img.height = (uint32_t)h;
            g->img.argb = argb;
            g->g.image = &g->img;
            f->glyph_bytes += (size_t)w * h * 4;
        } else {
            free(argb);
        }
        free(alpha);
    }
    f->nglyphs++;
    return &g->g;
}

void wolf_fonts_close(wolf_fonts *f) {
    if (!f) return;
    glyphs_clear(f);
    free(f->glyphs);
    for (int i = 0; i < f->nfaces; i++) free(f->faces[i].data);
    free(f);
}
