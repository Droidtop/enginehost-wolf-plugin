/*
 * The font source the core's text layout asks for glyphs (vm.h, wolf_render_source.glyph).
 * Fonts are pluggable (Droidtop/tracker#815), in this order:
 *   1. the game's own: the .ttf, .otf and .ttc files beside Game.exe and in Data/, matched to
 *      Game.dat's font names (texts 3-6) by their name tables; a character the named font lacks
 *      comes from the game's other fonts;
 *   2. a font the core provides, and 3. a default set with CJK coverage: Enginehost's, not built
 *      yet, so a game with no fonts of its own draws no text (logged once).
 * Rasterising belongs in the confined asset worker with images (#815); until that exists it runs
 * here, with stb_truetype from the vendor/stb submodule.
 */
#ifndef ENGINEHOST_WOLF_FONTS_H
#define ENGINEHOST_WOLF_FONTS_H

#include <stdint.h>

#include "vm.h"
#include "wolf_open.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wolf_fonts wolf_fonts;

/* Finds the game's fonts in folder and matches them to g's Game.dat names. NULL when out of memory. */
wolf_fonts *wolf_fonts_open(const char *folder, const wolf_game *g, wolf_vm *log_to);
/* A glyph, or NULL when no font has it. Valid until wolf_fonts_frame or wolf_fonts_close. */
const wolf_glyph *wolf_fonts_glyph(wolf_fonts *f, uint32_t font, uint32_t size, uint32_t cp);
/* Between frames: past its cap, the glyph cache starts over (never while a frame is drawn). */
void wolf_fonts_frame(wolf_fonts *f);
void wolf_fonts_close(wolf_fonts *f);

#ifdef __cplusplus
}
#endif

#endif
