/*
 * Sound files to PCM for the core's mixer: Ogg Vorbis (stb_vorbis, from the vendor/stb submodule
 * the images use) and WAV (8- or 16-bit PCM, read here). MP3 and MIDI are not decoded yet.
 */
#ifndef ENGINEHOST_WOLF_DECODE_H
#define ENGINEHOST_WOLF_DECODE_H

#include <stddef.h>
#include <stdint.h>

#include "wolf_mixer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOLF_DECODE_FRAMES_MAX (48000u * 60u * 15u) /* 15 minutes at 48 kHz (ours) */

enum { WOLF_DECODE_OK = 0, WOLF_DECODE_UNSUPPORTED = 1, WOLF_DECODE_BAD = -1 };

/* Decodes data (the file called name; the extension picks the decoder) into out, whose stereo
 * buffer the caller frees with free(). */
int wolf_decode_sound(const char *name, const uint8_t *data, size_t len, wolf_pcm *out);

#ifdef __cplusplus
}
#endif

#endif
