/* See wolf_frame.h. Images are decoded with stb_image (PNG, JPEG, BMP, TGA), from the vendor/stb
 * submodule. */
#include "wolf_frame.h"

#include <stdlib.h>
#include <string.h>

#include "wolf_commonrun.h"
#include "wolf_decode.h"
#include "wolf_files.h"
#include "wolf_mixer.h"

#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define CACHE_BYTES_MAX (192u << 20) /* decoded images kept; past this the cache starts over */
#define SOUND_BYTES_MAX (256u << 20) /* decoded sounds kept; past this, ones not playing go */
#define SOUNDS_MAX 4096u

typedef struct {
    char *name;
    wolf_pcm pcm; /* stereo NULL: the game has no such sound, or it cannot be decoded */
} sound;

typedef struct {
    char *name;
    wolf_image img; /* argb NULL: the game has no such image (remembered, so it is looked up once) */
} cached;

struct wolf_frame {
    wolf_open_game *g;
    wolf_vm *vm;
    wolf_files *files;
    wolf_commonrun run;
    cached *cache;
    uint32_t ncache, capcache;
    size_t cache_bytes;
    wolf_mixer *mixer; /* NULL without sound */
    sound *sounds;
    uint32_t nsounds;
    size_t sound_bytes;
};

static void cache_clear(wolf_frame *f) {
    for (uint32_t i = 0; i < f->ncache; i++) {
        free(f->cache[i].name);
        free((void *)f->cache[i].img.argb);
    }
    f->ncache = 0;
    f->cache_bytes = 0;
}

