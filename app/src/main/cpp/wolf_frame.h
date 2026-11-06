/*
 * One game's frames: each step runs the self-starting common events and moves the pictures on;
 * the pictures are then drawn by the GLES renderer (wolf_gl.c) or in software. Sound requests
 * play through the core's mixer, which another thread may drain (wolf_frame_audio is safe to
 * call from an audio callback). No JNI here, so a host build can run it on a game folder.
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
/* One game frame (1/60 s): events and pictures move on. */
void wolf_frame_run(wolf_frame *f);
/* The pictures as quads, lowest number first (vm.h, wolf_picture_quads). */
void wolf_frame_quads(wolf_frame *f, wolf_quad_fn fn, void *ctx);
/* The pictures drawn in software into pixels (width * height, 0xFFRRGGBB), the game's screen size. */
void wolf_frame_draw(wolf_frame *f, uint32_t *pixels);
/* wolf_frame_run, then wolf_frame_draw: the isolated, step-driven path. */
void wolf_frame_step(wolf_frame *f, uint32_t *pixels);
/* The next frames of sound (interleaved stereo int16); silence without sound. */
void wolf_frame_audio(wolf_frame *f, int16_t *out, uint32_t frames);
void wolf_frame_stop(wolf_frame *f);

#ifdef __cplusplus
}
#endif

#endif
