/*
 * Opening a Wolf game folder: finds its project data (unpacked under Data/BasicData, or packed
 * in Data.wolf or Data/BasicData.wolf), loads Game.dat, CommonEvent.dat and the three
 * databases with the core's loaders, and says why in plain words when it cannot.
 *
 * Every file is opened with the ordinary libc calls; in Enginehost's sandbox those go through
 * the host's file layer (plugin-native/enginehost_vfs.h), so this file never sees anything the
 * host did not hand it. Nothing here writes to the game folder.
 */
#ifndef ENGINEHOST_WOLF_OPEN_H
#define ENGINEHOST_WOLF_OPEN_H

#include <stddef.h>
#include <stdint.h>

#include "wolf_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The three databases, in the order the editor lists them. */
enum { WOLF_OPEN_DB_USER, WOLF_OPEN_DB_CHANGEABLE, WOLF_OPEN_DB_SYSTEM, WOLF_OPEN_DB_COUNT };

typedef struct {
    char source[64];  /* where the project data came from, for the log: "Data/BasicData", "Data.wolf", ... */
    wolf_game game;
    wolf_commons commons;
    wolf_db db[WOLF_OPEN_DB_COUNT];
    uint32_t width, height;
} wolf_open_game;

typedef void (*wolf_open_log_fn)(void *ctx, const char *line);

/*
 * Loads the game in folder. Returns 0 and fills *g, or -1 with a plain sentence for the person
 * in why (the game is refused, or its data is missing or not in a form this core reads).
 * log, when not NULL, gets one line per step: what was found and what each file held.
 */
int wolf_open(const char *folder, wolf_open_game *g, char *why, size_t why_size, wolf_open_log_fn log,
              void *log_ctx);
void wolf_open_free(wolf_open_game *g);

/* Shared with wolf_files.c. The child of dir called name in any case, of kind S_IFREG or
 * S_IFDIR, written to out: 0, or -1 when there is none. */
int wolf_find_ci(const char *dir, const char *name, unsigned kind, char *out, size_t out_size);
/* A whole file into a new buffer (capped at WOLF_READ_UNPACKED_MAX): 0, or an errno. */
int wolf_slurp(const char *path, uint8_t **data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