static const wolf_image *decode(wolf_frame *f, const char *name, wolf_image *out) {
    uint8_t *data;
    size_t len;
    memset(out, 0, sizeof *out);
    if (wolf_files_read(f->files, name, &data, &len) != 0) return NULL;
    int w = 0, h = 0, n = 0;
    stbi_uc *rgba = len <= 0x7FFFFFFF ? stbi_load_from_memory(data, (int)len, &w, &h, &n, 4) : NULL;
    free(data);
    if (!rgba) return NULL;
    size_t count = (size_t)w * (size_t)h;
    uint32_t *argb = malloc(count * sizeof *argb);
    if (argb) {
        for (size_t i = 0; i < count; i++) {
            const stbi_uc *p = &rgba[i * 4];
            argb[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
        out->width = (uint32_t)w;
        out->height = (uint32_t)h;
        out->argb = argb;
    }
    stbi_image_free(rgba);
    return argb ? out : NULL;
}

static const wolf_image *image(void *ctx, const char *name) {
    wolf_frame *f = ctx;
    for (uint32_t i = 0; i < f->ncache; i++)
        if (strcmp(f->cache[i].name, name) == 0) return f->cache[i].img.argb ? &f->cache[i].img : NULL;
    if (f->cache_bytes > CACHE_BYTES_MAX) cache_clear(f); /* never mid-frame for what is already drawn */
    if (f->ncache == f->capcache) {
        uint32_t cap = f->capcache ? f->capcache * 2 : 64;
        cached *c = realloc(f->cache, cap * sizeof *c);
        if (!c) return NULL;
        f->cache = c;
        f->capcache = cap;
    }
    cached *c = &f->cache[f->ncache];
    size_t n = strlen(name);
    c->name = malloc(n + 1);
    if (!c->name) return NULL;
    memcpy(c->name, name, n + 1);
    decode(f, name, &c->img);
    f->cache_bytes += (size_t)c->img.width * c->img.height * 4u + strlen(name);
    f->ncache++;
    return c->img.argb ? &c->img : NULL;
}

static char *copy(const char *s) {
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

/* Sounds no voice plays are freed in place (the mixer holds pointers into the table, so slots
 * never move): all of them, or only while the cache is over its cap. */
static void sounds_trim(wolf_frame *f, int all_unused) {
    for (uint32_t i = 0; i < f->nsounds; i++) {
        sound *s = &f->sounds[i];
        if (!s->name) continue;
        if (!all_unused && f->sound_bytes <= SOUND_BYTES_MAX) break;
        if (wolf_mixer_playing(f->mixer, &s->pcm)) continue;
        f->sound_bytes -= (size_t)s->pcm.frames * 4u;
        free((void *)s->pcm.stereo);
        free(s->name);
        memset(s, 0, sizeof *s);
    }
}

static const wolf_pcm *sound_for(wolf_frame *f, const char *name) {
    for (uint32_t i = 0; i < f->nsounds; i++)
        if (f->sounds[i].name && strcmp(f->sounds[i].name, name) == 0)
            return f->sounds[i].pcm.stereo ? &f->sounds[i].pcm : NULL;
    if (f->sound_bytes > SOUND_BYTES_MAX) sounds_trim(f, 0);
    if (!f->sounds) {
        f->sounds = calloc(SOUNDS_MAX, sizeof *f->sounds); /* fixed size: slots never move */
        if (!f->sounds) return NULL;
    }
    sound *s = NULL;
    for (uint32_t i = 0; i < f->nsounds && !s; i++)
        if (!f->sounds[i].name) s = &f->sounds[i];
    if (!s && f->nsounds == SOUNDS_MAX) {
        sounds_trim(f, 1);
        for (uint32_t i = 0; i < f->nsounds && !s; i++)
            if (!f->sounds[i].name) s = &f->sounds[i];
    }
    if (!s) {
        if (f->nsounds == SOUNDS_MAX) return NULL;
        s = &f->sounds[f->nsounds++];
    }
    s->name = copy(name);
    if (!s->name) return NULL;
    uint8_t *data;
    size_t len;
    if (wolf_files_read(f->files, name, &data, &len) == 0) {
        int r = wolf_decode_sound(name, data, len, &s->pcm);
        free(data);
        if (r == WOLF_DECODE_UNSUPPORTED) wolf_vm_log(f->vm, WOLF_LOG_WARN, "sound \"%s\" is in a format not decoded yet", name);
        else if (r != WOLF_DECODE_OK) wolf_vm_log(f->vm, WOLF_LOG_WARN, "sound \"%s\" could not be decoded", name);
    }
    f->sound_bytes += (size_t)s->pcm.frames * 4u;
    return s->pcm.stereo ? &s->pcm : NULL;
}

/* The core's 140, played through the mixer. */
static void on_sound(void *ctx, const wolf_sound *r) {
    wolf_frame *f = ctx;
    switch (r->action) {
    case WOLF_SOUND_STOP: wolf_mixer_stop(f->mixer, r); break;
    case WOLF_SOUND_FREE_UNUSED: sounds_trim(f, 1); break;
    case WOLF_SOUND_PRELOAD: sound_for(f, r->file); break;
    default: {
        const wolf_pcm *pcm = sound_for(f, r->file);
        if (pcm) wolf_mixer_play(f->mixer, r, pcm);
        break;
    }
    }
}

void wolf_frame_audio(wolf_frame *f, int16_t *out, uint32_t frames) {
    if (f->mixer) wolf_mixer_mix(f->mixer, out, frames);
    else memset(out, 0, (size_t)frames * 4u);
}

wolf_frame *wolf_frame_start(const char *folder, wolf_open_game *g, wolf_vm *vm, uint32_t audio_rate) {
    wolf_frame *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->g = g;
    f->vm = vm;
    f->files = wolf_files_open(folder);
    if (!f->files || wolf_commonrun_init(&f->run, vm, &g->commons) != WOLF_COMMONRUN_OK) {
        wolf_frame_stop(f);
        return NULL;
    }
    if (audio_rate) {
        f->mixer = wolf_mixer_create(audio_rate);
        if (!f->mixer) {
            wolf_frame_stop(f);
            return NULL;
        }
        wolf_audio audio = {f, on_sound};
        wolf_vm_set_audio(vm, &audio);
    }
    return f;
}

void wolf_frame_step(wolf_frame *f, uint32_t *pixels) {
    wolf_commonrun_step(&f->run);
    wolf_vm_pictures_tick(f->vm);
    uint32_t w = f->g->width, h = f->g->height;
    for (size_t i = 0; i < (size_t)w * h; i++) pixels[i] = 0xFF000000u;
    wolf_render_source src = {f, image};
    wolf_render_pictures(f->vm, &src, pixels, w, h, INT32_MIN, INT32_MAX);
}

void wolf_frame_stop(wolf_frame *f) {
    if (!f) return;
    if (f->mixer) wolf_vm_set_audio(f->vm, NULL);
    wolf_mixer_destroy(f->mixer);
    for (uint32_t i = 0; i < f->nsounds; i++) {
        free((void *)f->sounds[i].pcm.stereo);
        free(f->sounds[i].name);
    }
    free(f->sounds);
    wolf_commonrun_free(&f->run);
    cache_clear(f);
    free(f->cache);
    wolf_files_close(f->files);
    free(f);
}
