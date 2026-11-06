/*
 * One game's frames: each step runs the self-starting common events, moves the pictures on and
 * draws them into a frame. No JNI here, so a host build can run it on a game folder.
 */
#ifndef ENGINEHOST_WOLF_FRAME_H
#define ENGINEHOST_WOLF_FRAME_H

#include <stdint.h>

#include "vm.h"
#include "wolf_open.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wolf_frame wolf_frame;

/* g and vm must outlive the frame loop; vm was made for g (common events and databases set).
 * audio_rate: the output sample rate, or 0 for no sound. NULL when out of memory. */
wolf_frame *wolf_frame_start(const char *folder, wolf_open_game *g, wolf_vm *vm, uint32_t audio_rate);
/* One frame into pixels (width * height, 0xFFRRGGBB), the game's screen size. */
void wolf_frame_step(wolf_frame *f, uint32_t *pixels);
/* The next frames of sound (interleaved stereo int16); silence without sound. */
void wolf_frame_audio(wolf_frame *f, int16_t *out, uint32_t frames);
void wolf_frame_stop(wolf_frame *f);

#ifdef __cplusplus
}
#endif

#endif
