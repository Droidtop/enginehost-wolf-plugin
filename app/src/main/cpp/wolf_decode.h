/*
 * Sound files to PCM for the core's mixer. All audio and video decoding belongs to Enginehost's
 * sandboxed FFmpeg decode service (the owner, Droidtop/tracker#906 and #858: "All video and audio
 * decoding should be handled by the ffmpeg bundle"). Until its API exists the core decodes
 * nothing itself: every request answers WOLF_DECODE_WAITING, and the game runs silent.
 */
#ifndef ENGINEHOST_WOLF_DECODE_H
#define ENGINEHOST_WOLF_DECODE_H

#include <stddef.h>
#include <stdint.h>

#include "wolf_mixer.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { WOLF_DECODE_OK = 0, WOLF_DECODE_WAITING = 1, WOLF_DECODE_BAD = -1 };

/* Will hand data (the file called name) to the decode service and fill out, whose stereo buffer
 * the caller frees with free(). Today: WOLF_DECODE_WAITING. */
int wolf_decode_sound(const char *name, const uint8_t *data, size_t len, wolf_pcm *out);

#ifdef __cplusplus
}
#endif

#endif
