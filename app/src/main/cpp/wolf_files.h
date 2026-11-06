/*
 * A game's files by the names its commands use ("Picture/title.png", relative to Data/). The
 * runtime prefers a loose file, then the archive of that folder (Data/Picture.wolf), then
 * Data.wolf. Names that could leave the game folder are refused (docs/behaviour/risks.md).
 * Nothing here writes.
 */
#ifndef ENGINEHOST_WOLF_FILES_H
#define ENGINEHOST_WOLF_FILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wolf_files wolf_files;

/* folder: the game folder. NULL when out of memory. */
wolf_files *wolf_files_open(const char *folder);
void wolf_files_close(wolf_files *f);
/* 0 with a new buffer the caller frees; 1 when the game has no such file; -1 when the name is
 * refused or the file cannot be read. */
int wolf_files_read(wolf_files *f, const char *name, uint8_t **data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
