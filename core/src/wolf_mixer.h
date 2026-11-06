/*
 * Mixing BGM, BGS and sound effects into one stereo stream. The plugin decodes the files; the
 * mixer plays them as 140 asks (docs/behaviour/sound.md). Frames of the game are 1/60 s; fades
 * are given in game frames.
 */
#ifndef ENGINEHOST_WOLF_MIXER_H
#define ENGINEHOST_WOLF_MIXER_H

#include <stdint.h>

#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded sound: interleaved stereo int16 at rate. The plugin keeps it alive while any voice may
 * play it (until wolf_mixer_forget). */
typedef struct {
    uint32_t rate;
    uint32_t frames;
    const int16_t *stereo;
} wolf_pcm;

#define WOLF_MIXER_SE_VOICES 32

typedef struct wolf_mixer wolf_mixer;

/* rate: the output rate (the host's audio rate). NULL when out of memory. */
wolf_mixer *wolf_mixer_create(uint32_t rate);
void wolf_mixer_destroy(wolf_mixer *m);
/* Plays pcm as kind (WOLF_SOUND_BGM, _BGS, _SE) with request's volume, frequency, fade and start.
 * BGM and BGS loop and replace what was playing; the same sound asked again only takes the new
 * volume and frequency. SEs play once, up to WOLF_MIXER_SE_VOICES at a time (the oldest gives
 * way). */
void wolf_mixer_play(wolf_mixer *m, const wolf_sound *request, const wolf_pcm *pcm);
/* Stops kind, fading out over request->fade_frames (BGM, BGS); SE stops at once. */
void wolf_mixer_stop(wolf_mixer *m, const wolf_sound *request);
/* No voice plays pcm any more (before the plugin frees it). */
void wolf_mixer_forget(wolf_mixer *m, const wolf_pcm *pcm);
/* 1 while any voice plays pcm. */
int wolf_mixer_playing(const wolf_mixer *m, const wolf_pcm *pcm);
/* The next frames of output, interleaved stereo. */
void wolf_mixer_mix(wolf_mixer *m, int16_t *out, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif
