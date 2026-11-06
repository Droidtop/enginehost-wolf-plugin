/*
 * The GLES 2 renderer: draws the core's picture quads (vm.h, wolf_quad) on the GL thread of the
 * GLSurfaceView WolfPlugin attaches to Enginehost's display. Runs under allocate-then-clamp now
 * and through the Fibrewall GPU proxy later (Droidtop/tracker#858, #815). Every call here must be
 * made on that GL thread, with its context current.
 */
#ifndef ENGINEHOST_WOLF_GL_H
#define ENGINEHOST_WOLF_GL_H

#include <stdint.h>

#include "wolf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wolf_gl wolf_gl;

/* After the context is (re)created: everything GL is made again. NULL on failure (logged). */
wolf_gl *wolf_gl_create(uint32_t game_width, uint32_t game_height);
/* The view's size in pixels; the game's screen is scaled to fit, keeping its shape. */
void wolf_gl_resize(wolf_gl *gl, uint32_t view_width, uint32_t view_height);
/* Draws f's pictures. */
void wolf_gl_draw(wolf_gl *gl, wolf_frame *f);
/* Frees the CPU side; GL objects go with the context. */
void wolf_gl_destroy(wolf_gl *gl);

#ifdef __cplusplus
}
#endif

#endif
