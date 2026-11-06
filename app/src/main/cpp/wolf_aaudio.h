/*
 * Sound out through AAudio (NDK, API 26), for when the core runs in Enginehost's runtime process
 * with a display: the host's audio ring is only handed to isolated cores. The stream's callback
 * pulls from the frame's mixer (wolf_frame_audio).
 */
#ifndef ENGINEHOST_WOLF_AAUDIO_H
#define ENGINEHOST_WOLF_AAUDIO_H

#include <stdint.h>

#include "wolf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wolf_aaudio wolf_aaudio;

/* Opens a stereo 16-bit output stream at the device's own rate. NULL when there is none (the
 * game then runs silent). */
wolf_aaudio *wolf_aaudio_open(void);
/* The stream's sample rate, for the mixer. */
uint32_t wolf_aaudio_rate(const wolf_aaudio *a);
/* Starts pulling from f; stop with wolf_aaudio_close before f goes. */
int wolf_aaudio_start(wolf_aaudio *a, wolf_frame *f);
void wolf_aaudio_pause(wolf_aaudio *a, int paused);
void wolf_aaudio_close(wolf_aaudio *a);

#ifdef __cplusplus
}
#endif

#endif
