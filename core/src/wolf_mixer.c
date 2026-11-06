/* See wolf_mixer.h; the rules and their sources are in docs/behaviour/sound.md. */
#include "wolf_mixer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GAME_FPS 60.0

typedef struct {
    const wolf_pcm *pcm; /* NULL: silent */
    double pos;          /* in the sound's own frames */
    double step;         /* source frames per output frame: rate ratio times frequency */
    double volume;       /* 0..1+ */
    double gain;         /* the fade: 0..1 */
    double fade_by;      /* change in gain per output frame (0: none) */
    int loop;
    int stop_at_silence; /* a fade-out: the voice ends when the gain reaches 0 */
    uint64_t started;    /* for the oldest-first SE rule */
} voice;

struct wolf_mixer {
    uint32_t rate;
    voice bgm, bgs;
    voice se[WOLF_MIXER_SE_VOICES];
    uint64_t clock;
};

wolf_mixer *wolf_mixer_create(uint32_t rate) {
    wolf_mixer *m = calloc(1, sizeof *m);
    if (m) m->rate = rate ? rate : 44100;
    return m;
}

void wolf_mixer_destroy(wolf_mixer *m) { free(m); }

static double pct(int32_t v) { return v < 0 ? 0 : v > 1000 ? 10.0 : v / 100.0; }

/* Output frames for a fade of game frames. */
static double fade_len(const wolf_mixer *m, int32_t frames) { return frames > 0 ? frames / GAME_FPS * m->rate : 0; }

static void tune(const wolf_mixer *m, voice *v, const wolf_sound *r) {
    v->volume = pct(r->volume);
    double f = r->frequency > 0 ? r->frequency / 100.0 : 1.0;
    if (f < 0.1) f = 0.1;
    if (f > 4.0) f = 4.0; /* a cap of ours */
    v->step = (double)v->pcm->rate / (double)m->rate * f;
}

void wolf_mixer_play(wolf_mixer *m, const wolf_sound *r, const wolf_pcm *pcm) {
    if (!pcm || !pcm->stereo || !pcm->frames || !pcm->rate) return;
    voice *v;
    if (r->kind == WOLF_SOUND_SE) {
        v = NULL;
        for (int i = 0; i < WOLF_MIXER_SE_VOICES && !v; i++)
            if (!m->se[i].pcm) v = &m->se[i];
        if (!v) { /* the oldest gives way */
            v = &m->se[0];
            for (int i = 1; i < WOLF_MIXER_SE_VOICES; i++)
                if (m->se[i].started < v->started) v = &m->se[i];
        }
    } else {
        v = r->kind == WOLF_SOUND_BGS ? &m->bgs : &m->bgm;
        if (v->pcm == pcm && !v->stop_at_silence) { /* the same music: only the new settings (manual) */
            tune(m, v, r);
            return;
        }
    }
    memset(v, 0, sizeof *v);
    v->pcm = pcm;
    tune(m, v, r);
    v->loop = r->kind != WOLF_SOUND_SE;
    v->started = ++m->clock;
    if (r->kind != WOLF_SOUND_SE && r->start_ms > 0) {
        double at = (double)r->start_ms / 1000.0 * pcm->rate;
        v->pos = at < pcm->frames ? at : 0;
    }
    double len = r->kind == WOLF_SOUND_SE ? 0 : fade_len(m, r->fade_frames);
    v->gain = len > 0 ? 0 : 1;
    v->fade_by = len > 0 ? 1.0 / len : 0;
}

void wolf_mixer_stop(wolf_mixer *m, const wolf_sound *r) {
    if (r->kind == WOLF_SOUND_SE) {
        for (int i = 0; i < WOLF_MIXER_SE_VOICES; i++) m->se[i].pcm = NULL;
        return;
    }
    voice *v = r->kind == WOLF_SOUND_BGS ? &m->bgs : &m->bgm;
    if (!v->pcm) return;
    double len = fade_len(m, r->fade_frames);
    if (len <= 0) {
        v->pcm = NULL;
        return;
    }
    v->fade_by = -v->gain / len;
    v->stop_at_silence = 1;
}

void wolf_mixer_forget(wolf_mixer *m, const wolf_pcm *pcm) {
    if (m->bgm.pcm == pcm) m->bgm.pcm = NULL;
    if (m->bgs.pcm == pcm) m->bgs.pcm = NULL;
    for (int i = 0; i < WOLF_MIXER_SE_VOICES; i++)
        if (m->se[i].pcm == pcm) m->se[i].pcm = NULL;
}

int wolf_mixer_playing(const wolf_mixer *m, const wolf_pcm *pcm) {
    if (m->bgm.pcm == pcm || m->bgs.pcm == pcm) return 1;
    for (int i = 0; i < WOLF_MIXER_SE_VOICES; i++)
        if (m->se[i].pcm == pcm) return 1;
    return 0;
}

/* Adds one voice into acc (stereo, as doubles). */
static void add(voice *v, double *acc, uint32_t frames) {
    for (uint32_t i = 0; i < frames && v->pcm; i++) {
        const wolf_pcm *p = v->pcm;
        uint32_t a = (uint32_t)v->pos;
        if (a >= p->frames) {
            if (!v->loop) { v->pcm = NULL; break; }
            v->pos = fmod(v->pos, (double)p->frames);
            a = (uint32_t)v->pos;
            if (a >= p->frames) a = 0;
        }
        uint32_t b = a + 1 < p->frames ? a + 1 : (v->loop ? 0 : a);
        double t = v->pos - a, g = v->volume * v->gain;
        acc[i * 2] += ((1 - t) * p->stereo[a * 2] + t * p->stereo[b * 2]) * g;
        acc[i * 2 + 1] += ((1 - t) * p->stereo[a * 2 + 1] + t * p->stereo[b * 2 + 1]) * g;
        v->pos += v->step;
        if (v->fade_by != 0) {
            v->gain += v->fade_by;
            if (v->gain >= 1) { v->gain = 1; v->fade_by = 0; }
            if (v->gain <= 0) {
                v->gain = 0;
                v->fade_by = 0;
                if (v->stop_at_silence) v->pcm = NULL;
            }
        }
    }
}

void wolf_mixer_mix(wolf_mixer *m, int16_t *out, uint32_t frames) {
    enum { CHUNK = 512 };
    double acc[CHUNK * 2];
    while (frames) {
        uint32_t n = frames < CHUNK ? frames : CHUNK;
        memset(acc, 0, sizeof acc);
        add(&m->bgm, acc, n);
        add(&m->bgs, acc, n);
        for (int i = 0; i < WOLF_MIXER_SE_VOICES; i++) add(&m->se[i], acc, n);
        for (uint32_t i = 0; i < n * 2; i++) {
            double s = acc[i];
            out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        }
        out += n * 2;
        frames -= n;
    }
}
